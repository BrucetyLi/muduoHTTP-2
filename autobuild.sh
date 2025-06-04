#!/bin/bash

set -e
cd `pwd`/build
rm -rf *
cmake ..
make 
cd ..
