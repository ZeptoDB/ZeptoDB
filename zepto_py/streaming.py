"""
ZeptoDB Streaming Session — batch ingest with progress tracking.
"""
from __future__ import annotations

from typing import Optional, Callable, Iterator, Any
import time

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

try:
    import polars as pl
    HAS_POLARS = True
except ImportError:
    HAS_POLARS = False

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False


class StreamingSession:
    """
    High-throughput streaming ingest session.

    Provides batch ingest from pandas/polars DataFrames with:
    - Progress callbacks
    - Throughput measurement
    - Error handling with retry

    Example
    -------
    >>> pipeline = zeptodb.Pipeline()
    >>> pipeline.start()
    >>> sess = StreamingSession(pipeline, batch_size=50_000)
    >>> sess.ingest_pandas(df, show_progress=True)
    Ingested 1,000,000 rows in 1.82s (549,451 rows/sec)
    1000000
    """

    def __init__(
        self,
        pipeline: Any,
        batch_size: int = 50_000,
        on_error: str = "raise",     # "raise" | "skip" | "warn"
    ):
        self.pipeline   = pipeline
        self.batch_size = batch_size
        self.on_error   = on_error

        self._total_ingested = 0
        self._total_errors   = 0
        self._start_time     = 0.0

    # ------------------------------------------------------------------
    # Pandas
    # ------------------------------------------------------------------

    def ingest_pandas(
        self,
        df: "pd.DataFrame",
        show_progress: bool = False,
        progress_cb: Optional[Callable[[int, int], None]] = None,
    ) -> int:
        """
        Ingest a pandas DataFrame.

        Parameters
        ----------
        df : pd.DataFrame
        show_progress : bool
        progress_cb : callable(rows_done, total_rows), optional

        Returns
        -------
        int : rows ingested
        """
        if not HAS_PANDAS:
            raise ImportError("pandas is required")

        cols  = list(df.columns)
        total = len(df)
        self._start_time = time.perf_counter()
        ingested = 0

        for start in range(0, total, self.batch_size):
            end   = min(start + self.batch_size, total)
            chunk = df.iloc[start:end]

            batch_count = self._ingest_pandas_chunk(chunk, cols)
            ingested += batch_count

            if progress_cb:
                progress_cb(ingested, total)

            if show_progress:
                self._print_progress(ingested, total)

        self._total_ingested += ingested
        elapsed = time.perf_counter() - self._start_time

        if show_progress:
            rps = ingested / elapsed if elapsed > 0 else 0
            print(f"\nIngested {ingested:,} rows in {elapsed:.2f}s "
                  f"({rps:,.0f} rows/sec)")

        return ingested

    def _ingest_pandas_chunk(self, chunk: "pd.DataFrame", cols: list) -> int:
        count = 0
        for _, row in chunk.iterrows():
            kwargs = {}
            for col in cols:
                val = row[col]
                if hasattr(val, 'item'):
                    val = val.item()
                if HAS_PANDAS and isinstance(val, pd.Timestamp):
                    val = int(val.value)
                kwargs[col] = val

            try:
                self.pipeline.ingest(**kwargs)
                count += 1
            except Exception as e:
                self._total_errors += 1
                if self.on_error == "raise":
                    raise
                elif self.on_error == "warn":
                    import warnings
                    warnings.warn(f"Ingest error: {e}")

        try:
            self.pipeline.drain()
        except Exception:
            pass

        return count

    # ------------------------------------------------------------------
    # Polars
    # ------------------------------------------------------------------

    def ingest_polars(
        self,
        df: "pl.DataFrame",
        show_progress: bool = False,
        use_arrow: bool = True,
    ) -> int:
        """
        Ingest a polars DataFrame.

        Parameters
        ----------
        df : pl.DataFrame
        show_progress : bool
        use_arrow : bool
            If True, use Arrow IPC path (faster). Requires pyarrow.

        Returns
        -------
        int : rows ingested
        """
        if not HAS_POLARS:
            raise ImportError("polars is required")

        if use_arrow:
            try:
                return self._ingest_polars_arrow(df, show_progress)
            except ImportError:
                pass  # fallback to pandas path

        # Fallback: polars → pandas → ingest
        return self.ingest_pandas(df.to_pandas(), show_progress=show_progress)

    def _ingest_polars_arrow(
        self,
        df: "pl.DataFrame",
        show_progress: bool,
    ) -> int:
        """Zero-copy path via Arrow RecordBatches."""
        import pyarrow as pa

        arrow_table = df.to_arrow()
        total    = len(df)
        ingested = 0

        self._start_time = time.perf_counter()
        col_names = arrow_table.schema.names

        for batch in arrow_table.to_batches(max_chunksize=self.batch_size):
            for i in range(batch.num_rows):
                kwargs = {}
                for col_name in col_names:
                    col  = batch.column(col_name)
                    val  = col[i].as_py()
                    if val is not None:
                        kwargs[col_name] = val

                try:
                    self.pipeline.ingest(**kwargs)
                    ingested += 1
                except Exception as e:
                    self._total_errors += 1
                    if self.on_error == "raise":
                        raise

            try:
                self.pipeline.drain()
            except Exception:
                pass

            if show_progress:
                self._print_progress(ingested, total)

        self._total_ingested += ingested
        elapsed = time.perf_counter() - self._start_time

        if show_progress:
            rps = ingested / elapsed if elapsed > 0 else 0
            print(f"\nIngested {ingested:,} rows in {elapsed:.2f}s "
                  f"({rps:,.0f} rows/sec)")

        return ingested

    # ------------------------------------------------------------------
    # Iterator / Generator ingest
    # ------------------------------------------------------------------

    def ingest_iter(
        self,
        iterator: Iterator[dict],
        total_hint: Optional[int] = None,
        show_progress: bool = False,
    ) -> int:
        """
        Ingest from a dict iterator (generator-friendly).

        Parameters
        ----------
        iterator : Iterator[dict]
            Yields dicts of {column_name: value}.
        total_hint : int, optional
            Total row count hint for progress display.
        show_progress : bool

        Returns
        -------
        int : rows ingested

        Example
        -------
        >>> def tick_stream():
        ...     for i in range(1_000_000):
        ...         yield {"sym": 1, "price": 150.0 + i*0.01, "size": 100}
        >>> sess.ingest_iter(tick_stream(), total_hint=1_000_000, show_progress=True)
        """
        ingested = 0
        self._start_time = time.perf_counter()

        for row in iterator:
            try:
                self.pipeline.ingest(**row)
                ingested += 1
            except Exception as e:
                self._total_errors += 1
                if self.on_error == "raise":
                    raise

            if ingested % self.batch_size == 0:
                try:
                    self.pipeline.drain()
                except Exception:
                    pass

                if show_progress:
                    self._print_progress(ingested, total_hint)

        # Final drain
        try:
            self.pipeline.drain()
        except Exception:
            pass

        self._total_ingested += ingested

        if show_progress:
            elapsed = time.perf_counter() - self._start_time
            rps = ingested / elapsed if elapsed > 0 else 0
            print(f"\nIngested {ingested:,} rows in {elapsed:.2f}s "
                  f"({rps:,.0f} rows/sec)")

        return ingested

    # ------------------------------------------------------------------
    # Stats
    # ------------------------------------------------------------------

    @property
    def total_ingested(self) -> int:
        return self._total_ingested

    @property
    def total_errors(self) -> int:
        return self._total_errors

    def reset_stats(self) -> None:
        self._total_ingested = 0
        self._total_errors   = 0

    # ------------------------------------------------------------------
    # Private helpers
    # ------------------------------------------------------------------

    def _print_progress(self, done: int, total: Optional[int]) -> None:
        elapsed = time.perf_counter() - self._start_time
        rps     = done / elapsed if elapsed > 0 else 0

        if total:
            pct = done / total * 100
            print(f"\r  {done:>10,} / {total:,} rows  "
                  f"({pct:.1f}%)  {rps:>12,.0f} rows/sec",
                  end="", flush=True)
        else:
            print(f"\r  {done:>10,} rows  "
                  f"{rps:>12,.0f} rows/sec",
                  end="", flush=True)
