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
| `filter_cols` | none | Comma-separated table columns (INTEGER, BIGINT, FLOAT, DOUBLE, VARCHAR, VARCHAR[], BOOLEAN, DATE, TIMESTAMP) indexed for filtered search. DATE/TIMESTAMP are stored as exact INT64 epochs (days / µs); DOUBLE is stored as binary32 with an exact SQL re-filter (FLOAT precision shapes the search, SQL decides the result). TIMESTAMP_NS is rejected. VARCHAR[] columns become engine set columns (≤ 255 elements/row, elements ≤ 64 KiB) supporting the CONTAINS family below. |
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

### Session settings (SET vars)

All knobs default to the engine's measured operating points; **start
with the defaults and tune only against an observed symptom** (the
recipes below). Values are validated when a sextant query runs, and
the top-k rewrite and the `sextant_query()` debug function share the
same tuning path.

| Setting | Default | Meaning |
|---|---|---|
| `sextant_search_threads` | 1 | Within-query parallel scan threads (0/1 = serial; DuckDB parallelizes across queries). |
| `sextant_probe_fraction` | 0 (index default) | Corpus-fraction probe budget in [0, 1]. New trees default to 0.5 (~0.99 recall@10); lower trades recall for speed (0.25 ≈ 0.96 recall@10 on 100K+ corpora). |
| `sextant_fastscan_w` | 0 (engine default) | Shortlist width (engine default = max(k, 1000)). |
| `sextant_rerank` | true | Rerank the shortlist by decoded distance (the default quality contract; turning it off also drops the adaptive-W cut). |
| `sextant_exhaustive` | false | Probe every leaf — exact ranking, overrides the probe budget and W. |
| `sextant_leaf_cache_mb` | 0 (off) | DRAM budget (MiB) for the engine's leaf-extent W-TinyLFU cache. Binds at the **first attach of each index per database open** — set it before first use; mid-session changes do not re-open an attached handle. Never slower than mmap at any size; pays off when the hot leaf set fits (repeated/hot query workloads — measured ~17% on a repeated uniform batch, up to ~2x on hot query distributions). |
| `sextant_plane_cache_mb` | 0 (off) | Same, for the routing-plane cache. Same binding rules. |

**"Latency/QPS matters more than the last few points of recall"**
(serving under load, cheap first-stage retrieval):
```sql
SET sextant_probe_fraction = 0.25;  -- ~0.96 recall@10; 0.1 ≈ 0.92-0.94
```
This is the main dial. Measured on 100K–1M+ corpora: the default 0.5
targets ~0.99 recall@10 at half the flat-scan cost; every halving
buys roughly a proportional scan reduction at a known recall cost.
If you don't know your recall target, measure first (below) — don't
tune blind.

**"Results must be exact"** (ground truth, debugging, recall
measurement, or tiny tables where exactness is affordable):
```sql
SET sextant_exhaustive = true;
```
Probes every leaf and ranks exactly — the output is identical to a
brute-force sort. Use it to compute your recall baseline against your
own workload, then pick the smallest `sextant_probe_fraction` that
holds the recall you need. On large corpora it is a full scan:
expect it to be slow.

**"Top-k comes back thinner than k under heavy filtering"**
(selective predicates reject most of the shortlist, e.g. rare
`language = 'ja'` or `list_contains` on a rare tag):
```sql
SET sextant_fastscan_w = 8000;  -- widen the shortlist
```
Candidates rejected by filters never enter the ranking; a shortlist
of W can return at most W-filtered rows. Rule of thumb: W ≳ k /
predicate pass rate (a predicate passing ~1% of rows with k = 100
wants W ≳ 10000). Also raise W when k itself is large (k near/above
the 1000 default).

**"One expensive query feels slow, and it's the only query running"**
(large k, wide probe budget, or heavy filtering; DuckDB has nothing
else to parallelize across):
```sql
SET sextant_search_threads = 8;
```
Spreads one query's leaf scans across threads. Leave at 1 for
concurrent workloads — DuckDB already parallelizes across queries,
and per-query fan-out then oversubscribes.

**`sextant_rerank = false`** is the one knob with no common use case:
it skips the decoded-distance rerank for raw speed, but the fastscan
scores rank lossily (up to -25pp recall on flat codes) and it also
disables the adaptive-W cut. Reach for it only if you have measured
that rerank dominates latency and you accept approximate ordering.

