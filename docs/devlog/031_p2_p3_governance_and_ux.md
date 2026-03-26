# Devlog 031: P2/P3 Governance and UX Improvements

## Overview
ZeptoDB's management capabilities were expanded to cover enterprise-level governance (P2) and refined UX/UI configurations including Dark Mode and Marketing Pages (P3).

## What Was Done

### 1. Backend C++ API Additions (`http_server.cpp`)
- **Tenant Management**: Added `GET /admin/tenants`, `POST /admin/tenants`, and `DELETE /admin/tenants/:id` to securely interface with `TenantManager`.
- **API Key Usage**: Added `GET /admin/keys/:id/usage` endpoint to track key activity metadata, linking `last_used_ns` and `allowed_symbols`.
- **Server Settings**: Exposed a read-only endpoint `GET /admin/settings` to observe configuration flags including query timeout limits, active TLS layers, cluster presence, and auth enforcement.

### 2. Frontend React Client (`Next.js`)
- **API Client Layer**: Defined hooks (`fetchTenants`, `createTenant`, `deleteTenant`, `fetchKeyUsage`, `fetchSessions`, `fetchSettings`) in `lib/api.ts`.
- **Admin Dashboard Extensions**:
  - Restructured `app/admin/page.tsx` with comprehensive tabs.
  - **Tenants Tab**: Monitor resource boundaries and quota limits in real-time.
  - **Roles Tab**: An interactive matrix dictating action allowances (DDL/DML/Admin) mapped strictly across `admin`, `writer`, `reader`, `analyst`, and `metrics`.
  - **Sessions Tab**: Visibility into currently active connected client IP pools.
  - **Audit Features**: Fully responsive client-side filtering system on top of the backend Audit buffer allowing for wildcard search, temporal correlation, and CSV exportation.

### 3. UX Formatting & Aesthetics
- **Design System Transition**: Consolidated CSS configurations inside `theme.ts` to accommodate context-aware palettes (`dark` vs `light`).
- **Dark Mode Toggle**: Deployed a dynamic switch using React's Context API injected via `TopBar.tsx`, allowing fluid contrast switching.
- **TopBar Visuals**: Moved away from default Material UI chips to custom-styled, glowing Neon Dot SVGs reflecting `healthy`/`unhealthy` data connectivity.
- **Sidebar Scoping**: Hardened component conditions to completely respect RBAC boundaries (e.g. `metrics` users solely rendering the Dashboard element).

### 4. Marketing Layouts Integration
- Created boilerplate routes underneath a Next.js 13 route group `(marketing)`.
- Scaffolding `/home`, `/features`, and `/pricing` with matching color variables, optimizing for immediate production-ready delivery.

## Testing & Verifications
- C++ Engine was verified through explicit backend `ninja` compilations tracking zero duplicate linker references.
- React Frontend passed Turbopack builds enforcing zero TypeScript violations or undefined linter discrepancies.
- Added comprehensive unit tests targeting Governance constructs within `test_auth.cpp`.
