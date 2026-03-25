#!/bin/bash
# CI check: feature PRs must include docs or backlog changes.
# Runs on: changed file list from git diff (PR base..head)
#
# Exit 0 = pass, Exit 1 = fail
#
# Usage in CI:
#   git diff --name-only origin/main...HEAD | bash scripts/check_docs_updated.sh

set -euo pipefail

CHANGED_FILES=$(cat -)

# Skip check for docs-only or fix/refactor PRs (detected by no src/ changes)
if ! echo "$CHANGED_FILES" | grep -qE '^(src/|include/|tests/)'; then
  echo "✅ No source changes — docs check skipped."
  exit 0
fi

# Check if any doc-related file was touched
if echo "$CHANGED_FILES" | grep -qE '^(docs/|BACKLOG\.md|README\.md|CHANGELOG\.md)'; then
  echo "✅ Documentation updated."
  exit 0
fi

echo "❌ Source files changed but no documentation updated."
echo ""
echo "Feature PRs must update at least one of:"
echo "  - BACKLOG.md"
echo "  - docs/ (any file)"
echo "  - README.md"
echo "  - CHANGELOG.md"
echo ""
echo "If this is a trivial fix, add 'skip-docs-check' label to the PR."
exit 1
