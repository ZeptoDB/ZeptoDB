# Devlog 028: Repository Structure Cleanup

**Date:** 2026-03-25
**Status:** ✅ Complete

---

## Summary

루트 디렉토리가 30+ 항목으로 비대해져서 역할별로 정리. 배포/인프라 파일을 `deploy/`로 통합, 스크립트를 용도별 분리, AI 에이전트 설정을 `.ai/`로 격리, 빌드 출력을 `build/` 하나로 통일.

## Before

```
zeptodb/
├── Dockerfile              # 배포
├── k8s/                    # 배포
├── helm/                   # 배포
├── monitoring/             # 배포
├── scripts/                # 운영 + CI + hooks 혼재
│   ├── tune_bare_metal.sh
│   ├── backup.sh
│   ├── hooks/pre-push
│   ├── check_english_first.sh
│   └── ...
├── build/                  # GCC 빌드
├── build_clang/            # Clang 빌드
├── dist/                   # Python 빌드 잔재
├── apex_db.egg-info/       # Python 빌드 잔재
├── .pytest_cache/          # 테스트 캐시
├── ec2-jinmp.pem           # ⚠️ 비밀 키
├── AGENTS.md               # AI 설정
├── SOUL.md                 # AI 설정
├── USER.md, IDENTITY.md... # AI 설정 (8개 파일)
├── memory/                 # AI 메모리
└── ... (30+ root items)
```

## After

```
zeptodb/
├── src/                    # C++ 소스
├── include/                # C++ 헤더
├── tests/                  # 테스트
├── zepto_py/               # Python 바인딩
├── tools/                  # C++ CLI (zepto-cli, zepto-migrate)
├── examples/               # 예제
├── third_party/            # 외부 의존성
├── docs/                   # 문서
├── deploy/                 # 🆕 배포 관련 통합
│   ├── docker/Dockerfile
│   ├── k8s/
│   ├── helm/zeptodb/
│   ├── monitoring/
│   └── scripts/            # 운영 스크립트
├── .ai/                    # 🆕 AI 에이전트 설정
├── .github/
│   ├── workflows/
│   └── scripts/            # 🆕 CI 체크 스크립트
├── .githooks/              # 🆕 git hooks
├── build/                  # 단일 빌드 디렉토리
├── CMakeLists.txt
├── CMakePresets.json        # 🆕 빌드 프리셋
├── pyproject.toml
├── mkdocs.yml
├── LICENSE
├── README.md
├── README_ko.md
├── BACKLOG.md
└── COMPLETED.md
```

## Changes

### 1. 배포 파일 → `deploy/`

| Before | After |
|--------|-------|
| `Dockerfile` | `deploy/docker/Dockerfile` |
| `k8s/` | `deploy/k8s/` |
| `helm/` | `deploy/helm/` |
| `monitoring/` | `deploy/monitoring/` |

### 2. 스크립트 용도별 분리 → `scripts/` 폴더 제거

| Before | After | 이유 |
|--------|-------|------|
| `scripts/tune_bare_metal.sh` | `deploy/scripts/` | 운영/배포 |
| `scripts/backup.sh`, `restore.sh` | `deploy/scripts/` | 운영/배포 |
| `scripts/install_service.sh`, `zeptodb.service` | `deploy/scripts/` | 운영/배포 |
| `scripts/eod_process.sh` | `deploy/scripts/` | 운영/배포 |
| `scripts/ai_tune_bare_metal.py` | `deploy/scripts/` | 운영/배포 |
| `scripts/hooks/pre-push` | `.githooks/pre-push` | git 컨벤션 |
| `scripts/check_english_first.sh` | `.github/scripts/` | CI |
| `scripts/check_docs_updated.sh` | `.github/scripts/` | CI |
| `scripts/rename_i18n.sh` | `.github/scripts/` | CI |

### 3. AI 에이전트 설정 → `.ai/`

`AGENTS.md`, `SOUL.md`, `USER.md`, `IDENTITY.md`, `TOOLS.md`, `HEARTBEAT.md`, `CLAUDE.md`, `KIRO.md`, `memory/` → `.ai/`

`CLAUDE.md`, `KIRO.md`는 루트에 symlink 유지 (도구가 루트에서 찾음).

### 4. 빌드 통합 → `build/` 하나

| Before | After |
|--------|-------|
| `build/` (GCC) | `build/` (CMakePresets로 컴파일러 전환) |
| `build_clang/` (Clang) | 삭제 |
| `dist/`, `apex_db.egg-info/` | 삭제 |
| `.pytest_cache/` | 삭제 |

`CMakePresets.json` 추가:
```bash
cmake --preset default   # 시스템 컴파일러
cmake --preset clang     # clang-19
cmake --preset debug     # Debug 빌드
```

### 5. 보안

`ec2-jinmp.pem` → `/home/ec2-user/`로 이동 (repo 밖). `.gitignore`에 `*.pem` 이미 포함.

### 6. 문서 참조 업데이트

30+ 파일에서 경로 참조 일괄 업데이트:
- `k8s/` → `deploy/k8s/`
- `helm/` → `deploy/helm/`
- `Dockerfile` → `deploy/docker/Dockerfile`
- `scripts/*.sh` → `deploy/scripts/*.sh`
- CI workflow → `.github/scripts/`

`/opt/zeptodb/scripts/` (설치 후 절대 경로)는 변경하지 않음 — 이건 배포된 서버의 경로.

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

- 루트 항목 30+ → 20개로 감소
- 외부 기여자가 봤을 때 AI 설정 파일에 혼란받지 않음
- `deploy/` 하나만 보면 배포 관련 전부 파악 가능
- 빌드 디렉토리가 여러 개 생기는 문제 원천 차단
- 비밀 키가 repo에 존재하는 보안 이슈 해결
