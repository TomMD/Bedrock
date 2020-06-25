#include <libstuff/libstuff.h>
#include "SQLiteNode.h"
#include "SQLiteServer.h"
#include "SQLiteCommand.h"

// A bit of a hack until I think of a better way to do this.
atomic<uint64_t> currentCommandThreadID(0);
class DecrementOnDestruction {
  public:
    DecrementOnDestruction(atomic<int64_t>& counter) : _counter(counter) {} 
    ~DecrementOnDestruction() {
        _counter--;
    }
  private:
    atomic<int64_t>& _counter;
};

// Introduction
// ------------
// SQLiteNode builds atop STCPNode and SQLite to provide a distributed transactional SQL database. The STCPNode base
// class establishes and maintains connections with all peers: if any connection fails, it forever attempts to
// re-establish. This frees the SQLiteNode layer to focus on the high-level distributed database state machine.
//
// FIXME: Handle the case where two nodes have conflicting databases. Should find where they fork, tag the affected
//        accounts for manual review, and adopt the higher-priority
//
// FIXME: Leader should detect whether any followers fall out of sync for any reason, identify/tag affected accounts, and
//        re-synchronize.
//
// FIXME: Add test to measure how long it takes for leader to stabilize.
//
// FIXME: If leader dies before sending ESCALATE_RESPONSE (or if follower dies before receiving it), then a command might
//        have been committed to the database without notifying whoever initiated it. Perhaps have the caller identify
//        each command with a unique command id, and verify inside the query that the command hasn't been executed yet?

#undef SLOGPREFIX
#define SLOGPREFIX "{" << name << "/" << SQLiteNode::stateName(_state) << "} "

// Initializations for static vars.
const uint64_t SQLiteNode::SQL_NODE_DEFAULT_RECV_TIMEOUT = STIME_US_PER_M * 5;
const uint64_t SQLiteNode::SQL_NODE_SYNCHRONIZING_RECV_TIMEOUT = STIME_US_PER_S * 30;
atomic<bool> SQLiteNode::unsentTransactions(false);
uint64_t SQLiteNode::_lastSentTransactionID = 0;

const string SQLiteNode::consistencyLevelNames[] = {"ASYNC",
                                                    "ONE",
                                                    "QUORUM"};

SQLiteNode::SQLiteNode(SQLiteServer& server, SQLite& db, const string& name,
                       const string& host, const string& peerList, int priority, uint64_t firstTimeout,
                       const string& version)
    : STCPNode(name, host, max(SQL_NODE_DEFAULT_RECV_TIMEOUT, SQL_NODE_SYNCHRONIZING_RECV_TIMEOUT)),
      _db(db),
      _commitState(CommitState::UNINITIALIZED),
      _server(server),
      _stateChangeCount(0),
      _lastNetStatTime(chrono::steady_clock::now()),
      _handledCommitCount(0),
     _replicationThreadsShouldExit(false)
    {

    // TODO: Remove and spawn these per thread.
    for (int i = 0; i < 8; i++) {
        _replicationDBs.emplace_back(db);
    }


    SASSERT(priority >= 0);
    _originalPriority = priority;
    _priority = -1;
    _state = SEARCHING;
    _syncPeer = nullptr;
    _leadPeer = nullptr;
    _stateTimeout = STimeNow() + firstTimeout;
    _version = version;

    // Get this party started
    _changeState(SEARCHING);

    // Add any peers.
    list<string> parsedPeerList = SParseList(peerList);
    for (const string& peer : parsedPeerList) {
        // Get the params from this peer, if any
        string host;
        STable params;
        SASSERT(SParseURIPath(peer, host, params));
        string name = SGetDomain(host);
        if (params.find("nodeName") != params.end()) {
            name = params["nodeName"];
        }
        addPeer(name, host, params);
    }
}

SQLiteNode::~SQLiteNode() {
    // Make sure it's a clean shutdown
    SASSERTWARN(_escalatedCommandMap.empty());
    SASSERTWARN(!commitInProgress());
}

// This is the main replication loop that's run in the replication threads. It pops commands off of the
// `_replicationCommands` queue, and handles them *in parallel* as we run multiple instances of this function
// simultaneously. It's important to note that we NEED to run this function in at least two threads or we run into a
// starvation issue where a thread that's performing a transaction won't ever be notified that the transaction can be
// committed.
//
// There are three commands we handle here BEGIN_TRANSACTION, ROLLBACK_TRANSACTION, and COMMIT_TRANSACTION.
// ROLLBACK_TRANSACTION and COMMIT_TRANSACTION are trivial, they record the hash of the transaction that is ready to be
// COMMIT'ted (or rolled back), and notify any other threads that are waiting for this info that they can continue.
//
// BEGIN_TRANSACTION is where the interesting case is. This waits for the DB to be up-to-date, which is to say, the
// commit count of the DB is one behind the new commit count of the transaction it's attempting to run.
// Once that happens, it runs `handleBeginTransaction()` to do the body of work of the transaction.
//
// Finally, it waits for the new hash for the transaction to be ready to either COMMIT or ROLLBACK, and once that's
// true, it performs the corresponding operation and notifies any other threads that are waiting on the DB to come
// up-to-date that the commit count in the DB has changed.
//
// This thread exits when node._replicationThreadsShouldExit is set, which happens when a node stops FOLLOWING.
void SQLiteNode::replicate(SQLiteNode& node, Peer* peer, SData command) {
    // Initialize each new thread with a new number.
    SInitialize("replicate" + to_string(currentCommandThreadID.fetch_add(1)));

    // Make sure when this thread exits we decrement our thread counter.
    DecrementOnDestruction dod(node._replicationThreads);

    // Get a DB handle to work on.
    SQLite db(node._db);

    // These make the logging macros work, as they expect these variables to be in scope.
    auto _state = node._state.load();
    string name = node.name;

    if (SIEquals(command.methodLine, "BEGIN_TRANSACTION")) {
        try {
            while (true) {
                unique_lock<mutex> lock(node._replicationMutex);
                if (node._replicationThreadsShouldExit) {
                    return;
                }

                // Wait for the DB to come up to date.
                if (command.calcU64("NewCount") == db.getCommitCount() + 1) {
                    // We can unlock once we know our condition has passed, there's no race in case it changes after
                    // we've checked it but before we wait again, as we wont wait. We do this before any DB operations
                    // so that waiting on the DB can't block enqueueing new commands. But it's important that we do
                    // hold this lock before checking the conditions that determine if we can proceed with our DB
                    // operations.
                    lock.unlock();

                    // Importantly, we don't start our transaction until the previous transaction has fully completed.
                    // In the next version, where we handle concurrent transactions, this will need to be able to start
                    // regardless of whether the previous transaction has started.
                    node.handleBeginTransaction(db, peer, command);
                    break;
                } else {
                    // Wait and then start from the beginning.
                    node._replicationCV.wait(lock);
                }
            }

            // Wait for a COMMIT or ROLLBACK.
            while (true) {
                unique_lock<mutex> lock(node._replicationMutex);
                if (node._replicationThreadsShouldExit) {
                    db.rollback();
                    return;
                }

                // Look up our hashes to see if we can COMMIT or ROLLBACK.
                bool commit = false;
                bool rollback = false;
                {
                    lock_guard<mutex> hashLock(node._replicationHashMutex);
                    commit = node._replicationHashesToCommit.count(command["NewHash"]);
                    rollback = node._replicationHashesToRollback.count(command["NewHash"]);
                }

                // If we can't do either, keep waiting.
                if (!commit && !rollback) {
                    node._replicationCV.wait(lock);
                } else {
                    // Otherwise, we can either commit, or rollback. First we unlock so that we don't block other
                    // threads on the DB operation.
                    lock.unlock();

                    // Do the appropriate DB operation.
                    commit ? node.handleCommitTransaction(db, peer, command.calcU64("NewCount"), command["NewHash"]) : node.handleRollbackTransaction(db, peer, command);

                    // And clean up.
                    {
                        lock_guard<mutex> hashLock(node._replicationHashMutex);
                        commit ? node._replicationHashesToCommit.erase(command["NewHash"]) : node._replicationHashesToRollback.erase(command["NewHash"]);
                    }

                    // Let any threads waiting on the DB to be up-to-date know that the state has changed.
                    node._replicationCV.notify_all();
                    break;
                }
            }
        } catch (const SException& e) {
            SALERT("Caught exception in replication thread. Assuming this means we want to stop following. Exception: " << e.what());
            db.rollback();
            return;
        }
    } else if (SIEquals(command.methodLine, "ROLLBACK_TRANSACTION")) {
        lock_guard<mutex> hashLock(node._replicationHashMutex);
        node._replicationHashesToRollback.insert(command["NewHash"]);
        node._replicationCV.notify_all();
    } else if (SIEquals(command.methodLine, "COMMIT_TRANSACTION")) {
        lock_guard<mutex> hashLock(node._replicationHashMutex);
        node._replicationHashesToCommit.insert(command["Hash"]);
        node._replicationCV.notify_all();
    }

    // Create a dummy placeholder command.
    pair<Peer*, SData> commandWithPeer(nullptr, SData(""));
}

void SQLiteNode::startCommit(ConsistencyLevel consistency)
{
    // Verify we're not already committing something, and then record that we have begun. This doesn't actually *do*
    // anything, but `update()` will pick up the state in its next invocation and start the actual commit.
    SASSERT(_commitState == CommitState::UNINITIALIZED ||
            _commitState == CommitState::SUCCESS       ||
            _commitState == CommitState::FAILED);
    _commitState = CommitState::WAITING;
    _commitConsistency = consistency;
}

void SQLiteNode::sendResponse(const SQLiteCommand& command)
{
    Peer* peer = getPeerByID(command.initiatingPeerID);
    SASSERT(peer);
    // If it was a peer message, we don't need to wrap it in an escalation response.
    SData escalate("ESCALATE_RESPONSE");
    escalate["ID"] = command.id;
    escalate.content = command.response.serialize();
    SINFO("Sending ESCALATE_RESPONSE to " << peer->name << " for " << command.id << ".");
    _sendToPeer(peer, escalate);
}

void SQLiteNode::beginShutdown(uint64_t usToWait) {
    // Ignore redundant
    if (!gracefulShutdown()) {
        // Start graceful shutdown
        SINFO("Beginning graceful shutdown.");
        _gracefulShutdownTimeout.alarmDuration = usToWait;
        _gracefulShutdownTimeout.start();
    }
}

bool SQLiteNode::_isNothingBlockingShutdown() {
    // Don't shutdown if in the middle of a transaction
    if (_db.insideTransaction())
        return false;

    // If we're doing a commit, don't shut down.
    if (commitInProgress()) {
        return false;
    }

    // If we have non-"Connection: wait" commands escalated to leader, not done
    if (!_escalatedCommandMap.empty()) {
        return false;
    }

    return true;
}

bool SQLiteNode::shutdownComplete() {
    // First even see if we're shutting down
    if (!gracefulShutdown())
        return false;

    // Next, see if we're timing out the graceful shutdown and killing non-gracefully
    if (_gracefulShutdownTimeout.ringing()) {
        SWARN("Graceful shutdown timed out, killing non gracefully.");
        if (_escalatedCommandMap.size()) {
            SWARN("Abandoned " << _escalatedCommandMap.size() << " escalated commands.");
            for (auto& commandPair : _escalatedCommandMap) {
                commandPair.second->response.methodLine = "500 Abandoned";
                commandPair.second->complete = true;
                _server.acceptCommand(move(commandPair.second), false);
            }
            _escalatedCommandMap.clear();
        }
        _changeState(SEARCHING);
        return true;
    }

    // Not complete unless we're SEARCHING, SYNCHRONIZING, or WAITING
    if (_state > WAITING) {
        // Not in a shutdown state
        SINFO("Can't graceful shutdown yet because state="
              << stateName(_state) << ", commitInProgress=" << commitInProgress()
              << ", escalated=" << _escalatedCommandMap.size());

        // If we end up with anything left in the escalated command map when we're trying to shut down, let's log it,
        // so we can try and diagnose what's happening.
        if (!_escalatedCommandMap.empty()) {
            for (auto& cmd : _escalatedCommandMap) {
                string name = cmd.first;
                unique_ptr<SQLiteCommand>& command = cmd.second;
                int64_t created = command->request.calcU64("commandExecuteTime");
                int64_t elapsed = STimeNow() - created;
                double elapsedSeconds = (double)elapsed / STIME_US_PER_S;
                SINFO("Escalated command remaining at shutdown(" << name << "): " << command->request.methodLine
                      << ". Created: " << command->request["commandExecuteTime"] << " (" << elapsedSeconds << "s ago)");
            }
        }
        return false;
    }

    // If we have unsent data, not done
    for (auto peer : peerList) {
        if (peer->s && !peer->s->sendBufferEmpty()) {
            // Still sending data
            SINFO("Can't graceful shutdown yet because unsent data to peer '" << peer->name << "'");
            return false;
        }
    }

    // Finally, make sure nothing is blocking shutdown
    if (_isNothingBlockingShutdown()) {
        // Yes!
        SINFO("Graceful shutdown is complete");
        return true;
    } else {
        // Not done yet
        SINFO("Can't graceful shutdown yet because waiting on commands: commitInProgress="
              << commitInProgress() << ", escalated=" << _escalatedCommandMap.size());
        return false;
    }
}

