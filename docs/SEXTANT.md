# Sextant SQL Reference

Everything the extension exposes, with semantics and measured numbers.
Benchmarks below: CulturaX 2.9M x 768 + text payload (recall vs exact
SQL ground truth, 10-100 queries).

## Creating an index

```sql
CREATE INDEX <name> ON <table> USING sextant (<vector_column>)
  WITH (path = '<tree file>', ...options);
```

The vector column must be a fixed-size `FLOAT[d] ARRAY`. The tree is
built from the table into `path` (absolute, or relative to the database
directory). The table must have no uncommitted data at CREATE time.

### Options

| Option | Default | Meaning |
|---|---|---|
| `path` | required | Sidecar `.tree` file (created by the build). |
| `prebuilt` | off | Attach an existing tree instead of building; the tree must be rowid-identical to the table (row counts must match). |
| `filter_cols` | none | Comma-separated table columns (INTEGER, BIGINT, FLOAT, VARCHAR, BOOLEAN) indexed for filtered search. |
| `payload_col` | none | VARCHAR column stored in the tree, fetched per result row (avoids table fetches). |
| `delta_scan` | off | Append-only serving: INSERT is allowed; rows past the build are brute-forced at query time and merged with the tree's results. DELETE/UPDATE remain rejected. |
| `metric` | `l2` | `l2` (array_distance) or `ip` (array_inner_product, DESC ordering). |
| `cardinality` | `auto` | Filter-column value tracking for selectivity: `on` / `off` / `auto` (track, reject identity-like) or per-column `name=mode` lists. Auto fixes tiny-match-set recall at negligible cost (~4MB/1M entries). |
| `build_threads` | engine default | Engine build parallelism (8 is safe: identical recall to 1 on CulturaX, ~2x faster). |
| `stage_budget_mb` | 512 | Engine emission-pass cluster staging RAM (MiB). 128 cuts peak ~1 GiB at zero wall cost on CulturaX. |
| `staging_bytes` | 256 MiB | Push-staging RAM before spill to disk. |
| `metrics_file` | none | JSONL per-phase build metrics (incl. per-phase `rss_bytes`). |

### Build memory

The extension clamps DuckDB's `memory_limit` to 1 GiB for the duration
of a CREATE INDEX statement and restores it afterwards (single-pass
scans derive nothing from buffer cache). Scan parallelism still follows
the session `threads` setting — wide thread counts add in-flight chunk
memory only:

```sql
SET threads = 8;   -- before big builds: -1.3 GiB peak, no wall cost
```

Measured (CulturaX): untuned 20 GiB peak -> 5.3 GiB with the clamp;
4.0 GiB with `threads=8`; 4.2 GiB and 1:37 wall with `build_threads=8`
(engine CLI build of the same corpus: 1:21, 11.4 GiB).

## Querying

The optimizer rewrites `ORDER BY array_distance(v, q) LIMIT k` into a
`SEXTANT_INDEX_SCAN` (check with EXPLAIN). Supported shapes:

- **Plain top-k**: `... ORDER BY array_distance(v, q) LIMIT 10`
- **Filtered**: WHERE predicates on filter columns — `=`, `!=`, `<`,
  `<=`, `>`, `>=`, `IN`, `BETWEEN`-style ranges, `IS [NOT] NULL`,
  and `LIKE 'prefix%'` (translated to an engine prefix predicate).
  SQL NULL semantics are exact on trees built with nullable columns
  (all extension builds since 2026-09-19).
- **Inner product**: `... ORDER BY array_inner_product(v, q) DESC LIMIT k`
  on `metric='ip'` trees.
- **Delta serving**: with `delta_scan`, appended rows are brute-forced
  and merged exactly; predicates apply to delta rows too. Merge cost is
  ~0.25 ms per 1k delta rows (measured to 100k rows).

DESC ordering (L2), OFFSET, and multiple ORDER BY terms are not
rewritten; they run through DuckDB's exact path.

Not rewritten paths still return correct results — the rewrite is an
optimization, and an unusable index (missing/swapped sidecar) fails
loudly at query time with the precise reason rather than silently.

### Measured quality/latency (CulturaX)

| Query | Recall@10 | Speed vs exact |
|---|---|---|
| Unfiltered top-k | 0.9505 (vs exact GT) | — |
| Filtered (6 filter shapes incl. LIKE, tiny sets) | **1.000** | **218x** |
| Inner product (metric='ip') | **1.000** | **39x** |

## Debug function

`sextant_query(table, index_name, query_vector, k)` returns
`(row_id BIGINT, distance FLOAT)` directly from the engine. Parity with
the rewrite for L2/IP trees and delta serving; **no WHERE predicates** —
filtered queries must use the SQL top-k form. Distances are engine
scores (family-specific scale) except on the delta path, where they are
recomputed exactly.

## Immutability

DELETE and UPDATE on an indexed table always abort with a hint to drop
and re-create. INSERT is rejected unless every sextant index on the
table has `delta_scan`. DROP INDEX deletes the sidecar file.

## Stale index detection

A sidecar that cannot be opened or whose tree UUID does not match the
index metadata makes top-k queries over that table fail with the
precise error (DROP INDEX still works to clean up). A prebuilt attach
is rejected outright if the tree's row count differs from the table's.
