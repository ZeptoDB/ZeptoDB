# Community Setup Guide

Instructions for setting up ZeptoDB's community channels.

---

## 1. Discord Server

### Create Server

1. Go to https://discord.com → Create Server → "For a club or community"
2. Server name: **ZeptoDB**
3. Upload logo from `docs/assets/`

### Channel Structure

```
📢 INFORMATION
  #announcements      — releases, blog posts (read-only)
  #rules              — code of conduct link
  #roadmap            — link to BACKLOG.md

💬 COMMUNITY
  #general            — general discussion
  #show-and-tell      — share what you built with ZeptoDB
  #help               — usage questions

🛠️ DEVELOPMENT
  #contributing       — PR discussion, dev questions
  #bugs               — bug reports (redirect to GitHub Issues)
  #feature-requests   — ideas (redirect to GitHub Issues)

📊 USE CASES
  #finance-hft        — trading, quant, crypto
  #iot-manufacturing  — sensors, OPC-UA, MQTT
  #observability      — metrics, logs, monitoring
```

### Roles

| Role | Color | Permissions |
|------|-------|-------------|
| `@maintainer` | Red | Admin |
| `@contributor` | Blue | Manage messages in dev channels |
| `@community` | Green | Default for verified members |

### Bot Setup

- Add **GitHub bot** → webhook to `#announcements` for releases
- Add **MEE6** or **Carl-bot** for auto-role on join

### Invite Link

- Create permanent invite: `https://discord.gg/zeptodb`
- Add to: README.md, docs site footer, website

---

## 2. GitHub Discussions

1. Go to repo Settings → Features → Enable Discussions
2. Categories:
   - **Announcements** (maintainers only)
   - **Q&A** (answered format)
   - **Ideas** (feature proposals)
   - **Show and Tell** (community projects)
   - **General** (open discussion)

---

## 3. Social Accounts

| Platform | Handle | Purpose |
|----------|--------|---------|
| Twitter/X | `@zeptodb` | Release announcements, benchmarks, tips |
| LinkedIn | ZeptoDB | Enterprise audience, hiring |
| YouTube | ZeptoDB | Demo videos, conference talks |

---

## 4. Community Badges for README

```markdown
[![Discord](https://img.shields.io/discord/DISCORD_SERVER_ID?color=5865F2&logo=discord&logoColor=white&label=Discord)](https://discord.gg/zeptodb)
[![GitHub Discussions](https://img.shields.io/github/discussions/zeptodb/zeptodb?logo=github)](https://github.com/zeptodb/zeptodb/discussions)
[![Twitter Follow](https://img.shields.io/twitter/follow/zeptodb?style=social)](https://twitter.com/zeptodb)
```

Replace `DISCORD_SERVER_ID` with actual server ID after creation.
