#!/usr/bin/env bash
echo "GXX=g++" >> Makefile.new
echo "CC=gcc" >> Makefile.new
cat Makefile >> Makefile.new
mv Makefile.new Makefile
