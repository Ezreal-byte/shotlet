#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

HELP_OUTPUT="$("$REPO_DIR/build.sh" --help)"

grep -q -- "--app" <<< "$HELP_OUTPUT"
grep -q -- "--node" <<< "$HELP_OUTPUT"
grep -q -- "--tests" <<< "$HELP_OUTPUT"
grep -q -- "--all" <<< "$HELP_OUTPUT"
grep -q -- "Commands:" <<< "$HELP_OUTPUT"
grep -q -- "Outputs:" <<< "$HELP_OUTPUT"
grep -q -- "build/native/darwin-arm64/shotlet.node" <<< "$HELP_OUTPUT"
grep -q -- "build/native/darwin-x64/shotlet.node" <<< "$HELP_OUTPUT"

if "$REPO_DIR/build.sh" --macos-arm64 >/tmp/shotlet-build-script-test.out 2>&1; then
    echo "expected --macos-arm64 to be rejected"
    exit 1
fi

grep -q -- "unknown argument '--macos-arm64'" /tmp/shotlet-build-script-test.out
