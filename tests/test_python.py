"""
tests/test_python.py — Phase D Python 바인딩 테스트

실행:
    cd ~/zeptodb
    python3 -m pytest tests/test_python.py -v
"""

import sys
import os
import time

import pytest
import numpy as np

# build/ 디렉토리에서 apex.so, zepto_py/ 를 찾는다
BUILD_DIR = os.path.join(os.path.dirname(__file__), "..", "build")
sys.path.insert(0, os.path.abspath(BUILD_DIR))

import zeptodb  # apex.so (pybind11 모듈)


# ============================================================================
# 헬퍼
# ============================================================================

def make_pipeline():
    db = zeptodb.Pipeline()
    db.start()
    return db


def ingest_n(db, symbol: int, n: int, base_price: int = 10000, base_vol: int = 10):
    for i in range(n):
        db.ingest(symbol=symbol, price=base_price + i, volume=base_vol + i)
    db.drain()


# ============================================================================
# 기본 파이프라인 테스트
# ============================================================================

class TestBasicPipeline:

    def test_start_stop(self):
        db = zeptodb.Pipeline()
        db.start()
        db.stop()

    def test_single_ingest_count(self):
        db = make_pipeline()
        db.ingest(symbol=1, price=15000, volume=100)
        db.drain()
        r = db.count(symbol=1)
        assert r.ok()
        assert r.ivalue == 1
        db.stop()

    def test_multiple_ingest_count(self):
        db = make_pipeline()
        N = 50
        ingest_n(db, symbol=2, n=N)
        r = db.count(symbol=2)
        assert r.ok()
        assert r.ivalue == N
        db.stop()

    def test_count_has_latency(self):
        db = make_pipeline()
        ingest_n(db, symbol=3, n=10)
        r = db.count(symbol=3)
        assert r.latency_ns >= 0
        db.stop()

    def test_vwap_basic(self):
        """VWAP = sum(price*vol) / sum(vol)"""
        db = make_pipeline()
        # price=10000, vol=100 → VWAP = 10000
        db.ingest(symbol=10, price=10000, volume=100)
        db.ingest(symbol=10, price=20000, volume=100)
        db.drain()
        r = db.vwap(symbol=10)
        assert r.ok()
        # VWAP = (10000*100 + 20000*100) / 200 = 15000
        assert abs(r.value - 15000.0) < 1.0
        db.stop()

    def test_vwap_weighted(self):
        """비균등 volume: price=1000 vol=1, price=2000 vol=3 → VWAP=1750"""
        db = make_pipeline()
        db.ingest(symbol=11, price=1000, volume=1)
        db.ingest(symbol=11, price=2000, volume=3)
        db.drain()
        r = db.vwap(symbol=11)
        assert r.ok()
        assert abs(r.value - 1750.0) < 1.0
        db.stop()

    def test_filter_sum(self):
        """price > 10005 인 price의 합계"""
        db = make_pipeline()
        N = 10
        ingest_n(db, symbol=20, n=N, base_price=10000)
        # prices: 10000..10009. > 10005 → 10006,10007,10008,10009
        r = db.filter_sum(symbol=20, column="price", threshold=10005)
        assert r.ok()
        expected = sum(p for p in range(10000, 10010) if p > 10005)
        assert r.ivalue == expected
        db.stop()

    def test_query_result_fields(self):
        db = make_pipeline()
        db.ingest(symbol=30, price=5000, volume=50)
        db.drain()
        r = db.count(symbol=30)
        assert hasattr(r, "value")
        assert hasattr(r, "ivalue")
        assert hasattr(r, "rows_scanned")
        assert hasattr(r, "latency_ns")
        assert r.ok()
        db.stop()


# ============================================================================
# 배치 인제스트 테스트
# ============================================================================

