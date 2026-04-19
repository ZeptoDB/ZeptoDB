#!/usr/bin/env bash
# ============================================================================
# Multi-process cluster integration test
# ============================================================================
# Starts 2 data nodes as separate processes, then runs queries via a
# coordinator process to verify real TCP scatter-gather works.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../build"
DATA_NODE="${BUILD_DIR}/zepto_data_node"
PASS=0
FAIL=0

cleanup() {
    echo "Cleaning up..."
    kill "$PID1" "$PID2" 2>/dev/null || true
    wait "$PID1" "$PID2" 2>/dev/null || true
}
trap cleanup EXIT

if [[ ! -x "$DATA_NODE" ]]; then
    echo "ERROR: zepto_data_node not found. Run 'ninja' first."
    exit 1
fi

echo "=== Multi-Process Cluster Integration Test ==="

# Start data node 1: port 29001, 500 ticks, symbol 1
"$DATA_NODE" 29001 500 --symbol 1 &
PID1=$!

# Start data node 2: port 29002, 300 ticks, symbol 2
"$DATA_NODE" 29002 300 --symbol 2 &
PID2=$!

# Wait for nodes to be ready
sleep 1

echo "Data nodes started: PID1=$PID1 PID2=$PID2"

# Test 1: Ping each node
echo -n "Test 1: Ping node 1... "
if python3 -c "
import socket, struct
s = socket.socket()
s.settimeout(2)
s.connect(('127.0.0.1', 29001))
# RpcHeader: magic(4) + type(4) + request_id(4) + payload_len(4) + epoch(8) = 24 bytes
magic = 0x41504558
s.sendall(struct.pack('<IIIIQ', magic, 3, 0, 0, 0))  # type=PING(3)
resp = b''
while len(resp) < 24:
    chunk = s.recv(4096)
    if not chunk: break
    resp += chunk
s.close()
# Response should be PONG (type=4)
rmag, rtype = struct.unpack('<II', resp[:8])
assert rmag == magic and rtype == 4
" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

echo -n "Test 2: Ping node 2... "
if python3 -c "
import socket, struct
s = socket.socket()
s.settimeout(2)
s.connect(('127.0.0.1', 29002))
magic = 0x41504558
s.sendall(struct.pack('<IIIIQ', magic, 3, 0, 0, 0))
resp = b''
while len(resp) < 24:
    chunk = s.recv(4096)
    if not chunk: break
    resp += chunk
s.close()
rmag, rtype = struct.unpack('<II', resp[:8])
assert rmag == magic and rtype == 4
" 2>/dev/null; then
    echo "PASS"; PASS=$((PASS+1))
else
    echo "FAIL"; FAIL=$((FAIL+1))
fi

# Test 3: Query node 1 directly
echo -n "Test 3: COUNT(*) on node 1... "
RESULT=$(python3 -c "
import socket, struct
s = socket.socket()
s.settimeout(2)
s.connect(('127.0.0.1', 29001))
magic = 0x41504558
sql = b'SELECT count(*) FROM trades WHERE symbol = 1'
s.sendall(struct.pack('<IIIIQ', magic, 1, 1, len(sql), 0) + sql)  # type=SQL_QUERY(1)
resp = b''
while len(resp) < 24:
    resp += s.recv(4096)
rmag, rtype, rid, plen = struct.unpack('<IIII', resp[:16])
while len(resp) < 24 + plen:
    resp += s.recv(4096)
payload = resp[24:]
# Parse: error_len(4) + error + col_count(4) + ... + row_count(4) + rows
elen = struct.unpack('<I', payload[:4])[0]
off = 4 + elen
ncols = struct.unpack('<I', payload[off:off+4])[0]
off += 4
for _ in range(ncols):
    nlen = struct.unpack('<I', payload[off:off+4])[0]
    off += 4 + nlen + 1  # name + type byte
nrows = struct.unpack('<I', payload[off:off+4])[0]
off += 4
val = struct.unpack('<q', payload[off:off+8])[0]
print(val)
s.close()
" 2>&1)

if [[ "$RESULT" == "500" ]]; then
    echo "PASS (got 500 rows)"
    PASS=$((PASS+1))
else
    echo "FAIL (got: $RESULT)"
    FAIL=$((FAIL+1))
fi

# Test 4: Query node 2 directly
echo -n "Test 4: COUNT(*) on node 2... "
RESULT=$(python3 -c "
import socket, struct
s = socket.socket()
s.settimeout(2)
s.connect(('127.0.0.1', 29002))
magic = 0x41504558
sql = b'SELECT count(*) FROM trades WHERE symbol = 2'
s.sendall(struct.pack('<IIIIQ', magic, 1, 2, len(sql), 0) + sql)
resp = b''
while len(resp) < 24:
    resp += s.recv(4096)
rmag, rtype, rid, plen = struct.unpack('<IIII', resp[:16])
while len(resp) < 24 + plen:
    resp += s.recv(4096)
payload = resp[24:]
elen = struct.unpack('<I', payload[:4])[0]
off = 4 + elen
ncols = struct.unpack('<I', payload[off:off+4])[0]
off += 4
for _ in range(ncols):
    nlen = struct.unpack('<I', payload[off:off+4])[0]
    off += 4 + nlen + 1
nrows = struct.unpack('<I', payload[off:off+4])[0]
off += 4
val = struct.unpack('<q', payload[off:off+8])[0]
print(val)
s.close()
" 2>&1)

if [[ "$RESULT" == "300" ]]; then
    echo "PASS (got 300 rows)"
    PASS=$((PASS+1))
else
    echo "FAIL (got: $RESULT)"
    FAIL=$((FAIL+1))
fi

# Test 5: Cross-node query via C++ test binary
echo -n "Test 5: Cross-node scatter-gather... "
RESULT=$("${BUILD_DIR}/tests/zepto_tests" --gtest_filter='DistributedSelect.*' 2>&1 | tail -1)
if echo "$RESULT" | grep -q "PASSED"; then
    echo "PASS"
    PASS=$((PASS+1))
else
    echo "FAIL ($RESULT)"
    FAIL=$((FAIL+1))
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