**"Serving the same corpus repeatedly, DRAM to spare"** (hot query
distributions, e.g. RAG with recurring query themes):
```sql
SET sextant_leaf_cache_mb = 1024;  -- engine-owned W-TinyLFU leaf cache
SET sextant_plane_cache_mb = 128;
```
Rule of thumb: size the leaf cache to the *hot* leaf set, not the
tree — a uniform-random workload over a corpus larger than the cache
gains little (the admission gate falls back to mmap, never slower);
repeated/hot queries gain up to ~2x. These bind at the first use of
each index per database open, so set them before the first query.

## Querying

The optimizer rewrites `ORDER BY array_distance(v, q) LIMIT k` into a
`SEXTANT_INDEX_SCAN` (check with EXPLAIN). Supported shapes:

- **Plain top-k**: `... ORDER BY array_distance(v, q) LIMIT 10`
- **Filtered**: WHERE predicates on filter columns — `=`, `!=`, `<`,
  `<=`, `>`, `>=`, `IN`, `NOT IN`, `BETWEEN`-style ranges,
  `IS [NOT] NULL`, and `LIKE 'prefix%'` (translated to an engine
  prefix predicate).
  SQL NULL semantics are exact on trees built with nullable columns
  (all extension builds since 2026-09-19).
- **Set membership** (`VARCHAR[]` filter columns):
  `list_contains(tags, 'x')` (aka `array_has`),
  `list_has_any(tags, ['a','b'])` (aka `array_has_any`, intersects),
  `list_has_all(tags, ['a','b'])` (aka `array_has_all`, subset), and
  `tags @> ['a']` (sublist containment = `list_has_all`). NULL lists
  never match; `list_has_any(tags, [])` falls back to the exact SQL
  path (always false); `list_has_all(tags, [])` is vacuously true.
- **Inner product**: `... ORDER BY array_inner_product(v, q) DESC LIMIT k`
  on `metric='ip'` trees.
- **Delta serving**: with `delta_scan`, appended rows are brute-forced
  and merged exactly; predicates apply to delta rows too. Merge cost is
  ~0.25 ms per 1k delta rows (measured to 100k rows).
- **Concurrency**: searches are safe from multiple connections/threads
  (8-thread stress over lazy attach, scans and concurrent delta
  INSERTs is covered by `test/sql/sextant_concurrency.test`; the engine
  search itself is additionally TSan-verified). The engine handle is
  attached exactly once per index and never reopened while in use.

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

## Debug functions

`sextant_query(table, index_name, query_vector, k)` returns
`(row_id BIGINT, distance FLOAT)` directly from the engine. Parity with
the rewrite for L2/IP trees and delta serving; **no WHERE predicates** —
filtered queries must use the SQL top-k form. Distances are engine
scores (family-specific scale) except on the delta path, where they are
recomputed exactly.

`sextant_query_batch(table, index_name, query_vectors, k)` answers many
queries in one engine call — the batch path uses union leaf probing
(each leaf is scanned once for all queries that need it) and the
batched scan kernels, so it is much faster per query than repeated
`sextant_query` calls at serving scale. `query_vectors` is a list of
numeric array literals (same dimension as the index, up to 65536
queries); rows are `(query_index BIGINT 1-based, row_id BIGINT,
distance FLOAT)`. Same contract as `sextant_query`: same tuning SET
vars, delta serving included, no predicates.

```sql
SET sextant_search_threads = 8;   -- fan the batch out (see below)
SELECT * FROM sextant_query_batch('docs', 'idx',
    [q1, q2, q3], 10)
ORDER BY query_index, distance;
```

Throughput note (measured on CulturaX 2.9M×768, 256 queries, warm):
per-query cost matches the native C API (the extension adds no
per-call overhead), and the batch call scales with
`sextant_search_threads` — 1 thread ≈ 56 QPS, 8 threads ≈ 128 QPS,
engine-side throughput at 8 threads exceeds the single-query C API
(union probing + batched kernels). Set it above 1 when a large batch
is the whole workload, exactly like `sextant_search_threads` guidance
above.



## Immutability

DELETE and UPDATE on an indexed table always abort with a hint to drop
and re-create. INSERT is rejected unless every sextant index on the
table has `delta_scan`. DROP INDEX deletes the sidecar file.

## Stale index detection

A sidecar that cannot be opened or whose tree UUID does not match the
index metadata makes top-k queries over that table fail with the
precise error (DROP INDEX still works to clean up). A prebuilt attach
is rejected outright if the tree's row count differs from the table's.
