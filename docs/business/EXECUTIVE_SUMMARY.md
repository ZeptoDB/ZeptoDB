# ZeptoDB Executive Summary
**One-Page Overview**

---

## What is ZeptoDB?

An open-source time-series database that replaces kdb+, targeting the HFT (High-Frequency Trading) market.

**Core value:**
- **Performance:** kdb+ equivalent (5.52M ticks/sec, <1us latency)
- **Price:** 90% TCO reduction (open-source)
- **Productivity:** SQL (vs 6-month q language learning curve)
- **Python:** zero-copy integration (522ns)

---

## Market Opportunity

| Market | Projected Revenue (Year 1) | Priority |
|--------|---------------------------|----------|
| HFT/Prop Trading | $1.25M - $5M | P0 |
| Crypto Exchanges | $500K - $1M | P0 |
| Ad Tech | $1M - $3M | P1 |
| Hedge Funds | $500K - $1M | P1 |

**Total Year 1:** $3.6M ARR (43 customers)

---

## Competitive Advantage

### vs kdb+ (largest market)
- **Price:** $0 vs $100K-500K/year
- **Productivity:** SQL (1 week) vs q (6 months)
- **Python:** zero-copy vs IPC serialization
- **Performance:** 95% equivalent

### vs ClickHouse
- **Financial functions:** xbar, EMA, wj native
- **Real-time:** 55x faster (5.52M vs 100K ticks/sec)
- **Python DSL:** 4-37x faster than Polars

---

## Go-to-Market Strategy

### Phase 1 (Month 1-3): Quick Win
**ClickHouse migration + DuckDB integration**
- Development: 4 weeks + 2 weeks
- First revenue: $150K (3 months)
- Reference customer acquisition

### Phase 2 (Month 4-6): Big Bet
**kdb+ migration**
- Development: 7 weeks
- HFT market entry
- Revenue: $250K-500K per deal

### Phase 3 (Month 7-12): Scale
**Snowflake Hybrid + TimescaleDB**
- Complementary strategy
- IoT/DevOps expansion

---

## Financial Projection

### Year 1 (Month 12)
- **Revenue:** $3.6M ARR
- **Customers:** 43
- **Break-even:** Month 9-12
- **Team:** 9 (6 eng, 2 sales, 1 CS)

### Year 3
- **Revenue:** $24.3M ARR
- **Customers:** 215
- **Team:** 25

---

## Key Milestones

| Timeline | Milestone |
|----------|-----------|
| **Complete** | Feed Handler (FIX, ITCH, UDP) |
| **Complete** | kdb+ feature parity 95% |
| **Complete** | Production operations (monitoring, backup) |
| **Week 1-4** | ClickHouse migration |
| **Week 5-11** | kdb+ migration |
| **Month 3** | First PoC $150K |
| **Month 6** | First HFT customer $250K |
| **Month 12** | $3.6M ARR |

---

## What We Need

### Immediate (this week)
1. Start ClickHouse migration (2 engineers)
2. DuckDB integration in parallel (1 engineer)

### Month 3-6
3. kdb+ migration (+2 engineers)
4. Enterprise Sales (+1 sales)

### Investment Ask
- **Seed Round:** $1M-2M
- **Use of funds:** Team expansion (9 people), marketing, operating costs
- **Burn Rate:** $150K/month (Year 1)
- **Runway:** 12-18 months

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| kdb+ compatibility | 95% achieved, core features first |
| HFT market entry failure | Fast validation via ClickHouse (3 months) |
| Long sales cycle | Short-term (ClickHouse) + long-term (kdb+) in parallel |

---

## Why Now?

1. **Technology ready:** Feed Handler + kdb+ 95%
2. **Market timing:** Rising kdb+ cost burden
3. **Python trend:** Growing quant use of Jupyter
4. **Cloud migration:** HFT beginning to consider cloud

---

## The Ask

**Funding:** $1M-2M Seed
**Timeline:** 12 months
**Goal:** $3.6M ARR, break-even

**Contact:**
- Email: founders@zeptodb.io
- Deck: https://zeptodb.io/deck
- Demo: https://demo.zeptodb.io