void SQLiteNode::_sendOutstandingTransactions() {
    SQLITE_COMMIT_AUTOLOCK;

    // Make sure we have something to do.
    if (!unsentTransactions.load()) {
        return;
    }
    auto transactions = _db.getCommittedTransactions();
    string sendTime = to_string(STimeNow());
    for (auto& i : transactions) {
        uint64_t id = i.first;
        if (id <= _lastSentTransactionID) {
            continue;
        }
        string& query = i.second.first;
        string& hash = i.second.second;
        SData transaction("BEGIN_TRANSACTION");
        transaction["Command"] = "ASYNC";
        transaction["NewCount"] = to_string(id);
        transaction["NewHash"] = hash;
        transaction["leaderSendTime"] = sendTime;
        transaction["ID"] = "ASYNC_" + to_string(id);
        transaction.content = query;
        _sendToAllPeers(transaction, true); // subscribed only
        for (auto peer : peerList) {
            // Clear the response flag from the last transaction
            (*peer)["TransactionResponse"].clear();
        }
        SData commit("COMMIT_TRANSACTION");
        commit["ID"] = transaction["ID"];
        commit["CommitCount"] = transaction["NewCount"];
        commit["Hash"] = hash;
        _sendToAllPeers(commit, true); // subscribed only
        _lastSentTransactionID = id;
    }
    unsentTransactions.store(false);
}

void SQLiteNode::escalateCommand(unique_ptr<SQLiteCommand>&& command, bool forget) {
    lock_guard<mutex> leadPeerLock(_leadPeerMutex);
    // Send this to the leader
    SASSERT(_leadPeer);

    // If the leader is currently standing down, we won't escalate, we'll give the command back to the caller.
    if(_leadPeer.load()->state == STANDINGDOWN) {
        SINFO("Asked to escalate command but leader standing down, letting server retry.");
        _server.acceptCommand(move(command), false);
        return;
    }

    SASSERTEQUALS(_leadPeer.load()->state, LEADING);
    uint64_t elapsed = STimeNow() - command->request.calcU64("commandExecuteTime");
    SINFO("Escalating '" << command->request.methodLine << "' (" << command->id << ") to leader '" << _leadPeer.load()->name
          << "' after " << elapsed / 1000 << " ms");

    // Create a command to send to our leader.
    SData escalate("ESCALATE");
    escalate["ID"] = command->id;
    escalate.content = command->request.serialize();

    // Marking the command as escalated, even if we are going to forget it, because the command's destructor may need
    // this info.
    command->escalated = true;

    // Store the command as escalated, unless we intend to forget about it anyway.
    if (forget) {
        SINFO("Firing and forgetting command '" << command->request.methodLine << "' to leader.");
    } else {
        command->escalationTimeUS = STimeNow();
        _escalatedCommandMap.emplace(command->id, move(command));
    }

    // And send to leader.
    _sendToPeer(_leadPeer, escalate);
}

list<string> SQLiteNode::getEscalatedCommandRequestMethodLines() {
    list<string> returnList;
    for (auto& commandPair : _escalatedCommandMap) {
        returnList.push_back(commandPair.second->request.methodLine);
    }
    return returnList;
}

