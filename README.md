# defiant-storage-sqlite

The SQLite backend for `defiant-storage`. The caller passes a directory.
Each tenant is one database file in that directory, named by the lowercase
hex of the tenant bytes plus `.db`. The tenant string is never a path.
Isolation and the writer queue stay in the port.

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
does not know (another layout version, someone else's tables, the other kind
of store) is refused with a sentence naming the cause and left byte for byte
as it was. `documentation/sqlite.md` has the files, pragmas, connections and
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
steps, connections, a killed write, the cache and the key store's bytes;
`testSystem` runs two stores on one tenant file, a thread each, which never
take one counter value or hold live leases on one stream at once.
`testMigration` is the migration corpus gate: a committed store at SQL schema
version 1 (`tests/corpus/schema_1.sql`) read back row for row, then changed by
the steps a later layout takes, losing none. `check-model` plants eight defects
(an index step dropped, a file of other tables opened, DDL outside a step,
`secure_delete` off, reads on the write connection, the subject index step
dropped, the synchronous default weakened, the field index without its
language) and `check-defects` one (a
step that loses a row), and requires a named test to fail for each.

```bash
make test PREFIX=/path/to/prefix
```
