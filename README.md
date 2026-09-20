# Sextant DuckDB Extension

IVF-tree ANN index for DuckDB, backed by the sextant engine. Natural
SQL top-k queries are rewritten to an index scan; filter columns
(including VARCHAR[] set membership), inner-product metric and
append-only delta serving are supported.

- Build: `make release` (statically links `../sextant-engine/build-x86`)
- Test: `make test_release` (sqllogictests)
- Full SQL reference: [docs/SEXTANT.md](docs/SEXTANT.md)

```sql
LOAD sextant;
CREATE INDEX idx ON docs USING sextant (embedding)
  WITH (path = 'docs.tree',
        filter_cols = 'language,source,year');

-- Served by the index (SEXTANT_INDEX_SCAN):
SELECT * FROM docs
WHERE language = 'en'
  AND list_contains(tags, 'release-notes')
ORDER BY array_distance(embedding, query_vec)
LIMIT 10;
```

Search tuning (session `SET` variables). **The defaults are measured
quality-first operating points — don't tune without a symptom.** The
quick decision guide (full recipes in [docs/SEXTANT.md](docs/SEXTANT.md)):

| Symptom | Knob |
|---|---|
| Need more QPS, can give up a few points of recall | `SET sextant_probe_fraction = 0.25;` (default 0.5 ≈ 0.99 recall@10; 0.25 ≈ 0.96) |
| Need exact results / want to measure recall on your data | `SET sextant_exhaustive = true;` (brute-force-equivalent, slow on big corpora) |
| Fewer than k results under a selective filter | `SET sextant_fastscan_w = 8000;` (W ≳ k / filter pass rate) |
| One big query is the whole workload and feels slow | `SET sextant_search_threads = 8;` |
| No symptom | **Use the defaults** (`sextant_rerank = true` stays on — turning it off costs recall for little speed) |

**Serving preset** — max throughput for a dedicated batch-serving session
(measured 56 → 184 QPS on 2.9M×768; recall drops to ~0.96 — opt in
explicitly, don't ship it as a default):

```sql
SET sextant_search_threads = 8;     -- one batch is the whole workload
SET sextant_probe_fraction = 0.25;  -- ~0.96 recall@10
SET sextant_leaf_cache_mb = 1024;   -- hot-query workloads; engine-owned DRAM
SET sextant_plane_cache_mb = 128;   -- (not visible to DuckDB memory_limit)
```

Full reference for all options and settings:
[docs/SEXTANT.md](docs/SEXTANT.md).
