#!/usr/bin/env bash
# Destructive sextant-index lifecycle demo: DROP deletes the sidecar,
# uuid guard, missing-sidecar error, crash-window orphan. Complements
# test/sql/sextant_lifecycle.test (which covers the non-destructive paths).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DUCKDB="${REPO:?}/build/release/duckdb"
ENGINE="${REPO}/../sextant-engine/build-x86/tools/sextant"
FIXTURE="${REPO}/test/data/small.tree"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

run_sql() { "$DUCKDB" "$WORK/db.duckdb" 2>&1 | grep -vE '^\[|WARNING|arrow|lambda|duckdb\.org|^$'; }

cp "$FIXTURE" "$WORK/small.tree"

echo "== create index (relative path, dbdir-resolved) + insert fence =="
"$DUCKDB" "$WORK/db.duckdb" -c "
LOAD sextant;
CREATE TABLE t AS SELECT list_transform(range(32), j -> ((rowid*32+j)%97)::FLOAT)::FLOAT[32] AS v FROM range(2000) tbl(rowid);
CREATE INDEX idx ON t USING sextant (v) WITH (path = 'small.tree', prebuilt);" >/dev/null 2>&1

echo "== DROP INDEX deletes the sidecar =="
"$DUCKDB" "$WORK/db.duckdb" -c "LOAD sextant; DROP INDEX idx;" >/dev/null 2>&1
[ ! -e "$WORK/small.tree" ] && echo "OK: sidecar removed" || { echo "FAIL: sidecar survived DROP"; exit 1; }

echo "== crash-window orphan: sidecar re-created, index gone, table intact =="
cp "$FIXTURE" "$WORK/small.tree"
N=$("$DUCKDB" "$WORK/db.duckdb" -noheader -list -c "LOAD sextant; SELECT count(*) FROM t;" 2>/dev/null | tail -1)
[ "$N" = "2000" ] && echo "OK: table intact ($N rows)" || { echo "FAIL: table broken ($N)"; exit 1; }

echo "== uuid guard: stale tree at the path is rejected =="
"$DUCKDB" "$WORK/db.duckdb" -c "LOAD sextant; CREATE INDEX idx ON t USING sextant (v) WITH (path = 'small.tree', prebuilt);" >/dev/null 2>&1
"$DUCKDB" "$WORK/db.duckdb" -c "LOAD sextant; CHECKPOINT;" >/dev/null 2>&1
"$ENGINE" build-tree --input="${REPO}/test/data/small.fbin" --index="$WORK/other.tree" >/dev/null 2>&1
cp "$WORK/other.tree" "$WORK/small.tree"
# The plan-time INSERT fence fires before the sidecar is re-opened, so
# exercise the index via the top-k rewrite (scan bind re-verifies).
OUT=$("$DUCKDB" "$WORK/db.duckdb" -c "LOAD sextant; SELECT count(*) FROM (SELECT rowid FROM t ORDER BY array_distance(v, list_transform(range(32), j -> 0.5::FLOAT)::FLOAT[32]) LIMIT 5);" 2>&1 || true)
echo "$OUT" | grep -q "stale or wrong file" && echo "OK: uuid mismatch rejected" || { echo "FAIL: $OUT"; exit 1; }

echo "== missing sidecar: clear error, db stays usable =="
rm -f "$WORK/small.tree"
OUT=$("$DUCKDB" "$WORK/db.duckdb" -c "LOAD sextant; SELECT count(*) FROM (SELECT rowid FROM t ORDER BY array_distance(v, list_transform(range(32), j -> 0.5::FLOAT)::FLOAT[32]) LIMIT 5);" 2>&1 || true)
echo "$OUT" | grep -q "cannot open sidecar" && echo "OK: missing sidecar reported" || { echo "FAIL: $OUT"; exit 1; }
N=$("$DUCKDB" "$WORK/db.duckdb" -noheader -list -c "LOAD sextant; SELECT count(*) FROM t;" 2>/dev/null | tail -1)
[ "$N" = "2000" ] && echo "OK: db still usable" || { echo "FAIL: db unusable"; exit 1; }

echo "ALL DEMO CHECKS PASSED"
