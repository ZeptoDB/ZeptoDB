"""
ZeptoDB Lazy DSL
================
Polars의 .lazy() → .collect() 패러다임을 참고한 지연 평가 API.

사용 예:
    from zeptodb.dsl import DataFrame

    db = zeptodb.Pipeline()
    db.start()
    # ... ingest ...
    db.drain()

    df = DataFrame(db, symbol=1)

    # lazy 체인 — collect() 전까지 C++ 호출 없음
    total = df[df['price'] > 15000]['volume'].sum()
    print(total.collect())   # 이 시점에 C++ 실행

    vwap = df.vwap()
    print(vwap.collect())

    count = df.count()
    print(count.collect())

표현식 노드 타입:
    DataFrameNode   — 루트 (symbol, pipeline 참조)
    FilterNode      — column > threshold / column < threshold
    ColumnNode      — 컬럼 선택
    AggNode         — sum / count / vwap
"""

from __future__ import annotations
from typing import Optional, Union


# ============================================================================
# Expression 노드 계층
# ============================================================================

class Expr:
    """모든 lazy 표현식의 기반 클래스."""

    # 비교 연산자 → FilterNode 생성
    def __gt__(self, other) -> "CompareExpr":
        return CompareExpr(self, ">", other)

    def __lt__(self, other) -> "CompareExpr":
        return CompareExpr(self, "<", other)

    def __ge__(self, other) -> "CompareExpr":
        return CompareExpr(self, ">=", other)

    def __le__(self, other) -> "CompareExpr":
        return CompareExpr(self, "<=", other)

    def __eq__(self, other) -> "CompareExpr":
        return CompareExpr(self, "==", other)

    def sum(self) -> "LazyResult":
        """컬럼 합계 (filter 이후에 체인 가능)."""
        return LazyResult(AggNode("sum", self))

    def count(self) -> "LazyResult":
        """행 수 카운트."""
        return LazyResult(AggNode("count", self))

    def collect(self):
        """표현식 평가 실행. 서브클래스에서 구현."""
        raise NotImplementedError("collect() must be implemented by subclass")


class ColumnExpr(Expr):
    """컬럼 참조 노드. df['price'] → ColumnExpr('price', frame_ref)"""

    def __init__(self, name: str, frame: "DataFrame"):
        self._name = name
        self._frame = frame

    @property
    def name(self) -> str:
        return self._name

    @property
    def frame(self) -> "DataFrame":
        return self._frame

    def collect(self):
        """컬럼 raw 데이터 반환 (numpy zero-copy)."""
        return self._frame._db.get_column(self._frame._symbol, self._name)

    def __repr__(self) -> str:
        return f"ColumnExpr('{self._name}')"


class CompareExpr(Expr):
    """비교 노드. ColumnExpr > scalar"""

    def __init__(self, left: Expr, op: str, right):
        self._left = left
        self._op = op
        self._right = right

    @property
    def left(self) -> Expr:
        return self._left

    @property
    def op(self) -> str:
        return self._op

    @property
    def right(self):
        return self._right

    def __repr__(self) -> str:
        return f"CompareExpr({self._left} {self._op} {self._right})"


class AggNode(Expr):
    """집계 노드. sum / count / vwap"""

    def __init__(self, agg_type: str, source: Expr):
        self._agg_type = agg_type  # "sum", "count", "vwap"
        self._source = source

    @property
    def agg_type(self) -> str:
        return self._agg_type

    @property
    def source(self) -> Expr:
        return self._source

    def __repr__(self) -> str:
        return f"AggNode({self._agg_type}, {self._source})"


# ============================================================================
# FilteredFrame: df[condition] 결과
# ============================================================================

class FilteredFrame:
    """
    df[condition] 으로 생성된 lazy 필터 뷰.
    __getitem__으로 컬럼 선택 → FilteredColumn 반환.
    """

    def __init__(self, frame: "DataFrame", condition: CompareExpr):
        self._frame = frame
        self._condition = condition

    def __getitem__(self, col_name: str) -> "FilteredColumn":
        return FilteredColumn(self._frame, self._condition, col_name)

    def count(self) -> "LazyResult":
        """필터된 행 수."""
        return LazyResult(_FilteredCountNode(self._frame, self._condition))

    def __repr__(self) -> str:
        return f"FilteredFrame(symbol={self._frame._symbol}, cond={self._condition})"


class FilteredColumn:
    """
    FilteredFrame['column'] — 필터 조건이 붙은 컬럼.
    .sum() → LazyResult
    """

    def __init__(self, frame: "DataFrame", condition: CompareExpr, col_name: str):
        self._frame = frame
        self._condition = condition
        self._col_name = col_name

    def sum(self) -> "LazyResult":
        return LazyResult(_FilterSumNode(self._frame, self._condition, self._col_name))

    def __repr__(self) -> str:
        return (f"FilteredColumn(symbol={self._frame._symbol}, "
                f"col='{self._col_name}', cond={self._condition})")


# ============================================================================
# 내부 실행 노드 (collect 로직 포함)
# ============================================================================

