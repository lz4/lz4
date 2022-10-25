#!/bin/sh

set -e
set -x

datagen -g4200MB | lz4 -v3BD | lz4 -qt
