#!/bin/bash -e

make -C external/bockbuild/builds/tiff-4.0.8-x86 clean || true
make -C external/bockbuild/builds/tiff-4.0.8-x64 clean || true

external/bockbuild/bb MacSDK --package