// --------------------------------------------------------------------------
// State Machine
// --------------------------------------------------------------------------
// Here is a simplified state diagram showing the major state transitions:
//
//                              SEARCHING
//                                  |
//                            SYNCHRONIZING
//                                  |
//                               WAITING
//                    ___________/     \____________
//                   |                              |
//              STANDINGUP                     SUBSCRIBING
//                   |                              |
//                LEADING                       FOLLOWING
//                   |                              |
//             STANDINGDOWN                         |
//                   |___________       ____________|
//                               \     /
//                              SEARCHING
//
// In short, every node starts out in the SEARCHING state, where it simply tries
// to establish all its peer connections.  Once done, each node SYNCHRONIZES with
// the freshest peer, meaning they download whatever "commits" they are
// missing.  Then they WAIT until the highest priority node "stands up" to become
// the new "leader".  All other nodes then SUBSCRIBE and become "followers".  If the
// leader "stands down", then all followers unsubscribe and everybody goes back into
// the SEARCHING state and tries it all over again.
//
//
// State Transitions
// -----------------
// Each state transitions according to the following events and operates as follows:
bool SQLiteNode::update() {

    // Log network timing info.
    auto now = chrono::steady_clock::now();
    if (now > (_lastNetStatTime + 10s)) {
        auto elapsed = (now - _lastNetStatTime);
        _lastNetStatTime = now;
        string logMsg = "[performance] Network stats: " +
                        to_string(chrono::duration_cast<chrono::milliseconds>(elapsed).count()) +
                        " ms elapsed. ";
        for (auto& p : peerList) {
            if (p->s) {
                logMsg += p->name + " sent " + to_string(p->s->getSentBytes()) + " bytes, recv " + to_string(p->s->getRecvBytes()) + " bytes. ";
                p->s->resetCounters();
            } else {
                logMsg += p->name + " has no socket. ";
            }
        }
        SINFO(logMsg);
    }

    // Process the database state machine
    switch (_state) {
    /// - SEARCHING: Wait for a period and try to connect to all known
    ///     peers.  After a timeout, give up and go ahead with whoever
    ///     we were able to successfully connect to -- if anyone.  The
    ///     logic for this state is as follows:
    ///
    ///         if( no peers configured )             goto LEADING
    ///         if( !timeout )                        keep waiting
    ///         if( no peers connected )              goto LEADING
    ///         if( nobody has more commits than us ) goto WAITING
    ///         else send SYNCHRONIZE and goto SYNCHRONIZING
    ///
    case SEARCHING: {
        SASSERTWARN(!_syncPeer);
        SASSERTWARN(!_leadPeer);
        SASSERTWARN(_db.getUncommittedHash().empty());
        // If we're trying to shut down, just do nothing
        if (shutdownComplete())
            return false; // Don't re-update

        // If no peers, we're the leader, unless we're shutting down.
        if (peerList.empty()) {
            // There are no peers, jump straight to leading
            SHMMM("No peers configured, jumping to LEADING");
            _changeState(LEADING);
            _leaderVersion = _version;
            return true; // Re-update immediately
        }

        // How many peers have we logged in to?
        int numFullPeers = 0;
        int numLoggedInFullPeers = 0;
        Peer* freshestPeer = nullptr;
        for (auto peer : peerList) {
            // Wait until all connected (or failed) and logged in
            bool permaFollower = peer->params["Permafollower"] == "true";
            bool loggedIn = peer->test("LoggedIn");

            // Count how many full peers (non-permafollowers) we have
            numFullPeers += !permaFollower;

            // Count how many full peers are logged in
            numLoggedInFullPeers += (!permaFollower) && loggedIn;

            // Find the freshest peer
            if (loggedIn) {
                // The freshest peer is the one that has the most commits.
                if (!freshestPeer || peer->calcU64("CommitCount") > freshestPeer->calcU64("CommitCount")) {
                    freshestPeer = peer;
                }
            }
        }

        // Keep searching until we connect to at least half our non-permafollowers peers OR timeout
        SINFO("Signed in to " << numLoggedInFullPeers << " of " << numFullPeers << " full peers (" << peerList.size()
                              << " with permafollowers), timeout in " << (_stateTimeout - STimeNow()) / 1000
                              << "ms");
        if (((float)numLoggedInFullPeers < numFullPeers / 2.0) && (STimeNow() < _stateTimeout))
            return false;

        // We've given up searching; did we time out?
        if (STimeNow() >= _stateTimeout)
            SHMMM("Timeout SEARCHING for peers, continuing.");

        // If no freshest (not connected to anyone), wait
        if (!freshestPeer) {
            // Unable to connect to anyone
            SHMMM("Unable to connect to any peer, WAITING.");
            _changeState(WAITING);
            return true; // Re-update
        }

        // How does our state compare with the freshest peer?
        SASSERT(freshestPeer);
        uint64_t freshestPeerCommitCount = freshestPeer->calcU64("CommitCount");
        if (freshestPeerCommitCount == _db.getCommitCount()) {
            // We're up to date
            SINFO("Synchronized with the freshest peer '" << freshestPeer->name << "', WAITING.");
            _changeState(WAITING);
            return true; // Re-update
        }

        // Are we fresher than the freshest peer?
        if (freshestPeerCommitCount < _db.getCommitCount()) {
            // Looks like we're the freshest peer overall
            SINFO("We're the freshest peer, WAITING.");
            _changeState(WAITING);
            return true; // Re-update
        }

        // It has a higher commit count than us, synchronize.
        SASSERT(freshestPeerCommitCount > _db.getCommitCount());
        SASSERTWARN(!_syncPeer);
        _updateSyncPeer();
        if (_syncPeer) {
            _sendToPeer(_syncPeer, SData("SYNCHRONIZE"));
        } else {
            SWARN("Updated to NULL _syncPeer when about to send SYNCHRONIZE. Going to WAITING.");
            _changeState(WAITING);
            return true; // Re-update
        }
        _changeState(SYNCHRONIZING);
        return true; // Re-update
    }

    /// - SYNCHRONIZING: We only stay in this state while waiting for
    ///     the SYNCHRONIZE_RESPONSE.  When we receive it, we'll enter
    ///     the WAITING state.  Alternately, give up waitng after a
    ///     period and go SEARCHING.
    ///
    case SYNCHRONIZING: {
        SASSERTWARN(_syncPeer);
        SASSERTWARN(!_leadPeer);
        SASSERTWARN(_db.getUncommittedHash().empty());
        // Nothing to do but wait
        if (STimeNow() > _stateTimeout) {
            // Give up on synchronization; reconnect that peer and go searching
            SHMMM("Timed out while waiting for SYNCHRONIZE_RESPONSE, searching.");
            _reconnectPeer(_syncPeer);
            _syncPeer = nullptr;
            _changeState(SEARCHING);
            return true; // Re-update
        }
        break;
    }

    /// - WAITING: As the name implies, wait until something happens.  The
    ///     logic for this state is as follows:
    ///
    ///         loop across "LoggedIn" peers to find the following:
    ///             - freshest peer (most commits)
    ///             - highest priority peer
    ///             - current leader (might be STANDINGUP or STANDINGDOWN)
    ///         if( no peers logged in )
    ///             goto SEARCHING
    ///         if( a higher-priority LEADING leader exists )
    ///             send SUBSCRIBE and go SUBSCRIBING
    ///         if( the freshest peer has more commits han us )
    ///             goto SEARCHING
    ///         if( no leader and we're the highest prioriy )
    ///             clear "StandupResponse" on all peers
    ///             goto STANDINGUP
    ///
    case WAITING: {
        SASSERTWARN(!_syncPeer);
        SASSERTWARN(!_leadPeer);
        SASSERTWARN(_db.getUncommittedHash().empty());
        SASSERTWARN(_escalatedCommandMap.empty());
        // If we're trying and ready to shut down, do nothing.
        if (gracefulShutdown()) {
            // Do we have an outstanding command?
            if (1/* TODO: Commit in progress? */) {
                // Nope!  Let's just halt the FSM here until we shutdown so as to
                // avoid potential confusion.  (Technically it would be fine to continue
                // the FSM, but it makes the logs clearer to just stop here.)
                SINFO("Graceful shutdown underway and no queued commands, do nothing.");
                return false; // No fast update
            } else {
                // We do have outstanding commands, even though a graceful shutdown
                // has been requested.  This is probably due to us previously being a leader
                // to which commands had been sent directly -- we got the signal to shutdown,
                // and stood down immediately.  All the followers will re-escalate whatever
                // commands they were waiting on us to process, so they're fine.  But our own
                // commands still need to be processed.  We're no longer the leader, so we
                // can't do it.  Rather, even though we're trying to do a graceful shutdown,
                // we need to find and follower to the new leader, and have it process our
                // commands.  Once the new leader has processed our commands, then we can
                // shut down gracefully.
                SHMMM("Graceful shutdown underway but queued commands so continuing...");
            }
        }

        // Loop across peers and find the highest priority and leader
        int numFullPeers = 0;
        int numLoggedInFullPeers = 0;
        Peer* highestPriorityPeer = nullptr;
        Peer* freshestPeer = nullptr;
        Peer* currentLeader = nullptr;
        for (auto peer : peerList) {
            // Make sure we're a full peer
            if (peer->params["Permafollower"] != "true") {
                // Verify we're logged in
                ++numFullPeers;
                if (SIEquals((*peer)["LoggedIn"], "true")) {
                    // Verify we're still fresh
                    ++numLoggedInFullPeers;
                    if (!freshestPeer || peer->calcU64("CommitCount") > freshestPeer->calcU64("CommitCount"))
                        freshestPeer = peer;

                    // See if it's the highest priority
                    if (!highestPriorityPeer || peer->calc("Priority") > highestPriorityPeer->calc("Priority"))
                        highestPriorityPeer = peer;

                    // See if it is currently the leader (or standing up/down)
                    if (peer->state == STANDINGUP || peer->state == LEADING || peer->state == STANDINGDOWN) {
                        // Found the current leader
                        if (currentLeader)
                            PHMMM("Multiple peers trying to stand up (also '" << currentLeader->name
                                                                              << "'), let's hope they sort it out.");
                        currentLeader = peer;
                    }
                }
            }
        }

        // If there are no logged in peers, then go back to SEARCHING.
        if (!highestPriorityPeer) {
            // Not connected to any other peers
            SHMMM("Configured to have peers but can't connect to any, re-SEARCHING.");
            _changeState(SEARCHING);
            return true; // Re-update
        }
        SASSERT(highestPriorityPeer);
        SASSERT(freshestPeer);

        SDEBUG("Dumping evaluated cluster state: numLoggedInFullPeers=" << numLoggedInFullPeers << " freshestPeer=" << freshestPeer->name << " highestPriorityPeer=" << highestPriorityPeer->name << " currentLeader=" << (currentLeader ? currentLeader->name : "none"));

        // If there is already a leader that is higher priority than us,
        // subscribe -- even if we're not in sync with it.  (It'll bring
        // us back up to speed while subscribing.)
        if (currentLeader && _priority < highestPriorityPeer->calc("Priority") && currentLeader->state == LEADING) {
            // Subscribe to the leader
            SINFO("Subscribing to leader '" << currentLeader->name << "'");
            lock_guard<mutex> leadPeerLock(_leadPeerMutex);
            _leadPeer = currentLeader;
            _leaderVersion = (*_leadPeer)["Version"];
            _sendToPeer(currentLeader, SData("SUBSCRIBE"));
            _changeState(SUBSCRIBING);
            return true; // Re-update
        }

        // No leader to subscribe to, let's see if there's anybody else
        // out there with commits we don't have.  Might as well synchronize
        // while waiting.
        if (freshestPeer->calcU64("CommitCount") > _db.getCommitCount()) {
            // Out of sync with a peer -- resynchronize
            SHMMM("Lost synchronization while waiting; re-SEARCHING.");
            _changeState(SEARCHING);
            return true; // Re-update
        }

        // No leader and we're in sync, perhaps everybody is waiting for us
        // to stand up?  If we're higher than the highest priority, are using 
        // a real priority and are not a permafollower, and are connected to 
        // enough full peers to achieve quorum, we should be leader.
        if (!currentLeader && numLoggedInFullPeers * 2 >= numFullPeers &&
            _priority > 0 && _priority > highestPriorityPeer->calc("Priority")) {
            // Yep -- time for us to stand up -- clear everyone's
            // last approval status as they're about to send them.
            SINFO("No leader and we're highest priority (over " << highestPriorityPeer->name << "), STANDINGUP");
            for (auto peer : peerList) {
                peer->erase("StandupResponse");
            }
            _changeState(STANDINGUP);
            return true; // Re-update
        }

        // Otherwise, Keep waiting
        SDEBUG("Connected to " << numLoggedInFullPeers << " of " << numFullPeers << " full peers (" << peerList.size()
                               << " with permafollowers), priority=" << _priority);
        break;
    }

    /// - STANDINGUP: We're waiting for peers to approve or deny our standup
    ///     request.  The logic for this state is:
    ///
    ///         if( at least one peer has denied standup )
    ///             goto SEARCHING
    ///         if( everybody has responded and approved )
    ///             goto LEADING
    ///         if( somebody hasn't responded but we're timing out )
    ///             goto SEARCHING
    ///
    case STANDINGUP: {
        SASSERTWARN(!_syncPeer);
        SASSERTWARN(!_leadPeer);
        SASSERTWARN(_db.getUncommittedHash().empty());
        // Wait for everyone to respond
        bool allResponded = true;
        int numFullPeers = 0;
        int numLoggedInFullPeers = 0;
        if (gracefulShutdown()) {
            SINFO("Shutting down while standing up, setting state to SEARCHING");
            _changeState(SEARCHING);
            return true; // Re-update
        }
        for (auto peer : peerList) {
            // Check this peer; if not logged in, tacit approval
            if (peer->params["Permafollower"] != "true") {
                ++numFullPeers;
                if (SIEquals((*peer)["LoggedIn"], "true")) {
                    // Connected and logged in.
                    numLoggedInFullPeers++;

                    // Has it responded yet?
                    if (!peer->isSet("StandupResponse")) {
                        // At least one logged in full peer hasn't responded
                        allResponded = false;
                    } else if (!SIEquals((*peer)["StandupResponse"], "approve")) {
                        // It responeded, but didn't approve -- abort
                        PHMMM("Refused our STANDUP (" << (*peer)["Reason"] << "), cancel and RESEARCH");
                        _changeState(SEARCHING);
                        return true; // Re-update
                    }
                }
            }
        }

        // If everyone's responded with approval and we form a majority, then finish standup.
        bool majorityConnected = numLoggedInFullPeers * 2 >= numFullPeers;
        if (allResponded && majorityConnected) {
            // Complete standup
            SINFO("All peers approved standup, going LEADING.");
            _changeState(LEADING);
            _leaderVersion = _version;
            return true; // Re-update
        }

        // See if we're taking too long
        if (STimeNow() > _stateTimeout) {
            // Timed out
            SHMMM("Timed out waiting for STANDUP approval; reconnect all and re-SEARCHING.");
            _reconnectAll();
            _changeState(SEARCHING);
            return true; // Re-update
        }
        break;
    }

    /// - LEADING / STANDINGDOWN : These are the states where the magic
    ///     happens.  In both states, the node will execute distributed
    ///     transactions.  However, new transactions are only
    ///     started in the LEADING state (while existing transactions are
    ///     concluded in the STANDINGDOWN) state.  The logic for this state
    ///     is as follows:
    ///
    ///         if( we're processing a transaction )
    ///             if( all subscribed followers have responded/approved )
    ///                 commit this transaction to the local DB
    ///                 broadcast COMMIT_TRANSACTION to all subscribed followers
    ///                 send a STATE to show we've committed a new transaction
    ///                 notify the caller that the command is complete
    ///         if( we're LEADING and not processing a command )
    ///             if( there is another LEADER )         goto STANDINGDOWN
    ///             if( there is a higher priority peer ) goto STANDINGDOWN
    ///             if( a command is queued )
    ///                 if( processing the command affects the database )
    ///                    clear the TransactionResponse of all peers
    ///                    broadcast BEGIN_TRANSACTION to subscribed followers
    ///         if( we're standing down and all followers have unsubscribed )
    ///             goto SEARCHING
    ///
    case LEADING:
    case STANDINGDOWN: {
        SASSERTWARN(!_syncPeer);
        SASSERTWARN(!_leadPeer);

        // NOTE: This block very carefully will not try and call _changeState() while holding SQLite::g_commitLock,
        // because that could cause a deadlock when called by an outside caller!

        // If there's no commit in progress, we'll send any outstanding transactions that exist. We won't send them
        // mid-commit, as they'd end up as nested transactions interleaved with the one in progress.
        if (!commitInProgress()) {
            _sendOutstandingTransactions();
        }

        // This means we've started a distributed transaction and need to decide if we should commit it, which can mean
        // waiting on peers to approve the transaction. We can do this even after we've begun standing down.
        if (_commitState == CommitState::COMMITTING) {
            // Loop across all peers configured to see how many are:
            int numFullPeers = 0;     // Num non-permafollowers configured
            int numFullFollowers = 0; // Num full peers that are "subscribed"
            int numFullResponded = 0; // Num full peers that have responded approve/deny
            int numFullApproved = 0;  // Num full peers that have approved
            int numFullDenied = 0;    // Num full peers that have denied
            for (auto peer : peerList) {
                // Check this peer to see if it's full or a permafollower
                if (peer->params["Permafollower"] != "true") {
                    // It's a full peer -- is it subscribed, and if so, how did it respond?
                    ++numFullPeers;
                    if ((*peer)["Subscribed"] == "true") {
                        // Subscribed, did it respond?
                        numFullFollowers++;
                        const string& response = (*peer)["TransactionResponse"];
                        if (response.empty()) {
                            continue;
                        }
                        numFullResponded++;
                        numFullApproved += SIEquals(response, "approve");
                        if (!SIEquals(response, "approve")) {
                            SWARN("Peer '" << peer->name << "' denied transaction.");
                            ++numFullDenied;
                        } else {
                            SDEBUG("Peer '" << peer->name << "' has approved transaction.");
                        }
                    }
                }
            }

            // Did we get a majority? This is important whether or not our consistency level needs it, as it will
            // reset the checkpoint limit either way.
            bool majorityApproved = (numFullApproved * 2 >= numFullPeers);

            // Figure out if we have enough consistency
            bool consistentEnough = false;
            switch (_commitConsistency) {
                case ASYNC:
                    // Always consistent enough if we don't care!
                    consistentEnough = true;
                    break;
                case ONE:
                    // So long at least one full approved (if we have any peers, that is), we're good.
                    consistentEnough = !numFullPeers || (numFullApproved > 0);
                    break;
                case QUORUM:
                    // This one requires a majority
                    consistentEnough = majorityApproved;
                    break;
                default:
                    SERROR("Invalid write consistency.");
                    break;
            }

            // See if all active non-permafollowers have responded.
            // NOTE: This can be true if nobody responds if there are no full followers - this includes machines that
            // should be followers that are disconnected.
            bool everybodyResponded = numFullResponded >= numFullFollowers;

            // Record these for posterity
            SDEBUG(     "numFullPeers="           << numFullPeers
                   << ", numFullFollowers="       << numFullFollowers
                   << ", numFullResponded="       << numFullResponded
                   << ", numFullApproved="        << numFullApproved
                   << ", majorityApproved="       << majorityApproved
                   << ", writeConsistency="       << consistencyLevelNames[_commitConsistency]
                   << ", consistencyRequired="    << consistencyLevelNames[_commitConsistency]
                   << ", consistentEnough="       << consistentEnough
                   << ", everybodyResponded="     << everybodyResponded);

            // If anyone denied this transaction, roll this back. Alternatively, roll it back if everyone we're
            // currently connected to has responded, but that didn't generate enough consistency. This could happen, in
            // theory, if we were disconnected from enough of the cluster that we could no longer reach QUORUM, but
            // this should have been detected earlier and forced us out of leading.
            // TODO: we might want to remove the `numFullDenied` condition here. A single failure shouldn't cause the
            // entire cluster to break. Imagine a scenario where a follower disk was full, and every write operation
            // failed with an sqlite3 error.
            if (numFullDenied || (everybodyResponded && !consistentEnough)) {
                SINFO("Rolling back transaction because everybody currently connected responded "
                      "but not consistent enough. Num denied: " << numFullDenied << ". Follower write failure?");

                // Notify everybody to rollback
                SData rollback("ROLLBACK_TRANSACTION");
                rollback.set("ID", _lastSentTransactionID + 1);
                rollback.set("NewHash", _db.getUncommittedHash());
                _sendToAllPeers(rollback, true); // true: Only to subscribed peers.
                _db.rollback();

                // Finished, but failed.
                _commitState = CommitState::FAILED;
            } else if (consistentEnough) {
                // Commit this distributed transaction. Either we have quorum, or we don't need it.
                SDEBUG("Committing current transaction because consistentEnough: " << _db.getUncommittedQuery());
                uint64_t beforeCommit = STimeNow();
                int result = _db.commit();
                SINFO("SQLite::commit in SQLiteNode took " << ((STimeNow() - beforeCommit)/1000) << "ms.");

                // If this is the case, there was a commit conflict.
                if (result == SQLITE_BUSY_SNAPSHOT) {
                    // We already asked everyone to commit this (even if it was async), so we'll have to tell them to
                    // roll back.
                    SINFO("[performance] Conflict committing " << consistencyLevelNames[_commitConsistency]
                          << " commit, rolling back.");
                    SData rollback("ROLLBACK_TRANSACTION");
                    rollback.set("ID", _lastSentTransactionID + 1);
                    rollback.set("NewHash", _db.getUncommittedHash());
                    _sendToAllPeers(rollback, true); // true: Only to subscribed peers.
                    _db.rollback();

                    // Finished, but failed.
                    _commitState = CommitState::FAILED;
                } else {
                    // Hey, our commit succeeded! Record how long it took.
                    uint64_t beginElapsed, readElapsed, writeElapsed, prepareElapsed, commitElapsed, rollbackElapsed;
                    uint64_t totalElapsed = _db.getLastTransactionTiming(beginElapsed, readElapsed, writeElapsed,
                                                                         prepareElapsed, commitElapsed, rollbackElapsed);
                    SINFO("Committed leader transaction for '"
                          << _db.getCommitCount() << " (" << _db.getCommittedHash() << "). "
                          << " (consistencyRequired=" << consistencyLevelNames[_commitConsistency] << "), "
                          << numFullApproved << " of " << numFullPeers << " approved (" << peerList.size() << " total) in "
                          << totalElapsed / 1000 << " ms ("
                          << beginElapsed / 1000 << "+" << readElapsed / 1000 << "+"
                          << writeElapsed / 1000 << "+" << prepareElapsed / 1000 << "+"
                          << commitElapsed / 1000 << "+" << rollbackElapsed / 1000 << "ms)");

                    SINFO("[performance] Successfully committed " << consistencyLevelNames[_commitConsistency]
                          << " transaction. Sending COMMIT_TRANSACTION to peers.");
                    SData commit("COMMIT_TRANSACTION");
                    commit.set("ID", _lastSentTransactionID + 1);
                    _sendToAllPeers(commit, true); // true: Only to subscribed peers.

                    // clear the unsent transactions, we've sent them all (including this one);
                    _db.getCommittedTransactions();

                    // Update the last sent transaction ID to reflect that this is finished.
                    _lastSentTransactionID = _db.getCommitCount();

                    // Done!
                    _commitState = CommitState::SUCCESS;
                }
            } else {
                // Not consistent enough, but not everyone's responded yet, so we'll wait.
                SINFO("Waiting to commit. consistencyRequired=" << consistencyLevelNames[_commitConsistency]);

                // We're going to need to read from the network to finish this.
                return false;
            }

            // We were committing, but now we're not. The only code path through here that doesn't lead to the point
            // is the 'return false' immediately above here, everything else completes the transaction (even if it was
            // a failed transaction), so we can safely unlock now.
            SQLite::g_commitLock.unlock();
        }

        // If there's a transaction that's waiting, we'll start it. We do this *before* we check to see if we should
        // stand down, and since we return true, we'll never stand down as long as we keep adding new transactions
        // here. It's up to the server to stop giving us transactions to process if it wants us to stand down.
        if (_commitState == CommitState::WAITING) {
            // Lock the database. We'll unlock it when we complete in a future update cycle.
            SQLite::g_commitLock.lock();
            _commitState = CommitState::COMMITTING;
            SINFO("[performance] Beginning " << consistencyLevelNames[_commitConsistency] << " commit.");

            // Now that we've grabbed the commit lock, we can safely clear out any outstanding transactions, no new
            // ones can be added until we release the lock.
            _sendOutstandingTransactions();

            // We'll send the commit count to peers.
            uint64_t commitCount = _db.getCommitCount();

            // If there was nothing changed, then we shouldn't have anything to commit.
            // Except that this is allowed right now.
            // SASSERT(!_db.getUncommittedQuery().empty());

            // There's no handling for a failed prepare. This should only happen if the DB has been corrupted or
            // something catastrophic like that.
            SASSERT(_db.prepare());

            // Begin the distributed transaction
            SData transaction("BEGIN_TRANSACTION");
            SINFO("beginning distributed transaction for commit #" << commitCount + 1 << " ("
                  << _db.getUncommittedHash() << ")");
            transaction.set("NewCount", commitCount + 1);
            transaction.set("NewHash", _db.getUncommittedHash());
            transaction.set("leaderSendTime", to_string(STimeNow()));
            if (_commitConsistency == ASYNC) {
                transaction["ID"] = "ASYNC_" + to_string(_lastSentTransactionID + 1);
            } else {
                transaction.set("ID", _lastSentTransactionID + 1);
            }
            transaction.content = _db.getUncommittedQuery();

            for (auto peer : peerList) {
                // Clear the response flag from the last transaction
                (*peer)["TransactionResponse"].clear();
            }

            // And send it to everyone who's subscribed.
            uint64_t beforeSend = STimeNow();
            _sendToAllPeers(transaction, true);
            SINFO("[performance] SQLite::_sendToAllPeers in SQLiteNode took " << ((STimeNow() - beforeSend)/1000) << "ms.");

            // We return `true` here to immediately re-update and thus commit this transaction immediately if it was
            // asynchronous.
            return true;
        }

        // Check to see if we should stand down. We'll finish any outstanding commits before we actually do.
        if (_state == LEADING) {
            string standDownReason;
            if (gracefulShutdown()) {
                // Graceful shutdown. Set priority 1 and stand down so we'll re-connect to the new leader and finish
                // up our commands.
                standDownReason = "Shutting down, setting priority 1 and STANDINGDOWN.";
                _priority = 1;
            } else {
                // Loop across peers
                for (auto peer : peerList) {
                    // Check this peer
                    if (peer->state == LEADING) {
                        // Hm... somehow we're in a multi-leader scenario -- not good.
                        // Let's get out of this as soon as possible.
                        standDownReason = "Found another LEADER (" + peer->name + "), STANDINGDOWN to clean it up.";
                    } else if (peer->state == WAITING) {
                        // We have a WAITING peer; is it waiting to STANDUP?
                        if (peer->calc("Priority") > _priority) {
                            // We've got a higher priority peer in the works; stand down so it can stand up.
                            standDownReason = "Found higher priority WAITING peer (" + peer->name
                                              + ") while LEADING, STANDINGDOWN";
                        } else if (peer->calcU64("CommitCount") > _db.getCommitCount()) {
                            // It's got data that we don't, stand down so we can get it.
                            standDownReason = "Found WAITING peer (" + peer->name +
                                              ") with more data than us (we have " + SToStr(_db.getCommitCount()) +
                                              "/" + _db.getCommittedHash() + ", it has " + (*peer)["CommitCount"] +
                                              "/" + (*peer)["Hash"] + ") while LEADING, STANDINGDOWN";
                        }
                    }
                }
            }

            // Do we want to stand down, and can we?
            if (!standDownReason.empty()) {
                SHMMM(standDownReason);
                _changeState(STANDINGDOWN);
                SINFO("Standing down: " << standDownReason);
            }
        }

        // At this point, we're no longer committing. We'll have returned false above, or we'll have completed any
        // outstanding transaction, we can complete standing down if that's what we're doing.
        if (_state == STANDINGDOWN) {
            // See if we're done
            // We can only switch to SEARCHING if the server has no outstanding write work to do.
            if (_standDownTimeOut.ringing()) {
                SWARN("Timeout STANDINGDOWN, giving up on server and continuing.");
            } else if (!_server.canStandDown()) {
                // Try again.
                SINFO("Can't switch from STANDINGDOWN to SEARCHING yet, server prevented state change.");
                return false;
            }
            // Standdown complete
            SINFO("STANDDOWN complete, SEARCHING");
            _changeState(SEARCHING);

            // We're no longer waiting on responses from peers, we can re-update immediately and start becoming a
            // follower node instead.
            return true;
        }
        break;
    }

    /// - SUBSCRIBING: We're waiting for a SUBSCRIPTION_APPROVED from the
    ///     leader.  When we receive it, we'll go FOLLOWING. Otherwise, if we
    ///     timeout, go SEARCHING.
    ///
    case SUBSCRIBING:
        SASSERTWARN(!_syncPeer);
        SASSERTWARN(_leadPeer);
        SASSERTWARN(_db.getUncommittedHash().empty());
        // Nothing to do but wait
        if (STimeNow() > _stateTimeout) {
            // Give up
            SHMMM("Timed out waiting for SUBSCRIPTION_APPROVED, reconnecting to leader and re-SEARCHING.");
            lock_guard<mutex> leadPeerLock(_leadPeerMutex);
            _reconnectPeer(_leadPeer);
            _leadPeer = nullptr;
            _changeState(SEARCHING);
            return true; // Re-update
        }
        break;

    /// - FOLLOWING: This is where the other half of the magic happens.  Most
    ///     nodes will (hopefully) spend 99.999% of their time in this state.
    ///     FOLLOWING nodes simply begin and commit transactions with the
    ///     following logic:
    ///
    ///         if( leader steps down or disconnects ) goto SEARCHING
    ///         if( new queued commands ) send ESCALATE to leader
    ///
    case FOLLOWING:
        SASSERTWARN(!_syncPeer);
        // If graceful shutdown requested, stop following once there is
        // nothing blocking shutdown.  We stop listening for new commands
        // immediately upon TERM.)
        if (gracefulShutdown() && _isNothingBlockingShutdown()) {
            // Go searching so we stop following
            SINFO("Stopping FOLLOWING in order to gracefully shut down, SEARCHING.");
            _changeState(SEARCHING);
            return false; // Don't update
        }

        // If the leader stops leading (or standing down), we'll go SEARCHING, which allows us to look for a new
        // leader. We don't want to go searching before that, because we won't know when leader is done sending its
        // final transactions.
        SASSERT(_leadPeer);
        if (_leadPeer.load()->state != LEADING && _leadPeer.load()->state != STANDINGDOWN) {
            // Leader stepping down
            SHMMM("Leader stepping down, re-queueing commands.");

            // If there were escalated commands, give them back to the server to retry.
            for (auto& cmd : _escalatedCommandMap) {
                _server.acceptCommand(move(cmd.second), false);
            }
            _escalatedCommandMap.clear();

            // Are we in the middle of a commit? This should only happen if we received a `BEGIN_TRANSACTION` without a
            // corresponding `COMMIT` or `ROLLBACK`, this isn't supposed to happen.
            if (!_db.getUncommittedHash().empty()) {
                SWARN("Leader stepped down with transaction in progress, rolling back.");
                _db.rollback();
            }
            _changeState(SEARCHING);
            return true; // Re-update
        }

        break;

    default:
        SERROR("Invalid state #" << _state);
    }

    // Don't update immediately
    return false;
}

