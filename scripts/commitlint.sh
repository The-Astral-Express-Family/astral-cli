#!/bin/sh
# commitlint.sh - lightweight Conventional Commits gate for astral-cli.
#
# Pure POSIX sh: runs unchanged on Linux, macOS, and Windows (Git for
# Windows bundles its own sh). No node/python required.
#
# Usage:
#   scripts/commitlint.sh <commit-message-file>   # as a commit-msg hook
#   scripts/commitlint.sh --message "..."         # ad-hoc check
#   ... | scripts/commitlint.sh -                 # read from stdin
#
# Escape hatch (emergencies only): git commit --no-verify

set -eu

TYPES='feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert'
SUBJECT_MAX=100

usage() {
  echo "usage: commitlint.sh <file> | --message <text> | - (stdin)" >&2
}

fail() {
  printf 'commit message rejected: %s\n\n' "$1"
  printf '  expected form (Conventional Commits):\n'
  printf '    <type>(<optional scope>)<!>: <subject>\n'
  printf '  types: %s\n' "$(echo "$TYPES" | tr '|' ' ')"
  printf '  example: feat(todo): add fuzzy ranking behind --fuzzy\n\n'
  printf '  bypass (emergencies only): git commit --no-verify\n'
  exit 1
}

msg_file=''
msg_inline=''

case "${1-}" in
  '') usage; exit 64 ;;
  -) msg_file='-' ;;
  --message|-m)
    [ "${2-}" ] || { usage; exit 64; }
    msg_inline=$2
    ;;
  -h|--help) usage; exit 0 ;;
  *) msg_file=$1 ;;
esac

if [ -n "$msg_inline" ]; then
  msg=$msg_inline
elif [ "$msg_file" = '-' ]; then
  msg=$(cat)
else
  [ -f "$msg_file" ] || { printf 'commitlint: cannot read %s\n' "$msg_file" >&2; exit 64; }
  msg=$(cat "$msg_file")
fi

# First line is the subject.
subject=$(printf '%s\n' "$msg" | { read -r first || true; printf '%s' "$first"; })

[ -n "$subject" ] || fail 'empty subject'

# Automation-generated commits pass through.
case $subject in
  'Merge '*|'Initial commit'*|'Revert "'*) exit 0 ;;
esac

# type(scope)!?: subject   - scope is [a-z0-9._/-], breaking-change '!' allowed.
pattern="^(${TYPES})(\([a-z0-9][a-z0-9._/-]*\))?!?: .+"
printf '%s' "$subject" | grep -Eq "$pattern" \
  || fail "subject '$subject' does not match Conventional Commits"

# Length budget for the subject line.
length=${#subject}
[ "$length" -le "$SUBJECT_MAX" ] \
  || fail "subject is $length chars (max $SUBJECT_MAX): tighten it"

# No trailing period.
case $subject in
  *.) fail 'subject ends with a period - drop it' ;;
esac

# If there is a body, it must be separated by one blank line.
if [ "$msg" != "$subject" ]; then
  second_line=$(printf '%s\n' "$msg" | sed -n '2p')
  [ -z "$second_line" ] || fail 'body must be separated from the subject by a blank line'
fi

exit 0
