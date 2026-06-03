#!/usr/bin/env bash
# tools/discard_spike.sh — sanctioned discard of a throwaway spike branch + its
# worktree (TW-02). The destructive_guard blocks ad-hoc `git branch -D`; this
# reviewed script is the allowed path, BUT it REFUSES any branch not under
# `spike/` so it can never delete a real (`main`/`impl/*`/`waveD/*`) branch.
#
# Usage: tools/discard_spike.sh spike/<name>
set -euo pipefail

b="${1:-}"
case "$b" in
  spike/*) ;;
  *)
    echo "discard_spike: refuses '$b' — only spike/* branches may be discarded" >&2
    exit 2
    ;;
esac

# If a worktree is checked out on the branch, remove it first (spike work is
# throwaway, so --force is intended).
wt="$(git worktree list --porcelain \
  | awk -v want="refs/heads/$b" '/^worktree /{w=$2} $0=="branch "want{print w}')"
if [ -n "$wt" ]; then
  git worktree remove --force "$wt"
fi

git branch -D "$b"
echo "discarded spike branch $b${wt:+ (+worktree $wt)}"
