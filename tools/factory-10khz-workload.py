#!/usr/bin/env python3
"""Factory 10 KHz workload runner for ZeptoDB, InfluxDB, and TimescaleDB.

The script intentionally uses only the Python standard library plus Docker CLI
for TimescaleDB so it can run on CI/bench hosts without extra client packages.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import io
import json
import math
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Iterable


def http_request(
    url: str,
    *,
    method: str = "GET",
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 30.0,
) -> bytes:
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def post_zeptodb_sql(base_url: str, sql: str) -> bytes:
    return http_request(
        base_url.rstrip("/") + "/",
        method="POST",
        body=sql.encode("utf-8"),
        headers={"Content-Type": "text/plain"},
    )


def parse_zeptodb_count(payload: bytes) -> int:
    decoded = json.loads(payload.decode("utf-8"))
    data = decoded.get("data", [])
    if not data or not data[0]:
        return 0
    return int(data[0][0])


def row_stream(total_rows: int, symbols: int, rate: int) -> Iterable[tuple[int, int, int, int]]:
    start_ns = time.time_ns()
    step_ns = max(1, int(1_000_000_000 / rate))
    for i in range(total_rows):
        symbol = i % symbols
        ts_ns = start_ns + i * step_ns
        price = 10_000 + symbol
        volume = 100 + (i % 17)
        yield symbol, ts_ns, price, volume


def sleep_until(start: float, emitted_rows: int, rate: int) -> None:
    target = start + (emitted_rows / rate)
    delay = target - time.perf_counter()
    if delay > 0:
        time.sleep(delay)


def verify_with_retries(fn, expected: int, retries: int = 12, delay: float = 0.5) -> int:
    observed = 0
    for _ in range(retries):
        observed = int(fn())
        if observed >= expected:
            return observed
        time.sleep(delay)
    return observed


def run_zeptodb(args: argparse.Namespace) -> dict[str, object]:
    base_url = args.url.rstrip("/")
    post_zeptodb_sql(
        base_url,
        "CREATE TABLE IF NOT EXISTS trades "
        "(symbol INT64, timestamp TIMESTAMP_NS, price INT64, volume INT64)",
    )

    total_rows = args.seconds * args.rate
    inserted = 0
    failed = 0
    start = time.perf_counter()
    rows = row_stream(total_rows, args.symbols, args.rate)
    while inserted + failed < total_rows:
        batch = []
        for _ in range(min(args.batch_size, total_rows - inserted - failed)):
            try:
                symbol, ts_ns, price, volume = next(rows)
            except StopIteration:
                break
            batch.append(f"({symbol},{ts_ns},{price},{volume})")
        if not batch:
            break
        sql = "INSERT INTO trades VALUES " + ",".join(batch)
        try:
            post_zeptodb_sql(base_url, sql)
            inserted += len(batch)
        except Exception as exc:  # noqa: BLE001 - command-line diagnostic path
            failed += len(batch)
            print(f"zeptodb insert failed: {exc}", file=sys.stderr)
        sleep_until(start, inserted + failed, args.rate)

    elapsed = time.perf_counter() - start

    def count_rows() -> int:
        payload = post_zeptodb_sql(base_url, "SELECT count(*) FROM trades")
        return parse_zeptodb_count(payload)

    verified = verify_with_retries(count_rows, inserted)
    return result(args, inserted, failed, elapsed, verified)


def run_influxdb(args: argparse.Namespace) -> dict[str, object]:
    total_rows = args.seconds * args.rate
    write_url = (
        args.url.rstrip()
        + "/api/v2/write?"
        + urllib.parse.urlencode(
            {"org": args.org, "bucket": args.bucket, "precision": "ns"}
        )
    )
    headers = {
        "Authorization": f"Token {args.token}",
        "Content-Type": "text/plain; charset=utf-8",
    }
    inserted = 0
    failed = 0
    start = time.perf_counter()
    rows = row_stream(total_rows, args.symbols, args.rate)
    while inserted + failed < total_rows:
        lines: list[str] = []
        for _ in range(min(args.batch_size, total_rows - inserted - failed)):
            try:
                symbol, ts_ns, price, volume = next(rows)
            except StopIteration:
                break
            line = symbol % 5
            station = symbol % 20
            lines.append(
                "factory_ticks"
                f",run_id={args.run_id},symbol=s{symbol},line=L{line},station=S{station} "
                f"price={float(price)},volume={volume}i {ts_ns}"
            )
        if not lines:
            break
        try:
            http_request(
                write_url,
                method="POST",
                body=("\n".join(lines) + "\n").encode("utf-8"),
                headers=headers,
            )
            inserted += len(lines)
        except Exception as exc:  # noqa: BLE001
            failed += len(lines)
            print(f"influxdb write failed: {exc}", file=sys.stderr)
        sleep_until(start, inserted + failed, args.rate)

    elapsed = time.perf_counter() - start

    def count_rows() -> int:
        flux = f'''
from(bucket: "{args.bucket}")
  |> range(start: 1970-01-01T00:00:00Z)
  |> filter(fn: (r) => r._measurement == "factory_ticks")
  |> filter(fn: (r) => r.run_id == "{args.run_id}")
  |> filter(fn: (r) => r._field == "price")
  |> count()
'''
        payload = http_request(
            args.url.rstrip("/") + "/api/v2/query?org=" + urllib.parse.quote(args.org),
            method="POST",
            body=flux.encode("utf-8"),
            headers={
                "Authorization": f"Token {args.token}",
                "Content-Type": "application/vnd.flux",
                "Accept": "application/csv",
            },
        ).decode("utf-8")
        total_count = 0
        for row in csv.DictReader(io.StringIO(payload)):
            value = row.get("_value")
            if value:
                total_count += int(value)
        return total_count

    verified = verify_with_retries(count_rows, inserted)
    return result(args, inserted, failed, elapsed, verified)


def docker_exec(container: str, command: list[str], *, stdin: bytes | None = None) -> bytes:
    proc = subprocess.run(
        ["docker", "exec", "-i", container, *command],
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"docker exec failed ({proc.returncode}): {proc.stderr.decode('utf-8', 'replace')}"
        )
    return proc.stdout


def run_psql(container: str, user: str, db: str, sql: str) -> bytes:
    return docker_exec(
        container,
        ["psql", "-U", user, "-d", db, "-v", "ON_ERROR_STOP=1", "-c", sql],
    )


def setup_timescaledb(args: argparse.Namespace) -> None:
    run_psql(args.container, args.user, args.db, "CREATE EXTENSION IF NOT EXISTS timescaledb;")
    run_psql(
        args.container,
        args.user,
        args.db,
        """