class _FilterSumNode:
    """filter_sum C++ 쿼리 실행 노드."""

    def __init__(self, frame: "DataFrame", condition: CompareExpr, col_name: str):
        self._frame = frame
        self._condition = condition
        self._col_name = col_name

    def execute(self):
        # condition: ColumnExpr op scalar
        cond = self._condition
        if not isinstance(cond.left, ColumnExpr):
            raise TypeError("Filter LHS must be a column reference")

        filter_col = cond.left.name
        threshold = int(cond.right)

        # 현재 지원: column > threshold → filter_sum
        # 다른 연산자는 numpy fallback
        if cond.op == ">":
            result = self._frame._db.filter_sum(
                self._frame._symbol, filter_col, threshold
            )
            return result.ivalue
        else:
            # numpy fallback for !=, >=, < etc.
            import numpy as np
            col_data = self._frame._db.get_column(
                self._frame._symbol, filter_col
            )
            target_data = self._frame._db.get_column(
                self._frame._symbol, self._col_name
            )
            mask = _apply_op(col_data, cond.op, threshold)
            return int(np.sum(target_data[mask]))


class _FilteredCountNode:
    """필터된 행 수 카운트 노드."""

    def __init__(self, frame: "DataFrame", condition: CompareExpr):
        self._frame = frame
        self._condition = condition

    def execute(self):
        import numpy as np
        cond = self._condition
        if not isinstance(cond.left, ColumnExpr):
            raise TypeError("Filter LHS must be a column reference")

        col_data = self._frame._db.get_column(
            self._frame._symbol, cond.left.name
        )
        mask = _apply_op(col_data, cond.op, int(cond.right))
        return int(np.sum(mask))


def _apply_op(arr, op: str, threshold):
    """numpy 연산자 적용 헬퍼."""
    if op == ">":
        return arr > threshold
    elif op == "<":
        return arr < threshold
    elif op == ">=":
        return arr >= threshold
    elif op == "<=":
        return arr <= threshold
    elif op == "==":
        return arr == threshold
    else:
        raise ValueError(f"Unsupported operator: {op}")


# ============================================================================
# LazyResult: collect() 게이트
# ============================================================================

class LazyResult:
    """
    지연 평가 결과 컨테이너.
    .collect() 호출 시 실제 C++ 쿼리 실행.
    .value 속성으로도 접근 가능 (자동 collect).
    """

    def __init__(self, node):
        self._node = node
        self._cached = None
        self._evaluated = False

    def collect(self):
        """C++ 엔진 실행 후 결과 반환. 결과는 캐싱됨."""
        if not self._evaluated:
            if hasattr(self._node, "execute"):
                self._cached = self._node.execute()
            else:
                self._cached = self._node.collect()
            self._evaluated = True
        return self._cached

    @property
    def value(self):
        """collect() 자동 호출 후 값 반환."""
        return self.collect()

    def __repr__(self) -> str:
        status = f"={self._cached}" if self._evaluated else "(unevaluated)"
        return f"LazyResult{status}"


# ============================================================================
# DataFrame: Polars 스타일 lazy API 진입점
# ============================================================================

class DataFrame:
    """
    ZeptoDB Lazy DataFrame.

    Polars의 .lazy() → .collect() 패러다임을 참고:
      df = DataFrame(db, symbol=1)           # lazy 뷰 생성
      result = df[df['price'] > 15000]['volume'].sum()
      print(result.collect())                # 이 시점에 C++ 실행
    """

    def __init__(self, db, symbol: int):
        """
        Args:
            db: zeptodb.Pipeline 인스턴스
            symbol: 조회할 symbol ID
        """
        self._db = db
        self._symbol = symbol

    def __getitem__(self, key) -> Union[ColumnExpr, FilteredFrame]:
        """
        df['price']          → ColumnExpr (컬럼 참조)
        df[df['price'] > 10] → FilteredFrame (필터 뷰)
        """
        if isinstance(key, str):
            return ColumnExpr(key, self)
        elif isinstance(key, CompareExpr):
            return FilteredFrame(self, key)
        else:
            raise TypeError(
                f"DataFrame key must be str or CompareExpr, got {type(key).__name__}"
            )

    def vwap(self) -> "LazyResult":
        """VWAP 지연 쿼리. .collect() 시 C++ query_vwap() 실행."""
        return LazyResult(_VWAPNode(self))

    def count(self) -> "LazyResult":
        """COUNT 지연 쿼리. .collect() 시 C++ query_count() 실행."""
        return LazyResult(_CountNode(self))

    def filter(self, condition: CompareExpr) -> FilteredFrame:
        """명시적 필터. df.filter(df['price'] > 100) 형태."""
        return FilteredFrame(self, condition)

    def collect_column(self, name: str):
        """컬럼 데이터를 즉시 numpy array로 반환 (zero-copy)."""
        return self._db.get_column(self._symbol, name)

    @property
    def symbol(self) -> int:
        return self._symbol

    def __repr__(self) -> str:
        return f"DataFrame(symbol={self._symbol})"


# ============================================================================
# VWAP / COUNT 노드
# ============================================================================

class _VWAPNode:
    def __init__(self, frame: DataFrame):
        self._frame = frame

    def execute(self):
        result = self._frame._db.vwap(self._frame._symbol)
        if not result.ok():
            raise RuntimeError(f"VWAP query failed: {result}")
        return result.value


class _CountNode:
    def __init__(self, frame: DataFrame):
        self._frame = frame

    def execute(self):
        result = self._frame._db.count(self._frame._symbol)
        if not result.ok():
            raise RuntimeError(f"COUNT query failed: {result}")
        return result.ivalue
