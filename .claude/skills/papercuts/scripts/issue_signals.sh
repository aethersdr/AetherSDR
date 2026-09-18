#!/usr/bin/env bash
# issue_signals.sh — read-only signal dump for /papercuts triage.
#
# Usage:
#   issue_signals.sh --since YYYY-MM-DD       # all open issues created on/after date
#   issue_signals.sh 3505 3326 3815 ...       # a specific set of issue numbers
#   issue_signals.sh                          # default: open issues, last ~2 months
#
# Env: REPO (default aethersdr/AetherSDR), OUTDIR (default: a fresh mktemp dir).
#
# Emits to stdout a TSV: NUM  STATE  PRIO  THUMBS  COMMENTS  AUTHOR  LABELS  TITLE
# and writes two files (paths printed on stderr):
#   $OUTDIR/issues.json   — raw list incl. bodies, for the scoring fan-out
#   $OUTDIR/signals.tsv   — the same TSV as stdout
#
# Why gh issue list (not gh issue view per-issue): issue/comment bodies carry
# raw control characters that make `jq` choke when fetched one at a time. The
# list endpoint is well-behaved, and we sanitize the stream anyway (strip C0
# control bytes except TAB/LF/CR) so a stray byte never breaks the run.

set -euo pipefail

REPO="${REPO:-aethersdr/AetherSDR}"
OUTDIR="${OUTDIR:-$(mktemp -d -t papercuts.XXXXXX)}"
mkdir -p "$OUTDIR"
RAW="$OUTDIR/issues.json"
TSV="$OUTDIR/signals.tsv"

# Default window: ~2 months back from today (portable date math).
default_since() {
  date -u -v-60d +%Y-%m-%d 2>/dev/null || date -u -d '60 days ago' +%Y-%m-%d
}

SINCE=""
NUMS=()
NUMS_N=0   # tracked separately: bash < 4.4 (macOS /bin/bash) treats
           # ${#NUMS[@]} on an empty array as unbound under `set -u`.
while [ $# -gt 0 ]; do
  case "$1" in
    --since)
      [ $# -ge 2 ] || { echo "--since requires YYYY-MM-DD" >&2; exit 2; }
      SINCE="$2"; shift 2 ;;
    --since=*) SINCE="${1#*=}"; shift ;;
    *[!0-9]*|'') echo "invalid argument: $1" >&2; exit 2 ;;
    *) NUMS+=("$1"); NUMS_N=$((NUMS_N + 1)); shift ;;
  esac
done
[ -z "$SINCE" ] && [ "$NUMS_N" -eq 0 ] && SINCE="$(default_since)"

if [ -n "$SINCE" ] && ! [[ "$SINCE" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  echo "--since requires YYYY-MM-DD" >&2; exit 2
fi
if [ -n "$SINCE" ] && [ "$NUMS_N" -gt 0 ]; then
  echo "use --since or issue numbers, not both" >&2; exit 2
fi

# Strip C0 control chars except TAB(09) LF(0A) CR(0D) so jq never trips.
sanitize() { tr -d '\000-\010\013\014\016-\037'; }

QUERY_ARGS=()
if [ -n "$SINCE" ]; then
  QUERY_ARGS=(--search "created:>=$SINCE")
fi
gh issue list --repo "$REPO" --state open --limit 600 ${QUERY_ARGS[@]+"${QUERY_ARGS[@]}"} \
  --json number,title,labels,author,state,comments,reactionGroups,createdAt,body,assignees \
  | sanitize > "$RAW"

if [ "$(jq length "$RAW")" -ge 600 ]; then
  echo "warning: retrieval cap (600) reached; results may be incomplete" >&2
fi

# Build the filter: by explicit numbers if given, else by createdAt window.
if [ "$NUMS_N" -gt 0 ]; then
  FILTER='[.[] | select(.number as $n | $nums | index($n))]'
  NUMS_JSON="$(printf '%s\n' "${NUMS[@]}" | jq -s '.')"
  jq --argjson nums "$NUMS_JSON" "$FILTER" "$RAW" > "$RAW.tmp" && mv "$RAW.tmp" "$RAW"
else
  jq --arg since "$SINCE" '[.[] | select(.createdAt >= $since)]' "$RAW" \
    > "$RAW.tmp" && mv "$RAW.tmp" "$RAW"
fi

if [ "$NUMS_N" -gt 0 ]; then
  MISSING="$(jq -c --argjson nums "$NUMS_JSON" '$nums - [.[].number]' "$RAW")"
  if [ "$MISSING" != '[]' ]; then
    echo "warning: requested issues not returned: $MISSING; verify live state separately" >&2
  fi
fi

# Emit the signal TSV, sorted by number.
jq -r '
  sort_by(.number) | .[] |
  [ (.number|tostring),
    .state,
    ([.labels[].name] | map(select(startswith("priority:"))) | (.[0]//"none") | sub("priority: ";"")),
    (([.reactionGroups[]?|select(.content=="THUMBS_UP")|.users.totalCount]|add)//0|tostring),
    ((.comments|length)|tostring),
    .author.login,
    ([.labels[].name] | map(select(. as $l | ["claude-active","awaiting-response","insufficient-info","no-claude","aetherclaude-eligible"] | index($l))) | join(",")),
    (.title[0:60])
  ] | @tsv
' "$RAW" | tee "$TSV"

COUNT="$(jq length "$RAW")"
# A shape-valid but non-existent date (2026-02-30, 2026-13-01) is accepted by
# the search API and returns nothing, which reads like an empty backlog.
if [ "$COUNT" -eq 0 ] && [ -n "$SINCE" ]; then
  echo "warning: no open issues created on/after $SINCE; check the date is real" >&2
fi

echo "issues.json: $RAW"   >&2
echo "signals.tsv: $TSV"   >&2
echo "count: $COUNT" >&2
