# AGENTS.md

Guidance for coding agents (and humans in a hurry) working in this repo.

## What this is

Sextant DuckDB extension: an IVF-tree ANN IndexType for DuckDB, built on
the sextant engine (sibling `sextant-engine` repo, linked statically via
its C API). Requires DuckDB >= the vendored submodule version.

## Build & test

```sh
git submodule update --init --recursive   # duckdb + extension-ci-tools
make release
make test_release          # sqllogictest suite: 12 files, ~1000 assertions
```

**The engine is statically linked**: after ANY change to sextant-engine,
rebuild the extension (`make release`) before testing, or you'll test
against a stale engine.

No CI; the local suite is the contract. Error-path tests with index
settings need a FRESH index — already-attached indexes ignore later
setting changes.

## SQL surface (what the tests exercise)

- `CREATE INDEX ... USING sextant (vec) WITH (...)` — options: `path`,
  `prebuilt`, `delta_scan`, `filter_cols`, `build_threads`,
  `stage_budget_mb`, `metrics_file`
- `sextant_query(table, index, query_vec, k)`,
  `sextant_query_batch(table, index, queries, k)` (≤ 65536 queries)
- Tuning SET vars: `sextant_probe_fraction`, `sextant_fastscan_w`,
  `sextant_rerank`, `sextant_exhaustive`, `sextant_leaf_cache_mb`,
  `sextant_plane_cache_mb`
- Filter pushdown for scalar comparison predicates and the set family
  (`list_contains`/`array_has`/`has_any`/`has_all`/`@>`) on typed filter
  columns; delta rows are evaluated per-query with the same semantics

User docs: `README.md` + `docs/SEXTANT.md` (settings table, recipes,
serving preset). Keep both in sync when touching behavior.

## sqllogictest gotchas

- `statement error` matches a LITERAL substring of the error text — no
  regex, escape nothing
- WITH-clause booleans are bare words (`delta_scan`, not `= true`)
- `load __TEST_DIR__/x.db` creates a real database file in the temp dir
- Ground-truth comparisons vs the engine CLI: use `OFFSET 0+0` on the
  exact-side LIMIT to block the optimizer's rewrite

## Format coupling

The engine's tree-manifest format is v1, version-checked at open. If the
engine bumps the format, `test/data/small.tree` must be rebuilt with the
engine CLI (`build-tree --input test/data/small.fbin --index
test/data/small.tree`) and recommitted — the suite will fail loudly
otherwise.
