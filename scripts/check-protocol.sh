#!/usr/bin/env bash
# Fails if protocol/defs changed without regenerating outputs.
set -euo pipefail
cd "$(dirname "$0")/../protocol"
uv run protogen check
