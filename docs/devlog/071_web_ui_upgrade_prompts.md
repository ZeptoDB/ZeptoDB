# Devlog 071 — Web UI Upgrade Prompts for Gated Enterprise Features

Date: 2026-04-15

## Summary

Added license-aware upgrade prompts to the Web UI. Community edition users see a clear
upgrade card with a link to pricing when navigating to Enterprise-only pages (Cluster,
Tenants). The sidebar shows an "Enterprise" chip next to gated menu items.

## Changes

### New Files
- `web/src/components/UpgradeCard.tsx` — Reusable gated-feature card with lock icon and "View Plans" button linking to zeptodb.com/pricing
- `web/src/lib/useLicense.ts` — `useLicense()` React hook + `hasFeature()` helper

### Modified Files
- `web/src/lib/api.ts` — Added `LicenseInfo` interface and `fetchLicense()` calling `GET /api/license`
- `web/src/app/cluster/page.tsx` — Gated with `hasFeature(license, "cluster")`; shows UpgradeCard on Community
- `web/src/app/tenants/page.tsx` — Gated with `hasFeature(license, "advanced_rbac")`; shows UpgradeCard on Community
- `web/src/components/Sidebar.tsx` — Added `gatedFeature` field to NavItem; Cluster and Tenants show MUI `Chip label="Enterprise"` when feature unavailable; items remain clickable

## Design Decisions

- License is fetched once on mount via `useLicense()` hook (no polling — edition doesn't change at runtime)
- Gated pages still render their data-fetching hooks but return the UpgradeCard before the main JSX — this keeps the component simple
- Sidebar items remain navigable so users can see what they're missing (the page itself shows the upgrade prompt)
- `hasFeature()` returns `false` when license is `null` (loading/error), which means the gate shows briefly on first load — acceptable UX tradeoff for simplicity
