#!/bin/bash
# Rename *_ko.md files to *.ko.md for mkdocs-static-i18n compatibility
# Usage: ./scripts/rename_i18n.sh [--dry-run]

set -euo pipefail
DRY_RUN="${1:-}"

find docs/ -name '*_ko.md' | while read -r f; do
  new="${f%_ko.md}.ko.md"
  if [ "$DRY_RUN" = "--dry-run" ]; then
    echo "[dry-run] $f -> $new"
  else
    mv "$f" "$new"
    echo "renamed: $f -> $new"
  fi
done
