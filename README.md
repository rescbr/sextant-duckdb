# Sextant DuckDB Extension

IVF-tree ANN index for DuckDB, backed by the sextant engine. Natural
SQL top-k queries are rewritten to an index scan; filter columns,
payloads, inner-product metric and append-only delta serving are
supported.

- Build: `make release` (statically links `../sextant-engine/build-x86`)
- Test: `make test_release` (sqllogictests)
- Full SQL reference: [docs/SEXTANT.md](docs/SEXTANT.md)

```sql
LOAD sextant;
CREATE INDEX idx ON docs USING sextant (embedding)
  WITH (path = 'docs.tree',
        filter_cols = 'language,year',
        payload_col = 'text');

-- Served by the index (SEXTANT_INDEX_SCAN):
SELECT * FROM docs
WHERE language = 'en'
ORDER BY array_distance(embedding, query_vec)
LIMIT 10;
```

This repository started from the DuckDB extension template; template
mechanics (submodules, CI tooling, distribution) are documented in
[docs/NEXT_README.md](docs/NEXT_README.md).
