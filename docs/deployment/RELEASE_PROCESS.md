# Release Process

See [Branch and Release Policy](BRANCH_RELEASE_POLICY.md) for the governing
branch, promotion, and ruleset policy. This document describes the release
mechanics that implement that policy.

ZeptoDB uses `dev` as the integration branch and `main` as the release branch.
All normal development pushes should target `dev`. Promoting code to `main`
starts the release automation.

## Branch Flow

1. Push feature work to `dev` or to a short-lived branch that targets `dev`.
2. Run the required CI and review checks against `dev`.
3. Promote `dev` to `main` only when the repository is ready to publish a new
   version.
4. Do not push directly to `main` from local development. The repository
   pre-push hook blocks `refs/heads/main` unless `ZEPTO_ALLOW_MAIN_PUSH=1` is
   set for an explicit release-admin override.

## Automatic Versioning

`Version Main Release` runs on every push to `main`.

The workflow:

1. Fetches all `vMAJOR.MINOR.PATCH` tags.
2. Reads the current project versions from:
   - `CMakeLists.txt`
   - `zepto_py/__init__.py`
   - `web/package.json`
3. Compares the highest tag version with the highest checked-in version file.
4. Uses the checked-in version when it is ahead of the latest tag; otherwise
   bumps the latest tag's patch component.
5. Commits the synchronized version files to `main` with
   `chore(release): vX.Y.Z`.
6. Creates tag `vX.Y.Z`.
7. Dispatches the `Release` workflow against that tag.

This keeps binary artifact names, Docker tags, GitHub releases, and the Python
package version aligned.

## Release Workflow

The existing `Release` workflow still owns publishing:

- Linux amd64 and arm64 binary archives.
- Docker image tags `zeptodb/zeptodb:X.Y.Z` and `zeptodb/zeptodb:latest`.
- GitHub Release notes and downloadable archives.
- PyPI package publishing through trusted publishing.
- Homebrew tap update dispatch.

The workflow can be triggered either by a `v*` tag push or manually through
`workflow_dispatch` using a `v*` tag ref.

## Repository Settings

The pipeline expects these GitHub settings:

- The repository secret `RELEASE_BOT_TOKEN` is configured with permission to
  push to `main`, create `v*` tags, and dispatch workflows. This repository is
  under an organization policy that disables write permissions for the default
  `GITHUB_TOKEN`, so the release workflow cannot rely on `github.token`.
- `main` and `v*` rulesets allow the release bot account to bypass release
  automation writes. The current repository ruleset uses repository-admin
  bypass, so the token owner must have admin permission or the ruleset must be
  updated to name a narrower bot actor.
- Required release secrets are present:
  - `RELEASE_BOT_TOKEN`
  - `DOCKERHUB_USERNAME`
  - `DOCKERHUB_TOKEN`
  - `SITE_DEPLOY_TOKEN`
- PyPI trusted publishing is configured for the `pypi` environment.
- The self-hosted ARM64 runner used by `Graviton ARM64 Build & Test` is online.

## Version Policy

The default automatic bump is patch-only. Minor or major releases should be
prepared by manually updating the version files on `dev` before promoting to
`main`; the automation will publish that higher checked-in version exactly.

## Active Rulesets

The repository is configured with these active rules:

- `Dev branch safety`: protects `dev` from deletion and non-fast-forward
  updates.
- `Main release branch`: protects `main` from deletion and non-fast-forward
  updates, requires a pull request, requires one approving review, dismisses
  stale reviews on push, and requires review-thread resolution.
- `Release tags`: protects `v*` tags from unauthorized creation, deletion, and
  non-fast-forward updates.
