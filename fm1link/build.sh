#!/bin/sh
# SYRUP LINK's build: app/build/link.fwsc (or link-X.Y.fwsc with --release X.Y).
# Needs the JieLi toolchain ($JIELI_TOOLCHAIN) and the AC79 SDK ($AC79_SDK): see base/README.md.
set -eu
cd "$(dirname "$0")"
exec python3 base/tools/build.py app "$@"
