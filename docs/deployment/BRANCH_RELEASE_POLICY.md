# Branch and Release Policy

Version: 0.1
Effective date: 2026-06-13
Status: Initial policy

## Purpose

This policy makes `dev` the normal integration branch and keeps `main` as the
release branch. The goal is to let development move quickly on `dev` while
ensuring that every update to `main` intentionally creates a versioned release.

## Branch Roles

| Ref | Role | Policy |
| --- | --- | --- |
| `dev` | Integration branch | Default branch. All normal pushes and pull requests target `dev`. |
| `main` | Release branch | Promotion-only branch. Updating `main` starts release automation. |
| `v*` tags | Release tags | Immutable release identifiers created by release automation or an approved release admin. |

## Development Rules

1. All normal development work targets `dev`.
2. Feature branches may be used, but their pull request base should be `dev`.
3. Direct local pushes to `main` are blocked by the repository pre-push hook.
4. Force-pushes and deletions are not allowed on `dev`.
5. `dev` can receive direct pushes from maintainers for fast-moving integration
   work, but production promotion still happens through `main`.

## Release Rules

1. `main` is updated only when a new release should be created.
2. `main` requires a pull request, one approving review, stale review dismissal,
   and review-thread resolution.
3. Pushing to `main` starts `Version Main Release`.
4. The release workflow chooses the next version from checked-in version files
   and existing `vMAJOR.MINOR.PATCH` tags.
5. The workflow synchronizes `CMakeLists.txt`, `zepto_py/__init__.py`, and
   `web/package.json`.
6. The workflow creates and pushes `vX.Y.Z`, which triggers the tag-based
   `Release` workflow.
7. `v*` tags are protected from unauthorized creation, deletion, and
   non-fast-forward updates.

## Version Policy

Patch releases are automatic by default.

If the checked-in version files are ahead of the latest release tag, that
checked-in version is published exactly. Otherwise, the latest tag's patch
component is incremented.

Minor or major releases must be prepared on `dev` by updating the version files
before promoting to `main`.

## Required Repository Settings

The active repository configuration must include:

- Default branch: `dev`.
- Ruleset `Dev branch safety`:
  - Target: `refs/heads/dev`.
  - Rules: deletion protection, non-fast-forward protection.
- Ruleset `Main release branch`:
  - Target: `refs/heads/main`.
  - Rules: deletion protection, non-fast-forward protection, pull request
    requirement, one approving review, stale review dismissal, review-thread
    resolution.
- Ruleset `Release tags`:
  - Target: `refs/tags/v*`.
  - Rules: creation protection, deletion protection, non-fast-forward
    protection.

## Release Bot Policy

The organization currently disables write permission for the default
`GITHUB_TOKEN`. Because of that, `Version Main Release` must use
`RELEASE_BOT_TOKEN`.

The token owner must be a release bot or release-admin account with permission
to:

- Push the generated version commit to `main`.
- Create `v*` release tags.
- Bypass the `main` and `v*` rulesets only for release automation.

The first-pass ruleset uses repository-admin bypass. After a dedicated release
bot account or GitHub App is chosen, narrow the bypass actor from
repository-admin to that bot.

## Operating Procedure

For normal work:

1. Work on `dev` or a feature branch based on `dev`.
2. Push to `dev`.
3. Let CI run.

For a release:

1. Confirm `dev` is ready.
2. Open a promotion pull request from `dev` to `main`.
3. Review and merge after required checks and review are complete.
4. Confirm `Version Main Release` creates the version commit and `v*` tag.
5. Confirm the tag-triggered `Release` workflow publishes artifacts.

## Current Follow-ups

- Add `RELEASE_BOT_TOKEN` as a repository secret.
- Replace repository-admin bypass with a narrower release bot bypass after the
  bot identity is selected.
- Decide whether `main` should require named CI checks once the required check
  list is stable.
