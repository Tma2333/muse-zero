#!/bin/bash
# Build the C14 release artifact (MB_RELEASE_BUILD=1: no FAKE_RUN op,
# no auto-exit, build id mb-1.0-rel-1) and stage it under
# dist/release/ with its SHA-256. The dev config is always restored,
# even on build failure.
set -eu
cd "$(dirname "$0")/.."
CFG=core/bridge_build_config.h
cp "$CFG" /tmp/bridge_build_config.h.bak
restore() { cp /tmp/bridge_build_config.h.bak "$CFG"; }
trap restore EXIT
sed -i 's/#define MB_RELEASE_BUILD 0/#define MB_RELEASE_BUILD 1/' "$CFG"
grep -q '#define MB_RELEASE_BUILD 1' "$CFG"
.venv/bin/ufbt build
mkdir -p dist/release
cp .ufbt/build/muse_bridge.fap dist/release/muse_bridge.fap
( cd dist/release && sha256sum muse_bridge.fap > muse_bridge.fap.sha256 )
cat dist/release/muse_bridge.fap.sha256
echo "release artifact staged in dist/release/"
