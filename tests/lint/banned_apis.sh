#!/usr/bin/env bash
# Fails if code that runs inside the simulator uses something that breaks
# determinism: wall clocks, threads, <random>, floating point, or standard
# library functions whose output differs between libc++ and libstdc++.
#
# usage: banned_apis.sh <dir>...              scan, exit 1 on any hit
#        banned_apis.sh --selftest <dir>...   every code line in <dir> must hit
set -euo pipefail

selftest=0
if [[ "${1:-}" == "--selftest" ]]; then
  selftest=1
  shift
fi

patterns=(
  '#[[:space:]]*include[[:space:]]*<(chrono|thread|random|mutex|atomic|future|condition_variable|ctime)>'
  '(^|[^[:alnum:]_.>])(std::)?(rand|srand|time|getenv|clock_gettime|gettimeofday)[[:space:]]*\('
  '(^|[^[:alnum:]_])(float|double)([^[:alnum:]_]|$)'
  'std::(uniform_int_distribution|uniform_real_distribution|shuffle|random_device|mt19937|hash[[:space:]]*<)'
)

status=0
for dir in "$@"; do
  [[ -d "$dir" ]] || continue
  while IFS= read -r -d '' file; do
    # Strip // comments so prose like "double-check" does not trip the lint.
    stripped="$(sed 's://.*$::' "$file")"
    hits=""
    for p in "${patterns[@]}"; do
      h="$(printf '%s\n' "$stripped" | grep -nE "$p" || true)"
      [[ -n "$h" ]] && hits+="$h"$'\n'
    done
    if [[ $selftest -eq 1 ]]; then
      code_lines="$(printf '%s\n' "$stripped" | grep -nvE '^[[:space:]]*$' | cut -d: -f1 | sort -u)"
      hit_lines="$(printf '%s' "$hits" | grep -v '^$' | cut -d: -f1 | sort -u || true)"
      missed="$(comm -23 <(printf '%s\n' "$code_lines") <(printf '%s\n' "$hit_lines"))"
      if [[ -n "$missed" ]]; then
        echo "selftest: lint missed $file lines: $(echo $missed)"
        status=1
      fi
    elif [[ -n "$hits" ]]; then
      printf '%s' "$hits" | grep -v '^$' | sed "s|^|$file:|"
      status=1
    fi
  done < <(find "$dir" -type f \( -name '*.h' -o -name '*.cc' \) -print0 | sort -z)
done

exit $status