class TestBatchIngest:

    def test_batch_list(self):
        db = make_pipeline()
        db.ingest_batch(
            symbols=[1, 1, 1],
            prices=[15000, 15050, 15100],
            volumes=[100, 200, 300]
        )
        db.drain()
        r = db.count(symbol=1)
        assert r.ivalue == 3
        db.stop()

    def test_batch_numpy(self):
        db = make_pipeline()
        n = 100
        syms   = np.full(n, 5, dtype=np.int64)
        prices = np.arange(10000, 10000 + n, dtype=np.int64)
        vols   = np.arange(1, n + 1, dtype=np.int64)
        db.ingest_batch(symbols=syms, prices=prices, volumes=vols)
        db.drain()
        r = db.count(symbol=5)
        assert r.ivalue == n
        db.stop()

    def test_batch_mismatch_raises(self):
        db = make_pipeline()
        with pytest.raises(Exception):
            db.ingest_batch(
                symbols=[1, 2],
                prices=[100, 200, 300],
                volumes=[10, 20]
            )
        db.stop()

    def test_batch_vwap(self):
        db = make_pipeline()
        # 균일 price=10000, vol=1 → VWAP=10000
        n = 50
        syms   = np.full(n, 7, dtype=np.int64)
        prices = np.full(n, 10000, dtype=np.int64)
        vols   = np.ones(n, dtype=np.int64)
        db.ingest_batch(symbols=syms, prices=prices, volumes=vols)
        db.drain()
        r = db.vwap(symbol=7)
        assert r.ok()
        assert abs(r.value - 10000.0) < 1.0
        db.stop()


# ============================================================================
# Zero-copy 컬럼 접근 테스트
# ============================================================================

class TestZeroCopy:

    def test_get_column_returns_ndarray(self):
        db = make_pipeline()
        ingest_n(db, symbol=100, n=10)
        prices = db.get_column(symbol=100, name="price")
        assert isinstance(prices, np.ndarray)
        db.stop()

    def test_get_column_length(self):
        db = make_pipeline()
        N = 100
        ingest_n(db, symbol=101, n=N)
        prices = db.get_column(symbol=101, name="price")
        assert len(prices) == N
        db.stop()

    def test_get_column_values(self):
        db = make_pipeline()
        N = 10
        ingest_n(db, symbol=102, n=N, base_price=10000)
        prices = db.get_column(symbol=102, name="price")
        for i in range(N):
            assert prices[i] == 10000 + i
        db.stop()

    def test_get_column_volume(self):
        db = make_pipeline()
        N = 5
        ingest_n(db, symbol=103, n=N, base_vol=100)
        vols = db.get_column(symbol=103, name="volume")
        assert len(vols) == N
        assert vols[0] == 100
        db.stop()

    def test_zero_copy_no_data_raises(self):
        """데이터 없는 symbol에서 get_column 호출 시 예외"""
        db = make_pipeline()
        with pytest.raises(Exception):
            db.get_column(symbol=9999, name="price")
        db.stop()

    def test_is_view_not_copy(self):
        """writeable=False → numpy가 복사 없이 외부 버퍼 참조임을 확인"""
        db = make_pipeline()
        ingest_n(db, symbol=104, n=20)
        prices = db.get_column(symbol=104, name="price")
        # zero-copy 뷰는 기본적으로 writeable=False
        assert prices.flags["OWNDATA"] is False or prices.base is not None
        db.stop()


# ============================================================================
# 통계 테스트
# ============================================================================

class TestStats:

    def test_stats_returns_dict(self):
        db = make_pipeline()
        s = db.stats()
        assert isinstance(s, dict)
        assert "ticks_ingested" in s
        assert "ticks_stored" in s
        db.stop()

    def test_stats_ticks_ingested(self):
        db = make_pipeline()
        N = 15
        ingest_n(db, symbol=200, n=N)
        s = db.stats()
        assert s["ticks_ingested"] >= N
        db.stop()

    def test_stats_ticks_stored(self):
        db = make_pipeline()
        N = 10
        ingest_n(db, symbol=201, n=N)
        s = db.stats()
        assert s["ticks_stored"] >= N
        db.stop()


