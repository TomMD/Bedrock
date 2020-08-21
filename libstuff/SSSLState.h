#pragma once
#include <libstuff/libstuff.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

struct SSSLState {
    // Attributes
    int s;
    mbedtls_entropy_context ec;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;

    SSSLState();
    ~SSSLState();
};

// SSL helpers
extern SSSLState* SSSLOpen(int s, SX509* x509);
extern int SSSLSend(SSSLState* ssl, const char* buffer, int length);
extern int SSSLSend(SSSLState* ssl, const SBuffer& buffer);
extern bool SSSLSendConsume(SSSLState* ssl, SBuffer& sendBuffer);
extern bool SSSLSendAll(SSSLState* ssl, const string& buffer);
extern int SSSLRecv(SSSLState* ssl, char* buffer, int length);
extern bool SSSLRecvAppend(SSSLState* ssl, SBuffer& recvBuffer);
extern string SSSLGetState(SSSLState* ssl);
extern void SSSLShutdown(SSSLState* ssl);
extern void SSSLClose(SSSLState* ssl);
