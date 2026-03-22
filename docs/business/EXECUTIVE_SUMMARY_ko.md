# APEX-DB Executive Summary
**One-Page Overview**

---

## What is APEX-DB?

오픈소스 시계열 데이터베이스로 kdb+를 대체하며, HFT(High-Frequency Trading) 시장을 타겟으로 합니다.

**핵심 가치:**
- ⚡ **성능:** kdb+ 동등 (5.52M ticks/sec, <1μs 지연)
- 💰 **가격:** TCO 90% 절감 (오픈소스)
- 🚀 **생산성:** SQL (vs q 언어 6개월 학습)
- 🐍 **Python:** zero-copy 통합 (522ns)

---

## Market Opportunity

| 시장 | 예상 매출 (Year 1) | 우선순위 |
|------|-------------------|----------|
| HFT/Prop Trading | $1.25M - $5M | ⭐⭐⭐⭐⭐ |
| 암호화폐 거래소 | $500K - $1M | ⭐⭐⭐⭐ |
| 광고 테크 | $1M - $3M | ⭐⭐⭐⭐ |
| 헤지펀드 | $500K - $1M | ⭐⭐⭐ |

**Total Year 1:** $3.6M ARR (43 고객)

---

## Competitive Advantage

### vs kdb+ (가장 큰 시장)
- ✅ **가격:** $0 vs $100K-500K/년
- ✅ **생산성:** SQL (1주) vs q (6개월)
- ✅ **Python:** zero-copy vs IPC 직렬화
- ≈ **성능:** 95% 동등

### vs ClickHouse
- ✅ **금융 함수:** xbar, EMA, wj native
- ✅ **실시간:** 55x 빠름 (5.52M vs 100K ticks/sec)
- ✅ **Python DSL:** 4-37x faster than Polars

---

## Go-to-Market Strategy

### Phase 1 (Month 1-3): Quick Win
**ClickHouse 마이그레이션 + DuckDB 통합**
- 개발: 4주 + 2주
- 첫 매출: $150K (3개월)
- 레퍼런스 고객 확보

### Phase 2 (Month 4-6): Big Bet
**kdb+ 마이그레이션**
- 개발: 7주
- HFT 시장 진입
- 매출: $250K-500K per deal

### Phase 3 (Month 7-12): Scale
**Snowflake Hybrid + TimescaleDB**
- 보완재 전략
- IoT/DevOps 확장

---

## Financial Projection

### Year 1 (Month 12)
- **매출:** $3.6M ARR
- **고객:** 43개
- **Break-even:** Month 9-12
- **팀:** 9명 (6 eng, 2 sales, 1 CS)

### Year 3
- **매출:** $24.3M ARR
- **고객:** 215개
- **팀:** 25명

---

## Key Milestones

| 시기 | 마일스톤 |
|------|---------|
| **✅ 완료** | Feed Handler (FIX, ITCH, UDP) |
| **✅ 완료** | kdb+ 기능 95% 달성 |
| **✅ 완료** | 프로덕션 운영 (모니터링, 백업) |
| **Week 1-4** | ClickHouse 마이그레이션 |
| **Week 5-11** | kdb+ 마이그레이션 |
| **Month 3** | 첫 PoC $150K |
| **Month 6** | 첫 HFT 고객 $250K |
| **Month 12** | $3.6M ARR |

---

## What We Need

### Immediate (이번 주)
1. ClickHouse 마이그레이션 시작 (엔지니어 2명)
2. DuckDB 통합 병렬 진행 (엔지니어 1명)

### Month 3-6
3. kdb+ 마이그레이션 (엔지니어 +2명)
4. Enterprise Sales (영업 +1명)

### Investment Ask
- **Seed Round:** $1M-2M
- **용도:** 팀 확장 (9명), 마케팅, 운영 비용
- **Burn Rate:** $150K/month (Year 1)
- **Runway:** 12-18개월

---

## Risk Mitigation

| 리스크 | 완화 |
|--------|------|
| kdb+ 호환성 | 95% 달성, 핵심 우선 |
| HFT 진입 실패 | ClickHouse로 빠른 검증 (3개월) |
| 긴 영업 사이클 | 단기(ClickHouse) + 장기(kdb+) 병행 |

---

## Why Now?

1. ✅ **기술 준비 완료:** Feed Handler + kdb+ 95%
2. 📈 **시장 타이밍:** kdb+ 비용 부담 증가
3. 🚀 **Python 트렌드:** 퀀트들의 Jupyter 사용 증가
4. ☁️ **클라우드 전환:** HFT도 클라우드 검토 시작

---

## The Ask

**Funding:** $1M-2M Seed
**Timeline:** 12개월
**Goal:** $3.6M ARR, Break-even

**Contact:**
- Email: founders@apex-db.io
- Deck: https://apex-db.io/deck
- Demo: https://demo.apex-db.io
