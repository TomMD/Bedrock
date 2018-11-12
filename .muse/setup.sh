#!/bin/bash

apt install -y g++-6 gcc-6
update-alternatives --remove-all g++
update-alternatives --remove-all gcc
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-6 10
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-6 10
hash -r
# bear make
# rm Makefile