# ============================================================================
# DSL 테스트
# ============================================================================

class TestLazyDSL:

    def _setup_db(self, symbol: int, n: int = 20, base_price: int = 10000):
        db = make_pipeline()
        ingest_n(db, symbol=symbol, n=n, base_price=base_price)
        return db

    def test_dsl_import(self):
        # zepto_py/dsl.py 가 빌드 경로에 있어야 함
        dsl_path = os.path.join(BUILD_DIR, "zepto_py")
        sys.path.insert(0, dsl_path)
        # zepto_py 폴더 직접 import
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        assert DataFrame is not None

    def test_dsl_dataframe_creation(self):
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        db = self._setup_db(symbol=300)
        df = DataFrame(db, symbol=300)
        assert df.symbol == 300
        db.stop()

    def test_dsl_vwap_lazy(self):
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        db = self._setup_db(symbol=301, n=10)
        df = DataFrame(db, symbol=301)
        lazy_vwap = df.vwap()
        # collect 전까지 평가 없음
        assert lazy_vwap._evaluated is False
        val = lazy_vwap.collect()
        assert isinstance(val, float)
        assert val > 0
        db.stop()

    def test_dsl_count_lazy(self):
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        N = 25
        db = self._setup_db(symbol=302, n=N)
        df = DataFrame(db, symbol=302)
        lazy_count = df.count()
        assert lazy_count._evaluated is False
        val = lazy_count.collect()
        assert val == N
        db.stop()

    def test_dsl_filter_sum(self):
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        N = 10
        db = self._setup_db(symbol=303, n=N, base_price=10000)
        df = DataFrame(db, symbol=303)
        # prices: 10000..10009. > 10005 → 10006+10007+10008+10009
        result = df[df['price'] > 10005]['price'].sum()
        val = result.collect()
        expected = sum(p for p in range(10000, 10010) if p > 10005)
        assert val == expected
        db.stop()

    def test_dsl_column_access(self):
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        N = 5
        db = self._setup_db(symbol=304, n=N)
        df = DataFrame(db, symbol=304)
        col = df['price']
        data = col.collect()
        assert len(data) == N
        db.stop()

    def test_dsl_lazy_value_property(self):
        """LazyResult.value 는 collect() 자동 호출"""
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        db = self._setup_db(symbol=305, n=5)
        df = DataFrame(db, symbol=305)
        lazy = df.count()
        val = lazy.value  # collect() 자동
        assert val == 5
        db.stop()

    def test_dsl_cache(self):
        """collect() 두 번 호출해도 같은 결과 (캐싱)"""
        sys.path.insert(0, BUILD_DIR)
        from zepto_py.dsl import DataFrame
        db = self._setup_db(symbol=306, n=8)
        df = DataFrame(db, symbol=306)
        lazy = df.count()
        v1 = lazy.collect()
        v2 = lazy.collect()
        assert v1 == v2
        db.stop()


# ============================================================================
# 멀티 심볼 격리 테스트
# ============================================================================

class TestMultiSymbol:

    def test_symbols_isolated(self):
        db = make_pipeline()
        ingest_n(db, symbol=1, n=10)
        ingest_n(db, symbol=2, n=20)
        r1 = db.count(symbol=1)
        r2 = db.count(symbol=2)
        assert r1.ivalue == 10
        assert r2.ivalue == 20
        db.stop()

    def test_vwap_per_symbol(self):
        db = make_pipeline()
        db.ingest(symbol=50, price=1000, volume=1)
        db.ingest(symbol=51, price=2000, volume=1)
        db.drain()
        r50 = db.vwap(symbol=50)
        r51 = db.vwap(symbol=51)
        assert abs(r50.value - 1000.0) < 1.0
        assert abs(r51.value - 2000.0) < 1.0
        db.stop()
