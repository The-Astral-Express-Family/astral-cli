# One-time setup for Windows users who prefer PowerShell.
# Git for Windows runs hooks with its bundled sh, so scripts/commitlint.sh
# works unmodified; this script only points core.hooksPath at the repo.

$root = Split-Path -Parent $PSScriptRoot
git -C $root config core.hooksPath scripts/git-hooks
Write-Host "git hooks installed (core.hooksPath=scripts/git-hooks)"