CREATE TABLE IF NOT EXISTS factory_ticks (
  time TIMESTAMPTZ NOT NULL,
  run_id TEXT NOT NULL,
  symbol INTEGER NOT NULL,
  price DOUBLE PRECISION NOT NULL,
  volume BIGINT NOT NULL,
  line INTEGER NOT NULL,
  station INTEGER NOT NULL
);
SELECT create_hypertable('factory_ticks', 'time', if_not_exists => TRUE);
""",
    )


def ns_to_iso(ts_ns: int) -> str:
    seconds, nanos = divmod(ts_ns, 1_000_000_000)
    value = dt.datetime.fromtimestamp(seconds, tz=dt.timezone.utc)
    return value.strftime("%Y-%m-%dT%H:%M:%S") + f".{nanos:09d}+00:00"


def run_timescaledb(args: argparse.Namespace) -> dict[str, object]:
    setup_timescaledb(args)
    total_rows = args.seconds * args.rate
    inserted = 0
    failed = 0
    start = time.perf_counter()
    copy_cmd = [
        "docker",
        "exec",
        "-i",
        args.container,
        "psql",
        "-U",
        args.user,
        "-d",
        args.db,
        "-v",
        "ON_ERROR_STOP=1",
        "-c",
        "COPY factory_ticks(time, run_id, symbol, price, volume, line, station) "
        "FROM STDIN WITH (FORMAT csv)",
    ]
    proc = subprocess.Popen(
        copy_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert proc.stdin is not None
    try:
        rows = row_stream(total_rows, args.symbols, args.rate)
        while inserted + failed < total_rows:
            out = io.StringIO()
            writer = csv.writer(out, lineterminator="\n")
            batch_count = 0
            for _ in range(min(args.batch_size, total_rows - inserted - failed)):
                try:
                    symbol, ts_ns, price, volume = next(rows)
                except StopIteration:
                    break
                writer.writerow(
                    [
                        ns_to_iso(ts_ns),
                        args.run_id,
                        symbol,
                        float(price),
                        volume,
                        symbol % 5,
                        symbol % 20,
                    ]
                )
                batch_count += 1
            if batch_count == 0:
                break
            try:
                proc.stdin.write(out.getvalue().encode("utf-8"))
                proc.stdin.flush()
                inserted += batch_count
            except BrokenPipeError:
                failed += batch_count
                break
            sleep_until(start, inserted + failed, args.rate)
    finally:
        proc.stdin.close()
    return_code = proc.wait()
    stdout = proc.stdout.read() if proc.stdout is not None else b""
    stderr = proc.stderr.read() if proc.stderr is not None else b""
    if return_code != 0:
        failed += max(0, total_rows - inserted)
        print(stdout.decode("utf-8", "replace"), file=sys.stderr)
        print(stderr.decode("utf-8", "replace"), file=sys.stderr)

    elapsed = time.perf_counter() - start

    def count_rows() -> int:
        sql = f"SELECT count(*) FROM factory_ticks WHERE run_id = '{args.run_id}';"
        payload = run_psql(args.container, args.user, args.db, sql).decode("utf-8")
        for line in payload.splitlines():
            stripped = line.strip()
            if stripped.isdigit():
                return int(stripped)
        return 0

    verified = verify_with_retries(count_rows, inserted)
    return result(args, inserted, failed, elapsed, verified)


def result(
    args: argparse.Namespace,
    inserted: int,
    failed: int,
    elapsed: float,
    verified: int,
) -> dict[str, object]:
    target = args.seconds * args.rate
    status = "pass" if failed == 0 and verified >= inserted else "fail"
    return {
        "system": args.target,
        "status": status,
        "run_id": args.run_id,
        "target_rate": args.rate,
        "target_rows": target,
        "inserted_rows": inserted,
        "failed_rows": failed,
        "verified_rows": verified,
        "elapsed_sec": round(elapsed, 3),
        "achieved_rows_per_sec": round(inserted / elapsed, 2) if elapsed > 0 else 0,
        "symbols": args.symbols,
        "batch_size": args.batch_size,
    }


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--rate", type=int, default=10_000)
    parser.add_argument("--batch-size", type=int, default=500)
    parser.add_argument("--symbols", type=int, default=100)
    parser.add_argument("--run-id", default=f"factory{int(time.time())}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="target", required=True)

    zepto = sub.add_parser("zeptodb")
    add_common(zepto)
    zepto.add_argument("--url", default="http://127.0.0.1:18123")

    influx = sub.add_parser("influxdb")
    add_common(influx)
    influx.add_argument("--url", default="http://127.0.0.1:18086")
    influx.add_argument("--org", default="zepto")
    influx.add_argument("--bucket", default="factory")
    influx.add_argument("--token", default="zepto-token")

    timescale = sub.add_parser("timescaledb")
    add_common(timescale)
    timescale.add_argument("--container", required=True)
    timescale.add_argument("--user", default="zepto")
    timescale.add_argument("--db", default="factory")

    args = parser.parse_args(argv)
    if args.seconds <= 0 or args.rate <= 0 or args.batch_size <= 0 or args.symbols <= 0:
        parser.error("seconds, rate, batch-size, and symbols must be positive")

    try:
        if args.target == "zeptodb":
            payload = run_zeptodb(args)
        elif args.target == "influxdb":
            payload = run_influxdb(args)
        elif args.target == "timescaledb":
            payload = run_timescaledb(args)
        else:
            raise AssertionError(args.target)
    except (urllib.error.URLError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"{args.target} workload failed: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(payload, sort_keys=True))
    return 0 if payload["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