// Messages
// Here are the messages that can be received, and how a cluster node will respond to each based on its state:
void SQLiteNode::_onMESSAGE(Peer* peer, const SData& message) {
    SASSERT(peer);
    SASSERTWARN(!message.empty());
    SDEBUG("Received sqlitenode message from peer " << peer->name << ": " << message.serialize());
    // Every message broadcasts the current state of the node
    if (!message.isSet("CommitCount")) {
        STHROW("missing CommitCount");
    }
    if (!message.isSet("Hash")) {
        STHROW("missing Hash");
    }
    (*peer)["CommitCount"] = message["CommitCount"];
    (*peer)["Hash"] = message["Hash"];

    // Classify and process the message
    if (SIEquals(message.methodLine, "LOGIN")) {
        // LOGIN: This is the first message sent to and received from a new peer. It communicates the current state of
        // the peer (hash and commit count), as well as the peer's priority. Peers can connect in any state, so this
        // message can be sent and received in any state.
        if ((*peer)["LoggedIn"] == "true") {
            STHROW("already logged in");
        }
        if (!message.isSet("Priority")) {
            STHROW("missing Priority");
        }
        if (!message.isSet("State")) {
            STHROW("missing State");
        }
        if (!message.isSet("Version")) {
            STHROW("missing Version");
        }
        if (peer->params["Permafollower"] == "true" && (message["Permafollower"] != "true" || message.calc("Priority") > 0)) {
            STHROW("you're supposed to be a 0-priority permafollower");
        }
        if (peer->params["Permafollower"] != "true" && (message["Permafollower"] == "true" || message.calc("Priority") == 0)) {
            STHROW("you're *not* supposed to be a 0-priority permafollower");
        }

        // It's an error to have to peers configured with the same priority, except 0 and -1
        SASSERT(_priority == -1 || _priority == 0 || message.calc("Priority") != _priority);
        PINFO("Peer logged in at '" << message["State"] << "', priority #" << message["Priority"] << " commit #"
              << message["CommitCount"] << " (" << message["Hash"] << ")");
        peer->set("Priority", message["Priority"]);
        peer->set("LoggedIn", "true");
        peer->set("Version",  message["Version"]);
        peer->state = stateFromName(message["State"]);

        // Let the server know that a peer has logged in.
        _server.onNodeLogin(peer);
    } else if (!SIEquals((*peer)["LoggedIn"], "true")) {
        STHROW("not logged in");
    }
    else if (SIEquals(message.methodLine, "STATE")) {
        // STATE: Broadcast to all peers whenever a node's state changes. Also sent whenever a node commits a new query
        // (and thus has a new commit count and hash). A peer can react or respond to a peer's state change as follows:
        if (!message.isSet("State")) {
            STHROW("missing State");
        }
        if (!message.isSet("Priority")) {
            STHROW("missing Priority");
        }
        const State from = peer->state;
        peer->set("Priority", message["Priority"]);
        peer->state = stateFromName(message["State"]);
        const State to = peer->state;
        if (from == to) {
            // No state change, just new commits?
            PINFO("Peer received new commit in state '" << stateName(from) << "', commit #" << message["CommitCount"] << " ("
                  << message["Hash"] << ")");
        } else {
            // State changed -- first see if it's doing anything unusual
            PINFO("Peer switched from '" << stateName(from) << "' to '" << stateName(to) << "' commit #" << message["CommitCount"]
                  << " (" << message["Hash"] << ")");
            if (from == UNKNOWN) {
                PWARN("Peer coming from unrecognized state '" << stateName(from) << "'");
            }
            if (to == UNKNOWN) {
                PWARN("Peer going to unrecognized state '" << stateName(to) << "'");
            }

            // Make sure transition states are an approved pair
            bool okTransition = false;
            switch (from) {
            case UNKNOWN:
                break;
            case SEARCHING:
                okTransition = (to == SYNCHRONIZING || to == WAITING || to == LEADING);
                break;
            case SYNCHRONIZING:
                okTransition = (to == SEARCHING || to == WAITING);
                break;
            case WAITING:
                okTransition = (to == SEARCHING || to == STANDINGUP || to == SUBSCRIBING);
                break;
            case STANDINGUP:
                okTransition = (to == SEARCHING || to == LEADING);
                break;
            case LEADING:
                okTransition = (to == SEARCHING || to == STANDINGDOWN);
                break;
            case STANDINGDOWN:
                okTransition = (to == SEARCHING);
                break;
            case SUBSCRIBING:
                okTransition = (to == SEARCHING || to == FOLLOWING);
                break;
            case FOLLOWING:
                okTransition = (to == SEARCHING);
                break;
            }
            if (!okTransition) {
                PWARN("Peer making invalid transition from '" << stateName(from) << "' to '" << stateName(to) << "'");
            }

            // Next, should we do something about it?
            if (to == SEARCHING) {
                // SEARCHING: If anything ever goes wrong, a node reverts to the SEARCHING state. Thus if we see a peer
                // go SEARCHING, we reset its accumulated state.  Specifically, we mark it is no longer being
                // "subscribed", and we clear its last transaction response.
                peer->erase("TransactionResponse");
                peer->erase("Subscribed");
            } else if (to == STANDINGUP) {
                // STANDINGUP: When a peer announces it intends to stand up, we immediately respond with approval or
                // denial. We determine this by checking to see if there is any  other peer who is already leader or
                // also trying to stand up.
                //
                // **FIXME**: Should it also deny if it knows of a higher priority peer?
                SData response("STANDUP_RESPONSE");
                // Parrot back the node's attempt count so that it can differentiate stale responses.
                response["StateChangeCount"] = message["StateChangeCount"];
                if (peer->params["Permafollower"] == "true") {
                    // We think it's a permafollower, deny
                    PHMMM("Permafollower trying to stand up, denying.");
                    response["Response"] = "deny";
                    response["Reason"] = "You're a permafollower";
                }

                // What's our state
                if (SWITHIN(STANDINGUP, _state, STANDINGDOWN)) {
                    // Oh crap, it's trying to stand up while we're leading. Who is higher priority?
                    if (peer->calc("Priority") > _priority) {
                        // The other peer is a higher priority than us, so we should stand down (maybe it crashed, we
                        // came up as leader, and now it's been brought back up). We'll want to stand down here, but we
                        // do it gracefully so that we won't lose any transactions in progress.
                        if (_state == STANDINGUP) {
                            PWARN("Higher-priority peer is trying to stand up while we are STANDINGUP, SEARCHING.");
                            _changeState(SEARCHING);
                        } else if (_state == LEADING) {
                            PWARN("Higher-priority peer is trying to stand up while we are LEADING, STANDINGDOWN.");
                            _changeState(STANDINGDOWN);
                        } else {
                            PWARN("Higher-priority peer is trying to stand up while we are STANDINGDOWN, continuing.");
                        }
                    } else {
                        // Deny because we're currently in the process of leading and we're higher priority.
                        response["Response"] = "deny";
                        response["Reason"] = "I am leading";

                        // Hmm, why is a lower priority peer trying to stand up? Is it possible we're no longer in
                        // control of the cluster? Let's see how many nodes are subscribed.
                        if (_majoritySubscribed()) {
                            // we have a majority of the cluster, so ignore this oddity.
                            PHMMM("Lower-priority peer is trying to stand up while we are " << stateName(_state)
                                  << " with a majority of the cluster; denying and ignoring.");
                        } else {
                            // We don't have a majority of the cluster -- maybe it knows something we don't?  For
                            // example, it could be that the rest of the cluster has forked away from us. This can
                            // happen if the leader hangs while processing a command: by the time it finishes, the
                            // cluster might have elected a new leader, forked, and be a thousand commits in the future.
                            // In this case, let's just reset everything anyway to be safe.
                            PWARN("Lower-priority peer is trying to stand up while we are " << stateName(_state)
                                  << ", but we don't have a majority of the cluster so reconnecting and SEARCHING.");
                            _reconnectAll();
                            // TODO: This puts us in an ambiguous state if we switch to SEARCHING from LEADING,
                            // without going through the STANDDOWN process. We'll need to handle it better, but it's
                            // unclear if this can ever happen at all. exit() may be a reasonable strategy here.
                            _changeState(SEARCHING);
                        }
                    }
                } else {
                    // Approve if nobody else is trying to stand up
                    response["Response"] = "approve"; // Optimistic; will override
                    for (auto otherPeer : peerList) {
                        if (otherPeer != peer) {
                            // See if it's trying to be leader
                            if (otherPeer->state == STANDINGUP || otherPeer->state == LEADING || otherPeer->state == STANDINGDOWN) {
                                // We need to contest this standup
                                response["Response"] = "deny";
                                response["Reason"] = "peer '" + otherPeer->name + "' is '" + stateName(otherPeer->state) + "'";
                                break;
                            }
                        }
                    }
                }

                // Send the response
                if (SIEquals(response["Response"], "approve")) {
                    PINFO("Approving standup request");
                } else {
                    PHMMM("Denying standup request because " << response["Reason"]);
                }
                _sendToPeer(peer, response);
            } else if (from == STANDINGDOWN) {
                // STANDINGDOWN: When a peer stands down we double-check to make sure we don't have any outstanding
                // transaction (and if we do, we warn and rollback).
                if (!_db.getUncommittedHash().empty()) {
                    // Crap, we were waiting for a response that will apparently never come. I guess roll it back? This
                    // should never happen, however, as the leader shouldn't STANDOWN unless all subscribed followers
                    // (including us) have already unsubscribed, and we wouldn't do that in the middle of a
                    // transaction. But just in case...
                    SASSERTWARN(_state == FOLLOWING);
                    PWARN("Was expecting a response for transaction #"
                          << _db.getCommitCount() + 1 << " (" << _db.getUncommittedHash()
                          << ") but stood down prematurely, rolling back and hoping for the best.");
                    _db.rollback();
                }
            }
        }
    } else if (SIEquals(message.methodLine, "STANDUP_RESPONSE")) {
        // STANDUP_RESPONSE: Sent in response to the STATE message generated when a node enters the STANDINGUP state.
        // Contains a header "Response" with either the value "approve" or "deny".  This response is stored within the
        // peer for testing in the update loop.
        if (_state == STANDINGUP) {
            // We only verify this if it's present, which allows us to still receive valid STANDUP_RESPONSE
            // messages from peers on older versions. Once all nodes have been upgraded past the first version that
            // supports this, we can enforce that this count is present.
            if (message.isSet("StateChangeCount") && message.calc("StateChangeCount") != _stateChangeCount) {
                SHMMM("Received STANDUP_RESPONSE for old standup attempt (" << message.calc("StateChangeCount") << "), ignoring.");
                return;
            }
            if (!message.isSet("Response")) {
                STHROW("missing Response");
            }
            if (peer->isSet("StandupResponse")) {
                PWARN("Already received standup response '" << (*peer)["StandupResponse"] << "', now receiving '"
                      << message["Response"] << "', odd -- multiple leaders competing?");
            }
            if (SIEquals(message["Response"], "approve")) {
                PINFO("Received standup approval");
            } else {
                PHMMM("Received standup denial: reason='" << message["Reason"] << "'");
            }
            (*peer)["StandupResponse"] = message["Response"];
        } else {
            SINFO("Got STANDUP_RESPONSE but not STANDINGUP. Probably a late message, ignoring.");
        }
    } else if (SIEquals(message.methodLine, "SYNCHRONIZE")) {
        // If we're FOLLOWING, we'll let worker threads handle SYNCHRONIZATION messages. We don't on leader, because if
        // there's a backlog of commands, these can get stale, and by the time they reach the follower, it's already
        // behind, thus never catching up.
        if (_state == FOLLOWING) {
            // Attach all of the state required to populate a SYNCHRONIZE_RESPONSE to this message. All of this is
            // processed asynchronously, but that is fine, the final `SUBSCRIBE` message and its response will be
            // processed synchronously.
            SData request = message;
            request["peerCommitCount"] = (*peer)["CommitCount"];
            request["peerHash"] = (*peer)["Hash"];
            request["peerID"] = to_string(getIDByPeer(peer));
            request["targetCommit"] = to_string(unsentTransactions.load() ? _lastSentTransactionID : _db.getCommitCount());

            // The following properties are only used to expand out our log macros.
            request["name"] = name;
            request["peerName"] = peer->name;

            // Create a command from this request and pass it on to the server to handle.
            auto command = make_unique<SQLiteCommand>(move(request));
            command->initiatingPeerID = peer->id;
            _server.acceptCommand(move(command), true);
        } else {
            // Otherwise we handle them immediately, as the server doesn't deliver commands to workers until we've
            // stood up.
            SData response("SYNCHRONIZE_RESPONSE");
            _queueSynchronize(peer, response, false);
            _sendToPeer(peer, response);
        }
    } else if (SIEquals(message.methodLine, "SYNCHRONIZE_RESPONSE")) {
        // SYNCHRONIZE_RESPONSE: Sent in response to a SYNCHRONIZE request. Contains a payload of zero or more COMMIT
        // messages, all of which are immediately committed to the local database.
        if (_state != SYNCHRONIZING) {
            STHROW("not synchronizing");
        }
        if (!_syncPeer) {
            STHROW("too late, gave up on you");
        }
        if (peer != _syncPeer) {
            STHROW("sync peer mismatch");
        }
        PINFO("Beginning synchronization");
        try {
            // Received this synchronization response; are we done?
            _recvSynchronize(peer, message);
            uint64_t peerCommitCount = _syncPeer->calcU64("CommitCount");
            if (_db.getCommitCount() == peerCommitCount) {
                // All done
                SINFO("Synchronization complete, at commitCount #" << _db.getCommitCount() << " ("
                      << _db.getCommittedHash() << "), WAITING");
                _syncPeer = nullptr;
                _changeState(WAITING);
            } else if (_db.getCommitCount() > peerCommitCount) {
                // How did this happen?  Something is screwed up.
                SWARN("We have more data (" << _db.getCommitCount() << ") than our sync peer '" << _syncPeer->name
                      << "' (" << peerCommitCount << "), reconnecting and SEARCHING.");
                _reconnectPeer(_syncPeer);
                _syncPeer = nullptr;
                _changeState(SEARCHING);
            } else {
                // Otherwise, more to go
                SINFO("Synchronization underway, at commitCount #"
                      << _db.getCommitCount() << " (" << _db.getCommittedHash() << "), "
                      << peerCommitCount - _db.getCommitCount() << " to go.");
                _updateSyncPeer();
                if (_syncPeer) {
                    _sendToPeer(_syncPeer, SData("SYNCHRONIZE"));
                } else {
                    SWARN("No usable _syncPeer but syncing not finished. Going to SEARCHING.");
                    _changeState(SEARCHING);
                }

                // Also, extend our timeout so long as we're still alive
                _stateTimeout = STimeNow() + SQL_NODE_SYNCHRONIZING_RECV_TIMEOUT + SRandom::rand64() % STIME_US_PER_S * 5;
            }
        } catch (const SException& e) {
            // Transaction failed
            SWARN("Synchronization failed '" << e.what() << "', reconnecting and re-SEARCHING.");
            _reconnectPeer(_syncPeer);
            _syncPeer = nullptr;
            _changeState(SEARCHING);
            throw e;
        }
    } else if (SIEquals(message.methodLine, "SUBSCRIBE")) {
        // SUBSCRIBE: Sent by a node in the WAITING state to the current leader to begin FOLLOWING. Respond
        // SUBSCRIPTION_APPROVED with any COMMITs that the subscribing peer lacks (for example, any commits that have
        // occurred after it completed SYNCHRONIZING but before this SUBSCRIBE was received). Tag this peer as
        // "subscribed" for use in the LEADING and STANDINGDOWN update loops. Finally, if there is an outstanding
        // distributed transaction being processed, send it to this new follower.
        if (_state != LEADING) {
            STHROW("not leading");
        }
        PINFO("Received SUBSCRIBE, accepting new follower");
        SData response("SUBSCRIPTION_APPROVED");
        _queueSynchronize(peer, response, true); // Send everything it's missing
        _sendToPeer(peer, response);
        SASSERTWARN(!SIEquals((*peer)["Subscribed"], "true"));
        (*peer)["Subscribed"] = "true";

        // New follower; are we in the midst of a transaction?
        if (_commitState == CommitState::COMMITTING) {
            // Invite the new peer to participate in the transaction
            SINFO("Inviting peer into distributed transaction already underway (" << _db.getUncommittedHash() << ")");

            // TODO: This duplicates code in `update()`, would be nice to refactor out the common code.
            uint64_t commitCount = _db.getCommitCount();
            SData transaction("BEGIN_TRANSACTION");
            SINFO("beginning distributed transaction for commit #" << commitCount + 1 << " ("
                  << _db.getUncommittedHash() << ")");
            transaction.set("NewCount", commitCount + 1);
            transaction.set("NewHash", _db.getUncommittedHash());
            transaction.set("leaderSendTime", to_string(STimeNow()));
            transaction.set("ID", _lastSentTransactionID + 1);
            transaction.content = _db.getUncommittedQuery();
            _sendToPeer(peer, transaction);
        }
    } else if (SIEquals(message.methodLine, "SUBSCRIPTION_APPROVED")) {
        // SUBSCRIPTION_APPROVED: Sent by a follower's new leader to complete the subscription process. Includes zero or
        // more COMMITS that should be immediately applied to the database.
        if (_state != SUBSCRIBING) {
            STHROW("not subscribing");
        }
        if (_leadPeer != peer) {
            STHROW("not subscribing to you");
        }
        SINFO("Received SUBSCRIPTION_APPROVED, final synchronization.");
        try {
            // Done synchronizing
            _recvSynchronize(peer, message);
            SINFO("Subscription complete, at commitCount #" << _db.getCommitCount() << " (" << _db.getCommittedHash()
                  << "), FOLLOWING");
            _changeState(FOLLOWING);
        } catch (const SException& e) {
            // Transaction failed
            SWARN("Subscription failed '" << e.what() << "', reconnecting to leader and re-SEARCHING.");
            _reconnectPeer(_leadPeer);
            _changeState(SEARCHING);
            throw e;
        }
    } else if (SIEquals(message.methodLine, "BEGIN_TRANSACTION") || SIEquals(message.methodLine, "COMMIT_TRANSACTION") || SIEquals(message.methodLine, "ROLLBACK_TRANSACTION")) {
        _replicationThreads.fetch_add(1);
        thread(replicate, ref(*this), peer, message).detach();
    } else if (SIEquals(message.methodLine, "APPROVE_TRANSACTION") || SIEquals(message.methodLine, "DENY_TRANSACTION")) {
        // APPROVE_TRANSACTION: Sent to the leader by a follower when it confirms it was able to begin a transaction and
        // is ready to commit. Note that this peer approves the transaction for use in the LEADING and STANDINGDOWN
        // update loop.
        if (!message.isSet("ID")) {
            STHROW("missing ID");
        }
        if (!message.isSet("NewCount")) {
            STHROW("missing NewCount");
        }
        if (!message.isSet("NewHash")) {
            STHROW("missing NewHash");
        }
        if (_state != LEADING && _state != STANDINGDOWN) {
            STHROW("not leading");
        }
        string response = SIEquals(message.methodLine, "APPROVE_TRANSACTION") ? "approve" : "deny";
        try {
            // We ignore late approvals of commits that have already been finalized. They could have been committed
            // already, in which case `_lastSentTransactionID` will have incremented, or they could have been rolled
            // back due to a conflict, which would cuase them to have the wrong hash (the hash of the previous attempt
            // at committing the transaction with this ID).
            bool hashMatch = message["NewHash"] == _db.getUncommittedHash();
            if (hashMatch && to_string(_lastSentTransactionID + 1) == message["ID"]) {
                if (message.calcU64("NewCount") != _db.getCommitCount() + 1) {
                    STHROW("commit count mismatch. Expected: " + message["NewCount"] + ", but would actually be: "
                          + to_string(_db.getCommitCount() + 1));
                }
                if (peer->params["Permafollower"] == "true") {
                    STHROW("permafollowers shouldn't approve/deny");
                }
                PINFO("Peer " << response << " transaction #" << message["NewCount"] << " (" << message["NewHash"] << ")");
                (*peer)["TransactionResponse"] = response;
            } else {
                // Old command.  Nothing to do.  We already sent a commit or rollback.
                PINFO("Peer '" << message.methodLine << "' transaction #" << message["NewCount"]
                      << " (" << message["NewHash"] << ") after " << (hashMatch ? "commit" : "rollback") << ".");
            }
        } catch (const SException& e) {
            // Doesn't correspond to the outstanding transaction not necessarily fatal. This can happen if, for
            // example, a command is escalated from/ one follower, approved by the second, but where the first follower dies
            // before the second's approval is received by the leader. In this case the leader will drop the command
            // when the initiating peer is lost, and thus won't have an outstanding transaction (or will be processing
            // a new transaction) when the old, outdated approval is received. Furthermore, in this case we will have
            // already sent a ROLLBACK, so it will already correct itself. If not, then we'll wait for the follower to
            // determine it's screwed and reconnect.
            SWARN("Received " << message.methodLine << " for transaction #"
                  << message.calc("NewCount") << " (" << message["NewHash"] << ", " << message["ID"] << ") but '"
                  << e.what() << "', ignoring.");
        }
    } else if (SIEquals(message.methodLine, "ESCALATE")) {
        // ESCALATE: Sent to the leader by a follower. Is processed like a normal command, except when complete an
        // ESCALATE_RESPONSE is sent to the follower that initiated the escalation.
        if (!message.isSet("ID")) {
            STHROW("missing ID");
        }
        if (_state != LEADING) {
            // Reject escalation because we're no longer leading
            if (_state != STANDINGDOWN) {
                // Don't warn if we're standing down, this is expected.
                PWARN("Received ESCALATE but not LEADING or STANDINGDOWN, aborting command.");
            }
            SData aborted("ESCALATE_ABORTED");
            aborted["ID"] = message["ID"];
            aborted["Reason"] = "not leading";
            _sendToPeer(peer, aborted);
        } else {
            // We're leading, make sure the rest checks out
            SData request;
            if (!request.deserialize(message.content)) {
                STHROW("malformed request");
            }
            if ((*peer)["Subscribed"] != "true") {
                STHROW("not subscribed");
            }
            if (!message.isSet("ID")) {
                STHROW("missing ID");
            }
            PINFO("Received ESCALATE command for '" << message["ID"] << "' (" << request.methodLine << ")");

            // Create a new Command and send to the server.
            auto command = make_unique<SQLiteCommand>(move(request));
            command->initiatingPeerID = peer->id;
            command->id = message["ID"];
            _server.acceptCommand(move(command), true);
        }
    } else if (SIEquals(message.methodLine, "ESCALATE_CANCEL")) {
        // ESCALATE_CANCEL: Sent to the leader by a follower. Indicates that the follower would like to cancel the escalated
        // command, such that it is not processed. For example, if the client that sent the original request
        // disconnects from the follower before an answer is returned, there is no value (and sometimes a negative value)
        // to the leader going ahead and completing it.
        if (!message.isSet("ID")) {
            STHROW("missing ID");
        }
        if (_state != LEADING) {
            // Reject escalation because we're no longer leading
            PWARN("Received ESCALATE_CANCEL but not LEADING, ignoring.");
        } else {
            // We're leading, make sure the rest checks out
            SData request;
            if (!request.deserialize(message.content)) {
                STHROW("malformed request");
            }
            if ((*peer)["Subscribed"] != "true") {
                STHROW("not subscribed");
            }
            if (!message.isSet("ID")) {
                STHROW("missing ID");
            }
            const string& commandID = SToLower(message["ID"]);
            PINFO("Received ESCALATE_CANCEL command for '" << commandID << "'");

            // Pass it along to the server. We don't try and cancel a command that's currently being committed. It's
            // both super unlikely to happen (as it requires perfect timing), and not a deterministic operation anyway
            // (i.e., a few MS network latency would make it too late, anyway).
            _server.cancelCommand(commandID);
        }
    } else if (SIEquals(message.methodLine, "ESCALATE_RESPONSE")) {
        // ESCALATE_RESPONSE: Sent when the leader processes the ESCALATE.
        if (_state != FOLLOWING) {
            STHROW("not following");
        }
        if (!message.isSet("ID")) {
            STHROW("missing ID");
        }
        SData response;
        if (!response.deserialize(message.content)) {
            STHROW("malformed content");
        }

        // Go find the escalated command
        PINFO("Received ESCALATE_RESPONSE for '" << message["ID"] << "'");
        auto commandIt = _escalatedCommandMap.find(message["ID"]);
        if (commandIt != _escalatedCommandMap.end()) {
            // Process the escalated command response
            unique_ptr<SQLiteCommand>& command = commandIt->second;
            if (command->escalationTimeUS) {
                command->escalationTimeUS = STimeNow() - command->escalationTimeUS;
                SINFO("Total escalation time for command " << command->request.methodLine << " was "
                      << command->escalationTimeUS/1000 << "ms.");
            }
            command->response = response;
            command->complete = true;
            _server.acceptCommand(move(command), false);
            _escalatedCommandMap.erase(commandIt);
        } else {
            SHMMM("Received ESCALATE_RESPONSE for unknown command ID '" << message["ID"] << "', ignoring. ");
        }
    } else if (SIEquals(message.methodLine, "ESCALATE_ABORTED")) {
        // ESCALATE_RESPONSE: Sent when the leader aborts processing an escalated command. Re-submit to the new leader.
        if (_state != FOLLOWING) {
            STHROW("not following");
        }
        if (!message.isSet("ID")) {
            STHROW("missing ID");
        }
        PINFO("Received ESCALATE_ABORTED for '" << message["ID"] << "' (" << message["Reason"] << ")");

        // Look for that command
        auto commandIt = _escalatedCommandMap.find(message["ID"]);
        if (commandIt != _escalatedCommandMap.end()) {
            // Re-queue this
            unique_ptr<SQLiteCommand>& command = commandIt->second;
            PINFO("Re-queueing command '" << message["ID"] << "' (" << command->request.methodLine << ") ("
                  << command->id << ")");
            _server.acceptCommand(move(command), false);
            _escalatedCommandMap.erase(commandIt);
        } else
            SWARN("Received ESCALATE_ABORTED for unescalated command " << message["ID"] << ", ignoring.");
    } else if (SIEquals(message.methodLine, "CRASH_COMMAND") || SIEquals(message.methodLine, "BROADCAST_COMMAND")) {
        // Create a new Command and send to the server.
        SData messageCopy = message;
        PINFO("Received " << message.methodLine << " command, forwarding to server.");
        _server.acceptCommand(make_unique<SQLiteCommand>(move(messageCopy)), true);
    } else {
        STHROW("unrecognized message");
    }
}

