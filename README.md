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

## Queries

The store answers the port's queries (`defiant-storage/query.h`) with one
`SELECT` over `defiant_record`, not by reading every row out and filtering in
C. What is pushed down, and what is not:

| Part of a query | Where it runs |
|---|---|
| The rows of the projection type | an index range search on the primary key (`key >= prefix AND key < prefix-with-the-colon-raised`) |
| The predicate tree: and, or, not, `=`, `!=`, `in`, `<` to `>=`, constants, and the semi-join to another projection type's row with the same entity | in the statement; a leaf is one call of `gdq_leaf`, and a joined row is a primary-key lookup of its key |
| The sort, its direction, the entity tiebreak, the offset and the limit | in the statement (`ORDER BY gdq_key(...)`, `LIMIT`, `OFFSET`) |
| The total | a second `SELECT count(*)` with the same predicate, in the same read transaction |
| Reading a column, and a field out of a JSON column | `gdq_leaf` and `gdq_key`, which decode the stored row with the port's own code |
| An index on a column | **not done**: a projection is one encoded value per key, so the predicate is evaluated on every row of the type. The cost is linear in the rows of the type. An index would need the columns stored apart from the encoded row, a change of every backend's layout (AD-14). |

Every value reaches the statement as a bound parameter, and the node that says
which column to read as a pointer parameter; the text of the statement is a
fixed vocabulary and `?`, and has no column name, type name or constant in it.
A tree that needs more bound values (a leaf of 200 constants costs 200) or
function arguments than the connection allows is answered
`GDEFSTOR_ERR_UNSUPPORTED` and the port scans instead; the answer is the same.

Its tests compile the port's conformance driver, installed beside the port.
A fixture in `tests/corpus/` is the pre-version-1 shape (`record_v0`).
Opening it copies that row into `defiant_record` and records schema version 1.
