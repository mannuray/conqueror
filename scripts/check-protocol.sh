#!/usr/bin/env bash
# Fails if protocol/defs changed without regenerating outputs.
set -euo pipefail
# The repo from this script's own folder: inside a hook, git rev-parse --show-toplevel gives the current folder
# in a worktree.
top="$(cd "$(dirname "$0")/.." && pwd)"
cd "$top/protocol"
uv run protogen check
cd "$top/logo"
uv run logogen check