void SQLiteNode::_onConnect(Peer* peer) {
    SASSERT(peer);
    SASSERTWARN(!SIEquals((*peer)["LoggedIn"], "true"));
    // Send the LOGIN
    PINFO("Sending LOGIN");
    SData login("LOGIN");
    login["Priority"] = to_string(_priority);
    login["State"] = stateName(_state);
    login["Version"] = _version;
    login["Permafollower"] = _originalPriority ? "false" : "true";
    _sendToPeer(peer, login);
}

// --------------------------------------------------------------------------
// On Peer Disconnections
// --------------------------------------------------------------------------
// Whenever a peer disconnects, the following checks are made to verify no
// internal consistency has been lost:  (Technically these checks need only be
// made in certain states, but we'll check them in all states just to be sure.)
void SQLiteNode::_onDisconnect(Peer* peer) {
    SASSERT(peer);
    /// - Verify we don't have any important data buffered for sending to this
    ///   peer.  In particular, make sure we're not sending an ESCALATION_RESPONSE
    ///   because that means the initiating follower's command was successfully
    ///   processed, but it died before learning this.  This won't corrupt the
    ///   database per se (all nodes will still be synchronized, or will repair
    ///   themselves on reconnect), but it means that the data in the database
    ///   is out of touch with reality: we processed a command and reality doesn't
    ///   know it.  Not cool!
    ///
    if (peer->s && peer->s->sendBufferCopy().find("ESCALATE_RESPONSE") != string::npos)
        PWARN("Initiating follower died before receiving response to escalation: " << peer->s->sendBufferCopy());

    /// - Verify we didn't just lose contact with our leader.  This should
    ///   only be possible if we're SUBSCRIBING or FOLLOWING.  If we did lose our
    ///   leader, roll back any uncommitted transaction and go SEARCHING.
    ///
    if (peer == _leadPeer) {
        // We've lost our leader: make sure we aren't waiting for
        // transaction response and re-SEARCH
        PHMMM("Lost our LEADER, re-SEARCHING.");
        SASSERTWARN(_state == SUBSCRIBING || _state == FOLLOWING);
        {
            lock_guard<mutex> leadPeerLock(_leadPeerMutex);
            _leadPeer = nullptr;
        }
        if (!_db.getUncommittedHash().empty()) {
            // We're in the middle of a transaction and waiting for it to
            // approve or deny, but we'll never get its response.  Roll it
            // back and synchronize when we reconnect.
            PHMMM("Was expecting a response for transaction #" << _db.getCommitCount() + 1 << " ("
                                                               << _db.getUncommittedHash()
                                                               << ") but disconnected prematurely; rolling back.");
            _db.rollback();
        }

        // If there were escalated commands, give them back to the server to retry, unless it looks like they were in
        // progress when the leader died, in which case we say they completed with a 500 Error.
        for (auto& cmd : _escalatedCommandMap) {
            _server.acceptCommand(move(cmd.second), false);
        }
        _escalatedCommandMap.clear();
        _changeState(SEARCHING);
    }

    /// - Verify we didn't just lose contact with the peer we're synchronizing
    ///   with.  This should only be possible if we're SYNCHRONIZING.  If we did
    ///   lose our sync peer, give up and go back to SEARCHING.
    ///
    if (peer == _syncPeer) {
        // Synchronization failed
        PHMMM("Lost our synchronization peer, re-SEARCHING.");
        SASSERTWARN(_state == SYNCHRONIZING);
        _syncPeer = nullptr;
        _changeState(SEARCHING);
    }

    // If we're leader, but we've lost quorum, we can't commit anything, nor can worker threads. We need to drop out of
    // a state that implies we can perform commits, and cancel any outstanding commits.
    if (_state == LEADING || _state == STANDINGUP || _state == STANDINGDOWN) {
        int numFullPeers = 0;
        int numLoggedInFullPeers = 0;
        for (auto otherPeer : peerList) {
            // Skip the current peer, it no longer counts.
            if (otherPeer == peer) {
                continue;
            }
            // Make sure we're a full peer
            if (otherPeer->params["Permafollower"] != "true") {
                // Verify we're logged in
                ++numFullPeers;
                if (SIEquals((*otherPeer)["LoggedIn"], "true")) {
                    // Verify we're still fresh
                    ++numLoggedInFullPeers;
                }
            }
        }

        // If we've fallen below the minimum amount of peers required to control the database, we need to stop
        // committing things.
        if (numLoggedInFullPeers * 2 < numFullPeers) {
            // This works for workers, as they block on the state mutex to finish commits, so they've either already
            // completed, or they won't be able to until after this changes, and then they'll see the wrong state.
            //
            // It works for the sync thread as well, as there's handling in _changeState to rollback a commit when
            // dropping out of leading or standing down (and there can't be commits in progress in other states).
            SWARN("We were " << stateName(_state) << " but lost quorum. Going to SEARCHING.");
            _changeState(SEARCHING);
        }
    }
}

