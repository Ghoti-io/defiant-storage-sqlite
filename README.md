# defiant-storage-sqlite

The SQLite backend for `defiant-storage`. The caller passes a directory.
Each tenant is one database file in that directory, named by the lowercase
hex of the tenant bytes plus `.db`. The tenant string is never a path.
Schema steps are numbered and forward-only. Isolation and the writer queue
stay in the port.

`gdefssqlite_store_create` returns a `GDEFSTOR_Store`. Close it with
`gdefstor_store_destroy`.

Depends on `defiant-storage`, `defiant-storage-sql` and `vendor-sqlite`
through pkg-config. It does not vendor a second SQLite amalgamation. Its tests
also need `defiant-storage-memory`, to compare answers with it.

## The entity port and the key store

`model.h` opens the entity port (`defiant-storage`'s `model.h`) on SQLite:
`gdefssqlite_model_store_create()` takes a directory and keeps one file per
tenant, in WAL mode, with a write connection and read connections of their own
per tenant and a statement cache per connection. Every statement is written by
`defiant-storage-sql`; this library is the SQLite dialect and the executor.
`gdefssqlite_keystore_create()` is the key store port in files of its own
(`<hex>.keys.db`), with `secure_delete` on and the WAL truncated after each
write, so a destroyed key's bytes are in no file afterwards. A file this layout
does not know (the milestone-1 files below, a newer version, the other kind of
store) is refused with a sentence naming the cause and left byte for byte as it
was. `documentation/sqlite.md` has the files, pragmas, connections and
refusals.

```c
GDEFSTOR_Store * store = NULL;
gdefssqlite_model_store_create("/var/defiant/data", NULL, NULL, NULL, &store);
```

Its tests: `testModel` runs the port's conformance suites (`model.inc`, with
the purchase-order fixture, and `keystore.inc`) with every query also answered
by the reference scan; `testAgree` is the seeded sweep against the reference
scan and the memory backend; `testScale` asserts rows-scanned bounds and query
plans at 10,000 entities; `testFiles` covers refused files, recorded schema
steps, connections, a killed write, the cache and the key store's bytes.
`check-model` plants five defects (an index step dropped, a milestone-1 file
opened, DDL outside a step, `secure_delete` off, reads on the write connection)
and requires a named test to fail for each.

## The key/bytes store

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
