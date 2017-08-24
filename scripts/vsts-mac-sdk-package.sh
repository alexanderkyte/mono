#!/bin/bash -e

set -e

make -C external/bockbuild/builds/tiff-4.0.8-x86 clean || true
make -C external/bockbuild/builds/tiff-4.0.8-x64 clean || true


MONO_HASH=$1

git checkout $MONO_HASH && \
external/bockbuild/bb MacSDKRelease --verbose --package --arch darwin-universal --trace 2>&1 | tee build.log && \
mv packaging/MacSDKRelease/MonoFramework-MDK-*.pkg ./MacSDKRelease.pkg


