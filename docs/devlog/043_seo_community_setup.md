# Devlog 043 — SEO & Community Setup

Date: 2026-04-02

## What

Set up community infrastructure and SEO/discoverability assets for ZeptoDB (P2 backlog items).

## Created

### Community Infrastructure
- `CONTRIBUTING.md` — contributor guide (build, test, code style, PR guidelines)
- `CODE_OF_CONDUCT.md` — Contributor Covenant 2.1
- `.github/ISSUE_TEMPLATE/bug_report.md` — structured bug report template
- `.github/ISSUE_TEMPLATE/feature_request.md` — feature request template
- `.github/ISSUE_TEMPLATE/performance_issue.md` — performance regression template
- `.github/FUNDING.yml` — GitHub Sponsors configuration

### Community Guides
- `docs/community/COMMUNITY_SETUP.md` — Discord server setup guide (channels, roles, bots)
- `docs/community/REGISTRY_SUBMISSIONS.md` — ready-to-submit content for Awesome lists, DB-Engines, DBDB, AlternativeTo
- `docs/community/LAUNCH_POSTS.md` — Show HN, Reddit (r/programming, r/cpp, r/algotrading, r/selfhosted) post drafts with timing strategy

### README Renewal
- Refreshed `README.md` with:
  - More badges (with logo icons, docs link)
  - Navigation links (Quick Start, Performance, SQL, Docs, Contributing)
  - GIF demo placeholder (commented, ready for asciinema recording)
  - ASCII architecture diagram
  - Emoji section headers for scannability
  - Community section (commented, ready for Discord link)
  - Updated test count (830+)
  - Cleaner table formatting

## Next Steps (Manual)

1. Create Discord server following `docs/community/COMMUNITY_SETUP.md`
2. Enable GitHub Discussions in repo settings
3. Submit Awesome Time-Series DB PR per `docs/community/REGISTRY_SUBMISSIONS.md`
4. Submit DB-Engines registration form
5. Record demo GIF with asciinema
6. Publish Docker Hub image (prerequisite for launch posts)
7. Post Show HN when Docker image + website are ready