void SQLiteNode::_sendToPeer(Peer* peer, const SData& message) {
    SASSERT(peer);
    SASSERT(!message.empty());

    // If a peer is currently disconnected, we can't send it a message.
    if (!peer->s) {
        PWARN("Can't send message to peer, no socket. Message '" << message.methodLine << "' will be discarded.");
        return;
    }
    // Piggyback on whatever we're sending to add the CommitCount/Hash
    SData messageCopy = message;
    messageCopy["CommitCount"] = to_string(_db.getCommitCount());
    messageCopy["Hash"] = _db.getCommittedHash();
    peer->s->send(messageCopy.serialize());
}

void SQLiteNode::_sendToAllPeers(const SData& message, bool subscribedOnly) {
    // Piggyback on whatever we're sending to add the CommitCount/Hash, but only serialize once before broadcasting.
    SData messageCopy = message;
    if (!messageCopy.isSet("CommitCount")) {
        messageCopy["CommitCount"] = SToStr(_db.getCommitCount());
    }
    if (!messageCopy.isSet("Hash")) {
        messageCopy["Hash"] = _db.getCommittedHash();
    }
    const string& serializedMessage = messageCopy.serialize();

    // Loop across all connected peers and send the message
    for (auto peer : peerList) {
        // Send either to everybody, or just subscribed peers.
        if (peer->s && (!subscribedOnly || SIEquals((*peer)["Subscribed"], "true"))) {
            // Send it now, without waiting for the outer event loop
            peer->s->send(serializedMessage);
        }
    }
}

