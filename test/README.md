# Tests

`sql/` holds the SQLLogicTests. Run them with:

```sh
make test_release
```

`run_lifecycle_demo.sh` exercises the attach/drop/rename lifecycle against a
real DuckDB database file using the `test/data/small.tree` fixture (rebuild
the fixture with the engine CLI `build-tree` if the manifest format changes).
