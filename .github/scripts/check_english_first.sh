#!/bin/bash
# CI check: every .ko.md must have a corresponding English original.
# Usage: bash scripts/check_english_first.sh

set -euo pipefail

EXIT_CODE=0

find docs/ -name '*.ko.md' | while read -r ko_file; do
  en_file="${ko_file%.ko.md}.md"
  if [ ! -f "$en_file" ]; then
    echo "❌ Missing English original: $en_file (found: $ko_file)"
    EXIT_CODE=1
  fi
done

if [ "$EXIT_CODE" -eq 0 ]; then
  echo "✅ All Korean translations have English originals."
fi
exit $EXIT_CODE
