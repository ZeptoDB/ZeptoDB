# Devlog 028: Repository Structure Cleanup

**Date:** 2026-03-25
**Status:** ✅ Complete

---

## Summary

The root directory had grown bloated with 30+ items, so we reorganized by role. Deployment/infra files were consolidated into `deploy/`, scripts were separated by purpose, AI agent configs were isolated into `.ai/`, and build outputs were unified under a single `build/` directory.

## Before

```
zeptodb/
├── Dockerfile              # Deployment
├── k8s/                    # Deployment
├── helm/                   # Deployment
├── monitoring/             # Deployment
├── scripts/                # Ops + CI + hooks mixed together
│   ├── tune_bare_metal.sh
│   ├── backup.sh
│   ├── hooks/pre-push
│   ├── check_english_first.sh
│   └── ...
├── build/                  # GCC build
├── build_clang/            # Clang build
├── dist/                   # Python build artifacts
├── apex_db.egg-info/       # Python build artifacts
├── .pytest_cache/          # Test cache
├── ec2-jinmp.pem           # ⚠️ Secret key
├── AGENTS.md               # AI config
├── SOUL.md                 # AI config
├── USER.md, IDENTITY.md... # AI config (8 files)
├── memory/                 # AI memory
└── ... (30+ root items)
```

## After

```
zeptodb/
├── src/                    # C++ source
├── include/                # C++ headers
├── tests/                  # Tests
├── zepto_py/               # Python bindings
├── tools/                  # C++ CLI (zepto-cli, zepto-migrate)
├── examples/               # Examples
├── third_party/            # External dependencies
├── docs/                   # Documentation
├── deploy/                 # 🆕 Deployment consolidated
│   ├── docker/Dockerfile
│   ├── k8s/
│   ├── helm/zeptodb/
│   ├── monitoring/
│   └── scripts/            # Ops scripts
├── .ai/                    # 🆕 AI agent config
├── .github/
│   ├── workflows/
│   └── scripts/            # 🆕 CI check scripts
├── .githooks/              # 🆕 git hooks
├── build/                  # Single build directory
├── CMakeLists.txt
├── CMakePresets.json        # 🆕 Build presets
├── pyproject.toml
├── mkdocs.yml
├── LICENSE
├── README.md
├── README_ko.md
├── BACKLOG.md
└── COMPLETED.md
```

## Changes

### 1. Deployment files → `deploy/`

| Before | After |
|--------|-------|
| `Dockerfile` | `deploy/docker/Dockerfile` |
| `k8s/` | `deploy/k8s/` |
| `helm/` | `deploy/helm/` |
| `monitoring/` | `deploy/monitoring/` |

### 2. Scripts separated by purpose → `scripts/` folder removed

| Before | After | Reason |
|--------|-------|--------|
| `scripts/tune_bare_metal.sh` | `deploy/scripts/` | Ops/deployment |
| `scripts/backup.sh`, `restore.sh` | `deploy/scripts/` | Ops/deployment |
| `scripts/install_service.sh`, `zeptodb.service` | `deploy/scripts/` | Ops/deployment |
| `scripts/eod_process.sh` | `deploy/scripts/` | Ops/deployment |
| `scripts/ai_tune_bare_metal.py` | `deploy/scripts/` | Ops/deployment |
| `scripts/hooks/pre-push` | `.githooks/pre-push` | git convention |
| `scripts/check_english_first.sh` | `.github/scripts/` | CI |
| `scripts/check_docs_updated.sh` | `.github/scripts/` | CI |
| `scripts/rename_i18n.sh` | `.github/scripts/` | CI |

### 3. AI agent config → `.ai/`

`AGENTS.md`, `SOUL.md`, `USER.md`, `IDENTITY.md`, `TOOLS.md`, `HEARTBEAT.md`, `CLAUDE.md`, `KIRO.md`, `memory/` → `.ai/`

`CLAUDE.md` and `KIRO.md` remain as symlinks in root (tools expect them at root).

### 4. Build unified → single `build/`

| Before | After |
|--------|-------|
| `build/` (GCC) | `build/` (switch compilers via CMakePresets) |
| `build_clang/` (Clang) | Removed |
| `dist/`, `apex_db.egg-info/` | Removed |
| `.pytest_cache/` | Removed |

Added `CMakePresets.json`:
```bash
cmake --preset default   # System compiler
cmake --preset clang     # clang-19
cmake --preset debug     # Debug build
```

### 5. Security

`ec2-jinmp.pem` → moved to `/home/ec2-user/` (outside repo). `*.pem` already included in `.gitignore`.

### 6. Documentation reference updates

Bulk-updated path references across 30+ files:
- `k8s/` → `deploy/k8s/`
- `helm/` → `deploy/helm/`
- `Dockerfile` → `deploy/docker/Dockerfile`
- `scripts/*.sh` → `deploy/scripts/*.sh`
- CI workflow → `.github/scripts/`

`/opt/zeptodb/scripts/` (absolute path on deployed servers) was not changed — that's the path on the deployed server.

## .gitignore

```
build/
*.o *.a *.so *.pem
__pycache__/ .pytest_cache/ site/
.ai/ .claude/ .kiro/ .openclaw/
CLAUDE.md KIRO.md
dist/ *.egg-info/
```

## Rationale

- Root items reduced from 30+ → 20
- External contributors won't be confused by AI config files
- Everything deployment-related can be found by looking at `deploy/` alone
- Prevents the problem of multiple build directories proliferating
- Resolved the security issue of a secret key existing in the repo
