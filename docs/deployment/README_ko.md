# APEX-DB Deployment Guides

프로덕션 환경에 APEX-DB를 배포하는 방법을 안내합니다.

---

## 📚 문서 목록

### 1. [PRODUCTION_DEPLOYMENT.md](PRODUCTION_DEPLOYMENT.md) ⭐ 필독
**베어메탈 vs 클라우드 선택 가이드**
- 워크로드별 배포 옵션 선택
- 베어메탈 배포 (HFT Edition)
- 클라우드 네이티브 배포 (Analytics Edition)
- 비교표 및 마이그레이션 경로

### 2. [BARE_METAL_TUNING.md](BARE_METAL_TUNING.md)
**베어메탈 최적화 상세 가이드** (TODO)
- CPU Pinning 전략
- NUMA 최적화
- io_uring 설정
- 네트워크 튜닝
- 벤치마킹 방법

### 3. [KUBERNETES_OPS.md](KUBERNETES_OPS.md)
**Kubernetes 운영 가이드** (TODO)
- Helm Chart 사용법
- 롤링 업데이트
- 모니터링 설정
- 트러블슈팅

---

## 🚀 빠른 시작

### 베어메탈 (HFT/실시간)

```bash
# 1. 튜닝 스크립트 실행
cd apex-db
sudo ./scripts/tune_bare_metal.sh

# 2. 빌드
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPEX_BARE_METAL=ON
ninja -j$(nproc)

# 3. 실행 (NUMA 인식)
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 \
    ./apex_server --port 8123 --hugepages
```

### 클라우드 (퀀트/분석)

```bash
# 1. Docker 이미지 빌드
docker build -t apex-db:latest .

# 2. Kubernetes 배포
kubectl apply -f k8s/deployment.yaml

# 3. 확인
kubectl get pods -n apex-db
kubectl get svc -n apex-db
```

---

## 🎯 배포 옵션 선택

| 요구사항 | 배포 방식 |
|---------|---------|
| 레이턴시 < 100μs | 🔴 **베어메탈** |
| 레이턴시 < 1ms | 🟡 베어메탈 권장 |
| 레이턴시 > 1ms | 🟢 클라우드 OK |
| 자동 스케일링 필요 | 🟢 클라우드 |
| 고정 워크로드 | 🟡 베어메탈 |
| 비용 최적화 중요 | 🟢 클라우드 (spot) |

---

## 📞 지원

- **Enterprise 지원**: enterprise@apex-db.io
- **커뮤니티**: https://discord.gg/apex-db
- **문서**: https://docs.apex-db.io
