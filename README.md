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

Search tuning (session `SET` variables; the defaults are the measured
quality-first operating points):

```sql
SET sextant_probe_fraction = 0.25;  -- probe budget (index default 0.5 ≈ 0.99 recall@10;
                                    --  lower trades recall for speed)
SET sextant_fastscan_w = 4096;      -- shortlist width (default max(k, 1000))
SET sextant_rerank = false;         -- skip decoded-distance rerank (default on)
SET sextant_exhaustive = true;      -- probe every leaf (exact ranking)
SET sextant_search_threads = 4;     -- within-query scan parallelism (default 1)
```

Full reference for all options and settings:
[docs/SEXTANT.md](docs/SEXTANT.md).

This repository started from the DuckDB extension template; template
mechanics (submodules, CI tooling, distribution) are documented in
[docs/NEXT_README.md](docs/NEXT_README.md).
