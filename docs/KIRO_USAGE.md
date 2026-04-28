# Built with Kiro — ZeptoDB Case Study

> How I use **Kiro** (AI coding assistant, IDE + CLI) to develop, review, and
> test a ~100K LOC C++20 time-series database.
>
> Project: **ZeptoDB** — in-memory columnar DB, Highway SIMD, LLVM JIT, 830+ tests.
> Project site / docs: **[zeptodb.com](https://zeptodb.com)**
> Scope of this doc: what I built **on top of Kiro's IDE / CLI features**, and what I learned.

---

## TL;DR

I did **not** start from zero — I added new features (distributed cluster work, SQL DML, licensing, feed handlers) to an existing large C++ codebase using Kiro. To keep velocity high on a repo with 20+ subsystems and 100+ design docs, I leaned heavily on **three Kiro features**:

1. **Multi-agent orchestration** — an orchestrator delegates to developer / reviewer / QA subagents.
2. **MCP + Skills** — reusable, keyword-activated knowledge packs (design-doc index, edge-case catalog, cross-arch verification, etc.).
3. **Persistent context (`.kiro/` save & load)** — project rules, per-agent context, and settings that load automatically on every session.

All three are declaratively configured in the `.kiro/` directory, versioned with the code, and survive across sessions.

For the full ZeptoDB project (architecture, benchmarks, docs), see **[zeptodb.com](https://zeptodb.com)**.

---

## Feature 1 — Multi-Agent Orchestration

### What it is

Kiro lets me define multiple named agents, each with their own prompt, tool allowlist, and context files. One agent (the orchestrator) can spawn others in a pipeline (parallel or serial) using the `subagent` tool.

### How I use it in ZeptoDB

I split development into **four roles**, each a separate agent config in `.kiro/agents/`:

| Agent | File | Role | Tools allowed |
|-------|------|------|----------------|
| `zeptodb-dev` | `zeptodb.json` | Orchestrator / planner — **does not write code** | All (delegates via `subagent`) |
| `zepto-developer` | `zepto-developer.json` | Writes C++ / Python, updates docs | `fs_read`, `fs_write`, `execute_bash`, `grep`, `glob`, `code` |
| `zepto-reviewer` | `zepto-reviewer.json` | Code review, doc-consistency check | `fs_read`, `grep`, `glob`, `code` (read-only) |
| `zepto-qa` | `zepto-qa.json` | Writes tests, runs `ninja`, verifies x86_64 + aarch64 | `fs_read`, `fs_write`, `execute_bash`, ... |

The orchestrator is enforced as the default via `.kiro/settings.json`:

```json
{
  "project.instructions": ".kiro/KIRO.md",
  "chat.defaultAgent": "zeptodb-dev"
}
```

### Typical workflow for a new feature

```
User: "Add table-scoped partitioning"
        │
        ▼
┌──────────────────────────────────────────────────────┐
│ zeptodb-dev (orchestrator)                           │
│  1. Reads docs/design/layer1_storage_memory.md       │
│  2. Breaks into tasks, picks next devlog number (083)│
│  3. Delegates ▼                                      │
└──────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────┐
│ zepto-developer     │  implements in include/zeptodb/storage/,
│                     │  updates COMPLETED.md + devlog/083_*.md
└─────────────────────┘
        │
        ├────────────────────┬─────────────────────┐
        ▼                    ▼                     ▼
┌─────────────────┐  ┌─────────────────┐   (parallel)
│ zepto-reviewer  │  │ zepto-qa        │
│ doc ↔ code diff │  │ ninja + gtest   │
│ severity labels │  │ x86 + aarch64   │
└─────────────────┘  └─────────────────┘
        │                    │
        └──────────┬─────────┘
                   ▼
        orchestrator synthesizes
        (what was done · review verdict · test results)
```

### Why this is worth the setup cost

- **Tool safety via allowlist** — the reviewer agent physically cannot write files. Every review is guaranteed read-only.
- **Each agent loads only its own context** — the developer doesn't load QA's edge-case catalog, keeping the prompt focused.
- **Parallel verification** — reviewer and QA run at the same time via the orchestrator's `subagent` tool, cutting iteration time.
- **Deterministic handoffs** — each subagent call must include design-doc path, changed files, and acceptance criteria (see `.kiro/context/orchestrator.md`). No "vibes-based" delegation.

### Concrete example — devlog `082_table_scoped_partitioning.md`

The feature touched `include/zeptodb/storage/partition_manager.h`, `src/storage/`, the SQL executor, and four docs. Running it through the pipeline produced:

- developer diff (minimal, follows `layer1_storage_memory.md`)
- reviewer report flagging one missing `SQL_REFERENCE.md` update → fixed
- QA report: 14 new unit tests, ninja clean on x86_64, then re-run on Graviton via `tools/run-aarch64-tests.sh`

All in one orchestrator-driven session. The devlog itself lives under the project docs at **[zeptodb.com](https://zeptodb.com)**.

---

## Feature 2 — MCP & Skills (Reusable Knowledge Packs)

### What it is

Kiro supports two extension mechanisms:

- **MCP (Model Context Protocol) servers** — external processes that expose tools (e.g. a browser via `@playwright/mcp`). Configured in `.kiro/settings/mcp.json`.
- **Skills** — keyword-activated Markdown files (`SKILL.md`) that Kiro loads into context **only when relevant**. Configured via `resources: ["skill://.kiro/skills/**/SKILL.md"]` in each agent JSON.

### How I use it in ZeptoDB

#### MCP servers

```json
// .kiro/settings/mcp.json
{
  "mcpServers": {
    "playwright": {
      "command": "npx",
      "args": ["@playwright/mcp@latest"]
    }
  }
}
```

The Playwright MCP lets the agent drive a headless browser to smoke-test the Next.js Web UI (`web/`) — screenshotting `/admin`, verifying the login flow — without me writing any glue code.

#### Skills — six project-specific + one vendored

Living in `.kiro/skills/` (and one symlink to `.agents/skills/`):

| Skill | When it activates | Purpose |
|-------|-------------------|---------|
| `design-doc-index` | Analyzing a new request | Maps keywords → `docs/design/*.md` → `include/zeptodb/*` code path |
| `layer-patterns` | Modifying a specific layer | Idiomatic allocation, concurrency, extension recipes per layer |
| `edge-case-catalog` | Writing tests | 10 categories (empty, boundary, concurrency, network, memory, time, cluster, failover, platform…) with test-snippet hints |
| `doc-update-checklist` | After any code change | Prevents doc-code drift — enumerates which docs must be updated |
| `review-checklist-by-layer` | During review | Per-layer pitfalls (storage, execution, cluster…) |
| `cross-arch-verification` | Before declaring a build done | x86_64 vs aarch64 (Graviton) build + SIMD bit-exactness check, EKS bench cluster workflow |
| `cpp-coding-standards` | Writing C++ | Vendored from a public repo, pinned in `skills-lock.json` (SHA-256) |

Each `SKILL.md` has a YAML frontmatter `description` field that Kiro's router uses to decide whether to auto-load it — so the orchestrator's prompt stays lean, and domain knowledge is pulled in on demand.

### Why I moved knowledge into skills instead of the main prompt

- **Token budget** — 6 skills × ~5KB would bloat every agent call if inlined. Lazy-loading keeps small requests small.
- **Separation of concerns** — the QA agent uses `edge-case-catalog` and `cross-arch-verification`; the developer ignores them. Same repo, different activations.
- **Versionable + auditable** — skills live in git. A PR that adds a new pitfall to `review-checklist-by-layer/SKILL.md` is just a Markdown diff.
- **Vendor-locked integrity** — external skills are pinned via `skills-lock.json` with a computed hash, so a supply-chain tamper would fail the lock check.

---

## Feature 3 — Persistent Context (`.kiro/` Save & Load)

### What it is

Kiro loads a fixed set of files into every session **before** the user's message:

- `project.instructions` (set to `.kiro/KIRO.md` in `settings.json`) — the canonical project rulebook.
- Per-agent `resources` declared in each `.kiro/agents/*.json` — context files + skill globs.
- Skill `SKILL.md` files, auto-activated by keyword.

All of this is **stored in-repo** and reloaded on every new chat. I don't have to paste "here are the coding rules" ever again.

### How I use it in ZeptoDB

#### Top-level: `.kiro/KIRO.md` (12 KB)

The single source of truth for agents. Contains:

- Core rules (doc ↔ code consistency, tests required, build verification)
- Full **document structure map** — every `docs/design/*.md`, `docs/api/*.md`, `docs/operations/*.md` with a one-line description
- Code structure summary (`include/zeptodb/` layer tree)
- Logging rules (when to use `ZEPTO_INFO` vs `util::Logger` vs `AuditBuffer`)
- Backlog summary table
- Multi-agent workflow diagram
- Known issues ("do not 'fix' the missing `#include <vector>` in `fix_parser.h` — it's a pre-existing non-bug")

Because this is always loaded, every agent — even a fresh session — knows:
- what the project is,
- where the design docs are,
- which docs to update when code changes,
- and the house style for logging, tests, and devlog numbering.

#### Per-agent: `.kiro/context/*.md`

Role-specific context, separated so each agent only loads what it needs:

```
.kiro/context/
├── orchestrator.md    # delegation decision matrix, what to pass to each subagent
├── developer.md       # design-doc → code mapping, build commands, doc checklist
├── reviewer.md        # severity levels, common pitfalls, output format
└── qa.md              # test strategy, edge cases, EKS bench workflow, report format
```

#### Settings: `.kiro/settings.json`

```json
{ "project.instructions": ".kiro/KIRO.md", "chat.defaultAgent": "zeptodb-dev" }
```

Two lines, and every new chat in this repo automatically (a) loads `KIRO.md` as system instructions, (b) starts with the orchestrator selected.

### Directory layout at a glance

```
.kiro/
├── KIRO.md                  # project rulebook, auto-loaded
├── settings.json            # project.instructions + default agent
├── MCP.json                 # (empty for now — placeholder)
├── settings/
│   ├── mcp.json             # Playwright MCP server
│   └── lsp.json             # clangd / pyright LSP config
├── agents/                  # 4 agent definitions
│   ├── zeptodb.json
│   ├── zepto-developer.json
│   ├── zepto-reviewer.json
│   └── zepto-qa.json
├── context/                 # per-agent context files
│   ├── orchestrator.md
│   ├── developer.md
│   ├── reviewer.md
│   └── qa.md
└── skills/                  # 6 project skills + 1 vendored
    ├── design-doc-index/SKILL.md
    ├── layer-patterns/SKILL.md
    ├── edge-case-catalog/SKILL.md
    ├── doc-update-checklist/SKILL.md
    ├── review-checklist-by-layer/SKILL.md
    ├── cross-arch-verification/SKILL.md
    └── cpp-coding-standards -> ../../.agents/skills/cpp-coding-standards
```

### Why persistent context matters for a project like ZeptoDB

ZeptoDB has 100+ design docs, 5 architectural layers, 830+ tests, and cross-arch (x86_64 + aarch64) requirements. Without persistent context:

- Every new chat would waste its opening tokens re-learning "where is the ring buffer code".
- Agents would drift — one session might put new logging under `ZEPTO_INFO`, the next under `util::Logger`.
- Doc-code drift would accumulate because "update COMPLETED.md and devlog" is the kind of thing humans forget but a rulebook doesn't.

Persistent `.kiro/` turns the repo itself into the prompt.

---

## What I Built (Scope Summary)

Features I added to ZeptoDB while using this Kiro setup — each has a corresponding devlog in `docs/devlog/` (browsable on [zeptodb.com](https://zeptodb.com)):

- Multi-node distributed cluster (consistent hashing, RF=2, auto-failover) — `phase_c_distributed.md`
- SQL DML (INSERT / UPDATE / DELETE) — `sql_dml.md`
- Feed handlers: FIX, ITCH, Binance, Kafka consumer — `kafka_consumer_design.md`
- License system (JWT claims, feature gating) — `license_system.md`
- Cross-arch SIMD (Highway) verified bit-exact on x86_64 AVX2 + aarch64 NEON
- Table-scoped partitioning (devlog 082)
- Web UI (Next.js admin console + Playwright MCP smoke tests)

Everything shipped went through the orchestrator → developer → (reviewer ‖ qa) pipeline.

---

## Learnings

**What worked well**

- **Tool allowlists are underrated.** Making the reviewer read-only removed an entire class of accidents.
- **Keyword-activated skills scale better than mega-prompts.** I grew from 1 to 6 skills without feeling the weight.
- **Checking `.kiro/` into git** means onboarding a new engineer is `git clone` + open Kiro — they immediately get the same rules, the same agents, and the same doc-update checklist.
- **Devlog numbering in context** (`next devlog: 083_*.md`) eliminates the "what number do I use?" micro-decision every time.

**What to watch out for**

- **Context files are a form of code.** When I moved `ZEPTO_INFO` vs `util::Logger` rules from `KIRO.md` to `developer.md`, one agent briefly lost sight of them. Rule of thumb: anything **every** role must know goes in `KIRO.md`; role-specific goes in `.kiro/context/<role>.md`.
- **Skills must have strong `description` frontmatter.** A vague description means Kiro won't auto-activate the skill when needed. I rewrote three descriptions to start with "Load when…".
- **Don't inline the design docs into skills.** Let skills **point** at `docs/design/*.md` (the `design-doc-index` skill does exactly this). Otherwise the two copies drift.
- **MCP servers are stateful.** The Playwright MCP needs an explicit close — I wrap runs in CI with a teardown step.
- **Parallel subagents can race on the same file.** I avoid this by giving the reviewer read-only tools and restricting QA to `tests/` paths.

**One-liner takeaway**

> Treat the agent setup like production code — version it, lint it, keep it minimal, and let it load itself. Then the only thing left to do is describe the feature.

---

## Learn more about ZeptoDB

This document focuses on the **Kiro usage** side. For ZeptoDB itself — architecture, SQL reference, benchmarks, deployment guides, full devlog history — see the project site:

👉 **[https://zeptodb.com](https://zeptodb.com)**