void SQLiteNode::broadcast(const SData& message, Peer* peer) {
    if (peer) {
        SINFO("Sending broadcast: " << message.serialize() << " to peer: " << peer->name);
        _sendToPeer(peer, message);
    } else {
        SINFO("Sending broadcast: " << message.serialize());
        _sendToAllPeers(message, false);
    }
}

void SQLiteNode::_changeState(SQLiteNode::State newState) {
    // Exclusively lock the stateMutex, nobody else will be able to get a shared lock until this is released.
    unique_lock<decltype(stateMutex)> lock(stateMutex);

    // Did we actually change _state?
    State oldState = _state;
    if (newState != oldState) {
        // If we were following, and now we're not, we give up an any replications.
        if (oldState == FOLLOWING) {
            _replicationThreadsShouldExit = true;
            _replicationCV.notify_all();

            // Polling wait for threads to quit.
            while (_replicationThreads) {
                usleep(10'000);
            }

            // Done exiting. Reset so that we can resume FOLLOWING in the future.
            _replicationThreadsShouldExit = false;
        }

        // Depending on the state, set a timeout
        SINFO("Switching from '" << stateName(_state) << "' to '" << stateName(newState) << "'");
        uint64_t timeout = 0;
        if (newState == STANDINGUP) {
            // If two nodes try to stand up simultaneously, they can get in a conflicted state where they're waiting
            // for the other to respond, but neither sends a response. We want a short timeout on this state.
            // TODO: Maybe it would be better to re-send the message indicating we're standing up when we see someone
            // hasn't responded.
            timeout = STIME_US_PER_S * 5 + SRandom::rand64() % STIME_US_PER_S * 5;
        } else if (newState == SEARCHING || newState == SUBSCRIBING) {
            timeout = SQL_NODE_DEFAULT_RECV_TIMEOUT + SRandom::rand64() % STIME_US_PER_S * 5;
        } else if (newState == SYNCHRONIZING) {
            timeout = SQL_NODE_SYNCHRONIZING_RECV_TIMEOUT + SRandom::rand64() % STIME_US_PER_S * 5;
        } else {
            timeout = 0;
        }
        SDEBUG("Setting state timeout of " << timeout / 1000 << "ms");
        _stateTimeout = STimeNow() + timeout;

        // Additional logic for some old states
        if (SWITHIN(LEADING, oldState, STANDINGDOWN) && !SWITHIN(LEADING, newState, STANDINGDOWN)) {
            // If we stop leading, unset _leaderVersion from our own _version.
            // It will get re-set to the version on the new leader.
            _leaderVersion = "";

            // We are no longer leading.  Are we processing a command?
            if (commitInProgress()) {
                // Abort this command
                SWARN("Stopping LEADING/STANDINGDOWN with commit in progress. Canceling.");
                _commitState = CommitState::FAILED;
                _db.rollback();
            }

            // We send any unsent transactions here before we finish switching states, we need to make sure these are
            // all sent to the new leader before we complete the transition.
            _sendOutstandingTransactions();
        }

        // Clear some state if we can
        if (newState < SUBSCRIBING) {
            // We're no longer SUBSCRIBING or FOLLOWING, so we have no leader
            lock_guard<mutex> leadPeerLock(_leadPeerMutex);
            _leadPeer = nullptr;
        }

        // Additional logic for some new states
        if (newState == LEADING) {
            // Seed our last sent transaction.
            {
                SQLITE_COMMIT_AUTOLOCK;
                unsentTransactions.store(false);
                _lastSentTransactionID = _db.getCommitCount();
                // Clear these.
                _db.getCommittedTransactions();
            }
        } else if (newState == STANDINGDOWN) {
            // start the timeout countdown.
            _standDownTimeOut.alarmDuration = STIME_US_PER_S * 30; // 30s timeout before we give up
            _standDownTimeOut.start();

            // Abort all remote initiated commands if no longer LEADING
            // TODO: No we don't, we finish it, as per other documentation in this file.
        } else if (newState == SEARCHING) {
            if (!_escalatedCommandMap.empty()) {
                // This isn't supposed to happen, though we've seen in logs where it can.
                // So what we'll do is try and correct the problem and log the state we're coming from to see if that
                // gives us any more useful info in the future.
                _escalatedCommandMap.clear();
                SWARN(
                    "Switching from '" << stateName(_state) << "' to '" << stateName(newState)
                                       << "' but _escalatedCommandMap not empty. Clearing it and hoping for the best.");
            }
        } else if (newState == WAITING) {
            // The first time we enter WAITING, we're caught up and ready to join the cluster - use our real priority from now on
            _priority = _originalPriority;
        }

        // Send to everyone we're connected to, whether or not
        // we're "LoggedIn" (else we might change state after sending LOGIN,
        // but before we receive theirs, and they'll miss it).
        // Broadcast the new state
        _state = newState;
        SData state("STATE");
        state["StateChangeCount"] = to_string(++_stateChangeCount);
        state["State"] = stateName(_state);
        state["Priority"] = SToStr(_priority);
        _sendToAllPeers(state);
    }
}

void SQLiteNode::_queueSynchronize(Peer* peer, SData& response, bool sendAll) {
    _queueSynchronizeStateless(peer->nameValueMap, name, peer->name, _state, (unsentTransactions.load() ? _lastSentTransactionID : _db.getCommitCount()), _db, response, sendAll);
}

void SQLiteNode::_queueSynchronizeStateless(const STable& params, const string& name, const string& peerName, State _state, uint64_t targetCommit, SQLite& db, SData& response, bool sendAll) {
    // This is a hack to make the PXXXX macros works, since they expect `peer->name` to be defined.
    struct {string name;} peerBase;
    auto peer = &peerBase;
    peerBase.name = peerName;

    // Peer is requesting synchronization.  First, does it have any data?
    uint64_t peerCommitCount = 0;
    if(params.find("CommitCount") != params.end()) {
        peerCommitCount = SToUInt64(params.at("CommitCount"));
    }
    if (peerCommitCount > db.getCommitCount())
        STHROW("you have more data than me");
    if (peerCommitCount) {
        // It has some data -- do we agree on what we share?
        string myHash, ignore;
        if (!db.getCommit(peerCommitCount, ignore, myHash)) {
            PWARN("Error getting commit for peer's commit: " << peerCommitCount << ", my commit count is: " << db.getCommitCount());
            STHROW("error getting hash");
        }
        string compareHash;
        if (params.find("Hash") != params.end()) {
            compareHash = params.at("Hash");
        }
        if (myHash != compareHash) {
            SWARN("Hash mismatch. Peer at commit:" << peerCommitCount << " with hash " << compareHash
                  << ", but we have hash: " << myHash << " for that commit.");
            STHROW("hash mismatch");
        }
        PINFO("Latest commit hash matches our records, beginning synchronization.");
    } else {
        PINFO("Peer has no commits, beginning synchronization.");
    }

    // We agree on what we share, do we need to give it more?
    SQResult result;
    if (peerCommitCount == targetCommit) {
        // Already synchronized; nothing to send
        PINFO("Peer is already synchronized");
        response["NumCommits"] = "0";
    } else {
        // Figure out how much to send it
        uint64_t fromIndex = peerCommitCount + 1;
        uint64_t toIndex = targetCommit;
        if (!sendAll)
            toIndex = min(toIndex, fromIndex + 100); // 100 transactions at a time
        if (!db.getCommits(fromIndex, toIndex, result))
            STHROW("error getting commits");
        if ((uint64_t)result.size() != toIndex - fromIndex + 1)
            STHROW("mismatched commit count");

        // Wrap everything into one huge message
        PINFO("Synchronizing commits from " << peerCommitCount + 1 << "-" << targetCommit);
        response["NumCommits"] = SToStr(result.size());
        for (size_t c = 0; c < result.size(); ++c) {
            // Queue the result
            SASSERT(result[c].size() == 2);
            SData commit("COMMIT");
            commit["CommitIndex"] = SToStr(peerCommitCount + c + 1);
            commit["Hash"] = result[c][0];
            commit.content = result[c][1];
            response.content += commit.serialize();
        }
        SASSERTWARN(response.content.size() < 10 * 1024 * 1024); // Let's watch if it gets over 10MB
    }
}

void SQLiteNode::_recvSynchronize(Peer* peer, const SData& message) {
    SASSERT(peer);
    // Walk across the content and commit in order
    if (!message.isSet("NumCommits"))
        STHROW("missing NumCommits");
    int commitsRemaining = message.calc("NumCommits");
    SData commit;
    const char* content = message.content.c_str();
    int messageSize = 0;
    int remaining = (int)message.content.size();
    while ((messageSize = commit.deserialize(content, remaining))) {
        // Consume this message and process
        // **FIXME: This could be optimized to commit in one huge transaction
        content += messageSize;
        remaining -= messageSize;
        if (!SIEquals(commit.methodLine, "COMMIT"))
            STHROW("expecting COMMIT");
        if (!commit.isSet("CommitIndex"))
            STHROW("missing CommitIndex");
        if (commit.calc64("CommitIndex") < 0)
            STHROW("invalid CommitIndex");
        if (!commit.isSet("Hash"))
            STHROW("missing Hash");
        if (commit.content.empty())
            SALERT("Synchronized blank query");
        if (commit.calcU64("CommitIndex") != _db.getCommitCount() + 1)
            STHROW("commit index mismatch");

        // This block repeats until we successfully commit, or throw out of it.
        // This allows us to retry in the event we're interrupted for a checkpoint. This should only happen once,
        // because the second try will be blocked on the checkpoint.
        while (true) {
            try {
                _db.waitForCheckpoint();
                if (!_db.beginTransaction()) {
                    STHROW("failed to begin transaction");
                }

                // Inside a transaction; get ready to back out if an error
                if (!_db.writeUnmodified(commit.content)) {
                    STHROW("failed to write transaction");
                }
                if (!_db.prepare()) {
                    STHROW("failed to prepare transaction");
                }

                // Done, break out of `while (true)`.
                break;
            } catch (const SException& e) {
                // Transaction failed, clean up
                SERROR("Can't synchronize (" << e.what() << "); shutting down.");
                // **FIXME: Remove the above line once we can automatically handle?
                _db.rollback();
                throw e;
            } catch (const SQLite::checkpoint_required_error& e) {
                _db.rollback();
                SINFO("[checkpoint] Retrying synchronize after checkpoint.");
            }
        }

        // Transaction succeeded, commit and go to the next
        SDEBUG("Committing current transaction because _recvSynchronize: " << _db.getUncommittedQuery());
        _db.commit();
        if (_db.getCommittedHash() != commit["Hash"])
            STHROW("potential hash mismatch");
        --commitsRemaining;
    }

    // Did we get all our commits?
    if (commitsRemaining)
        STHROW("commits remaining at end");
}

void SQLiteNode::_updateSyncPeer()
{
    Peer* newSyncPeer = nullptr;
    uint64_t commitCount = _db.getCommitCount();
    for (auto peer : peerList) {
        // If either of these conditions are true, then we can't use this peer.
        if (!peer->test("LoggedIn") || peer->calcU64("CommitCount") <= commitCount) {
            continue;
        }

        // Any peer that makes it to here is a usable peer, so it's by default better than nothing.
        if (!newSyncPeer) {
            newSyncPeer = peer;
        }
        // If the previous best peer and this one have the same latency (meaning they're probably both 0), the best one
        // is the one with the highest commit count.
        else if (newSyncPeer->latency == peer->latency) {
            if (peer->calc64("CommitCount") > newSyncPeer->calc64("CommitCount")) {
                newSyncPeer = peer;
            }
        }
        // If the existing best has no latency, then this peer is faster (because we just checked if they're equal and
        // 0 is the slowest latency value).
        else if (newSyncPeer->latency == 0) {
            newSyncPeer = peer;
        }
        // Finally, if this peer is faster than the best, but not 0 itself, it's the new best.
        else if (peer->latency != 0 && peer->latency < newSyncPeer->latency) {
            newSyncPeer = peer;
        }
    }

    // Log that we've changed peers.
    if (_syncPeer != newSyncPeer) {
        string from, to;
        if (_syncPeer) {
            from = _syncPeer->name + " (commit count=" + (*_syncPeer)["CommitCount"] + "), latency="
                                   + to_string(_syncPeer->latency/1000) + "ms";
        } else {
            from = "(NONE)";
        }
        if (newSyncPeer) {
            to = newSyncPeer->name + " (commit count=" + (*newSyncPeer)["CommitCount"] + "), latency="
                                   + to_string(newSyncPeer->latency/1000) + "ms";
        } else {
            to = "(NONE)";
        }

        // We see strange behavior when choosing peers. Peers are being chosen from distant data centers rather than
        // peers on the same LAN. This is extra diagnostic info to try and see why we don't choose closer ones.
        list<string> nonChosenPeers;
        for (auto peer : peerList) {
            if (peer == newSyncPeer || peer == _syncPeer) {
                continue; // These ones we're already logging.
            } else if (!peer->test("LoggedIn")) {
                nonChosenPeers.push_back(peer->name + ":!loggedIn");
            } else if (peer->calcU64("CommitCount") <= commitCount) {
                nonChosenPeers.push_back(peer->name + ":commit=" + (*peer)["CommitCount"]);
            } else {
                nonChosenPeers.push_back(peer->name + ":" + to_string(peer->latency/1000) + "ms");
            }
        }
        SINFO("Updating SYNCHRONIZING peer from " << from << " to " << to << ". Not chosen: " << SComposeList(nonChosenPeers));

        // And save the new sync peer internally.
        _syncPeer = newSyncPeer;
    }
}

void SQLiteNode::_reconnectPeer(Peer* peer) {
    // If we're connected, just kill the connection
    if (peer->s) {
        // Reset
        SHMMM("Reconnecting to '" << peer->name << "'");
        shutdownSocket(peer->s);
        (*peer)["LoggedIn"] = "false";
    }
}

void SQLiteNode::_reconnectAll() {
    // Loop across and reconnect
    for (auto peer : peerList) {
        _reconnectPeer(peer);
    }
}

bool SQLiteNode::_majoritySubscribed() {
    // Count up how may full and subscribed peers we have (A "full" peer is one that *isn't* a permafollower).
    int numFullPeers = 0;
    int numFullFollowers = 0;
    for (auto peer : peerList) {
        if (peer->params["Permafollower"] != "true") {
            ++numFullPeers;
            if (peer->test("Subscribed")) {
                ++numFullFollowers;
            }
        }
    }

    // Done!
    return (numFullFollowers * 2 >= numFullPeers);
}

bool SQLiteNode::peekPeerCommand(SQLiteNode* node, SQLite& db, SQLiteCommand& command)
{
    Peer* peer = nullptr;
    try {
        if (SIEquals(command.request.methodLine, "SYNCHRONIZE")) {
            peer = node->getPeerByID(SToUInt64(command.request["peerID"]));
            if (!peer) {
                // There's nobody to send to, but this was a valid command that's been handled.
                return true;
            }
            command.response.methodLine = "SYNCHRONIZE_RESPONSE";
            _queueSynchronizeStateless(command.request.nameValueMap,
                                       command.request["name"],
                                       command.request["peerName"],
                                       node->_state,
                                       SToUInt64(command.request["targetCommit"]),
                                       db,
                                       command.response,
                                       false);

            // The following two lines are copied from `_sendToPeer`.
            command.response["CommitCount"] = to_string(db.getCommitCount());
            command.response["Hash"] = db.getCommittedHash();
            peer->sendMessage(command.response);
            return true;
        }
    } catch (const SException& e) {
        if (peer) {
            // Any failure causes the response to in initiate a reconnect, if we got a peer.
            command.response.methodLine = "RECONNECT";
            command.response["Reason"] = e.what();
            peer->sendMessage(command.response);
        }

        // If we even got here, then it must have been a peer command, so we'll call it complete.
        return true;
    }
    return false;
}

void SQLiteNode::handleBeginTransaction(SQLite& db, Peer* peer, const SData& message) {
    AutoScopedWallClockTimer timer(_syncTimer);

    // BEGIN_TRANSACTION: Sent by the LEADER to all subscribed followers to begin a new distributed transaction. Each
    // follower begins a local transaction with this query and responds APPROVE_TRANSACTION. If the follower cannot start
    // the transaction for any reason, it is broken somehow -- disconnect from the leader.
    // **FIXME**: What happens if LEADER steps down before sending BEGIN?
    // **FIXME**: What happens if LEADER steps down or disconnects after BEGIN?
    bool success = true;
    uint64_t leaderSentTimestamp = message.calcU64("leaderSendTime");
    uint64_t followerDequeueTimestamp = STimeNow();
    if (!message.isSet("ID")) {
        STHROW("missing ID");
    }
    if (!message.isSet("NewCount")) {
        STHROW("missing NewCount");
    }
    if (!message.isSet("NewHash")) {
        STHROW("missing NewHash");
    }
    if (_state != FOLLOWING) {
        STHROW("not following");
    }
    if (!db.getUncommittedHash().empty()) {
        STHROW("already in a transaction");
    }

    // This block repeats until we successfully commit, or error out of it.
    // This allows us to retry in the event we're interrupted for a checkpoint. This should only happen once,
    // because the second try will be blocked on the checkpoint.
    while (true) {
        try {
            db.waitForCheckpoint();
            if (!db.beginTransaction()) {
                STHROW("failed to begin transaction");
            }

            // Inside transaction; get ready to back out on error
            if (!db.writeUnmodified(message.content)) {
                STHROW("failed to write transaction");
            }
            if (!db.prepare()) {
                STHROW("failed to prepare transaction");
            }

            // Successful commit; we in the right state?
            if (db.getUncommittedHash() != message["NewHash"]) {
                // Something is screwed up
                PWARN("New hash mismatch: command='" << message["Command"] << "', commitCount=#" << db.getCommitCount()
                      << "', committedHash='" << db.getCommittedHash() << "', uncommittedHash='"
                      << db.getUncommittedHash() << "', messageHash='" << message["NewHash"] << "', uncommittedQuery='"
                      << db.getUncommittedQuery() << "'");
                STHROW("new hash mismatch");
            }

            // Done, break out of `while (true)`.
            break;
        } catch (const SException& e) {
            // Something caused a write failure.
            success = false;
            db.rollback();

            // This is a fatal error case.
            break;
        } catch (const SQLite::checkpoint_required_error& e) {
            db.rollback();
            SINFO("[checkpoint] Retrying beginTransaction after checkpoint.");
        }
    }

    // Are we participating in quorum?
    if (_priority) {
        // If the ID is /ASYNC_\d+/, no need to respond, leader will ignore it anyway.
        string verb = success ? "APPROVE_TRANSACTION" : "DENY_TRANSACTION";
        if (!SStartsWith(message["ID"], "ASYNC_")) {
            // Not a permafollower, approve the transaction
            PINFO(verb << " #" << db.getCommitCount() + 1 << " (" << message["NewHash"] << ").");
            SData response(verb);
            response["NewCount"] = SToStr(db.getCommitCount() + 1);
            response["NewHash"] = success ? db.getUncommittedHash() : message["NewHash"];
            response["ID"] = message["ID"];
            lock_guard<mutex> leadPeerLock(_leadPeerMutex);
            if (!_leadPeer) {
                STHROW("no leader?");
            }
            _sendToPeer(_leadPeer, response);
        } else {
            PINFO("Skipping " << verb << " for ASYNC command.");
        }
    } else {
        PINFO("Would approve/deny transaction #" << db.getCommitCount() + 1 << " (" << db.getUncommittedHash()
              << ") for command '" << message["Command"] << "', but a permafollower -- keeping quiet.");
    }
    uint64_t transitTimeUS = followerDequeueTimestamp - leaderSentTimestamp;
    uint64_t applyTimeUS = STimeNow() - followerDequeueTimestamp;
    float transitTimeMS = (float)transitTimeUS / 1000.0;
    float applyTimeMS = (float)applyTimeUS / 1000.0;
    PINFO("Replicated transaction " << message.calcU64("NewCount") << ", sent by leader at " << leaderSentTimestamp
          << ", transit/dequeue time: " << transitTimeMS << "ms, applied in: " << applyTimeMS << "ms, should COMMIT next.");
}

void SQLiteNode::handleCommitTransaction(SQLite& db, Peer* peer, const uint64_t commandCommitCount, const string& commandCommitHash) {
    AutoScopedWallClockTimer timer(_syncTimer);

    // COMMIT_TRANSACTION: Sent to all subscribed followers by the leader when it determines that the current
    // outstanding transaction should be committed to the database. This completes a given distributed transaction.
    if (_state != FOLLOWING) {
        STHROW("not following");
    }
    if (db.getUncommittedHash().empty()) {
        STHROW("no outstanding transaction");
    }
    if (commandCommitCount != db.getCommitCount() + 1) {
        STHROW("commit count mismatch. Expected: " + to_string(commandCommitCount) + ", but would actually be: "
              + to_string(db.getCommitCount() + 1));
    }
    if (commandCommitHash != db.getUncommittedHash()) {
        STHROW("hash mismatch:" + commandCommitHash + "!=" + db.getUncommittedHash() + ";");
    }

    SDEBUG("Committing current transaction because COMMIT_TRANSACTION: " << db.getUncommittedQuery());
    db.commit();

    // Clear the list of committed transactions. We're following, so we don't need to send these.
    db.getCommittedTransactions();

    // Log timing info.
    // TODO: This is obsolete and replaced by timing info in BedrockCommand. This should be removed.
    uint64_t beginElapsed, readElapsed, writeElapsed, prepareElapsed, commitElapsed, rollbackElapsed;
    uint64_t totalElapsed = db.getLastTransactionTiming(beginElapsed, readElapsed, writeElapsed, prepareElapsed,
                                                         commitElapsed, rollbackElapsed);
    SINFO("Committed follower transaction #" << to_string(commandCommitCount) << " (" << commandCommitHash << ") in "
          << totalElapsed / 1000 << " ms (" << beginElapsed / 1000 << "+"
          << readElapsed / 1000 << "+" << writeElapsed / 1000 << "+"
          << prepareElapsed / 1000 << "+" << commitElapsed / 1000 << "+"
          << rollbackElapsed / 1000 << "ms)");
    _handledCommitCount++;
    if (_handledCommitCount % 5000 == 0) {
        // Log how much time we've spent handling 5000 commits.
        auto timingInfo = _syncTimer.getStatsAndReset();
        SINFO("Over the last 5000 commits, (total: " << _handledCommitCount << ") " << timingInfo.second.count() << "/" << timingInfo.first.count() << "ms spent in replication");
    }
}

void SQLiteNode::handleRollbackTransaction(SQLite& db, Peer* peer, const SData& message) {
    AutoScopedWallClockTimer timer(_syncTimer);
    // ROLLBACK_TRANSACTION: Sent to all subscribed followers by the leader when it determines that the current
    // outstanding transaction should be rolled back. This completes a given distributed transaction.
    if (!message.isSet("ID")) {
        STHROW("missing ID");
    }
    if (_state != FOLLOWING) {
        STHROW("not following");
    }
    if (db.getUncommittedHash().empty()) {
        SINFO("Received ROLLBACK_TRANSACTION with no outstanding transaction.");
    }
    db.rollback();
}

SQLiteNode::State SQLiteNode::leaderState() const {
    lock_guard<mutex> leadPeerLock(_leadPeerMutex);
    if (_leadPeer) {
        return _leadPeer.load()->state;
    }
    return State::UNKNOWN;
}
