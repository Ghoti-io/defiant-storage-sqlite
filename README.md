# defiant-storage-sqlite

The SQLite backend for `defiant-storage`. The caller passes a directory.
Each tenant is one database file in that directory, named by the lowercase
hex of the tenant bytes plus `.db`. The tenant string is never a path.
Schema steps are numbered and forward-only. Isolation and the writer queue
stay in the port.

`gdefssqlite_store_create` returns a `GDEFSTOR_Store`. Close it with
`gdefstor_store_destroy`.

Depends on `defiant-storage` and `vendor-sqlite` through pkg-config. It does
not vendor a second SQLite amalgamation.

```c
GDEFSTOR_Store * store = NULL;
gdefssqlite_store_create("/var/defiant", NULL, NULL, &store);
```

```bash
make test PREFIX=/path/to/prefix
```

Its tests compile the port's conformance driver, installed beside the port.
A fixture in `tests/corpus/` is the pre-version-1 shape (`record_v0`).
Opening it copies that row into `defiant_record` and records schema version 1.
