#!/bin/bash

alias c++=em++
alias g++=em++
alias ar=emar
alias cc=emcc
alias gcc=emcc
alias cmake=emcmake
alias configure=emconfigure
alias ranlib=emranlib
export CC=emcc
export CXX=em++
source /emsdk/emsdk_env.sh
cd /opendht
cmake -DOPENDHT_PYTHON=OFF
make -j8
