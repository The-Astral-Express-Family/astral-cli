#!/bin/sh
# One-time setup: points git at the repo-managed hooks in scripts/git-hooks
# so the commitlint gate is versioned with the code (no per-clone copies).

set -eu
root=$(cd "$(dirname "$0")/.." && pwd)

chmod +x "$root/scripts/commitlint.sh" 2>/dev/null || true
chmod +x "$root/scripts/git-hooks/commit-msg" 2>/dev/null || true

git -C "$root" config core.hooksPath scripts/git-hooks

echo "git hooks installed (core.hooksPath=scripts/git-hooks)"
echo "try it: echo 'bad message' | sh scripts/commitlint.sh -"
