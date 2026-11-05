#!/usr/bin/env bash
# Select the Rust/C profiles together; successful compilation is not qualification.
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")" && pwd)/release-build.py" "$@"
