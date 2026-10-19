# The entity port and the key store on SQLite

**Status:** describes what shipped in story S3 of the storage rewrite
(2026-10-18).

`model.h` opens the entity port (`defiant-storage`'s `model.h`) and the key
store port (its `keystore.h`) over SQLite files. Every statement either one
runs is written by `defiant-storage-sql` (its `documentation/sql.md` has the
tables and statements). This library gives it two things, and owns the rest:

- **the dialect** (`gdefssqlite_dialect()`), which is data;
- **the executor**: files, connections, the statement cache, the pragmas.

The key/bytes API of `sqlite.h` is separate and unchanged. Its files are the
milestone-1 layout, which the v2 open refuses (section 3).

## 1. The dialect

| Member | SQLite |
| --- | --- |
| quotes | `"` and `"` |
| placeholder | `?`, by position |
| types | `INTEGER` for ids, integers, decimals (the unscaled int64), booleans, dates and date-times; `REAL` for floats and order keys; `TEXT` for text and references; `BLOB` for bytes |
| collation | none: `BINARY`, SQLite's default, compares by bytes |
| upsert | `ON CONFLICT (...) DO UPDATE` |
| `RETURNING` | yes (SQLite 3.35 and later; the vendored build is 3.53.4) |
| NULL order | low, so the SQL layer writes no `NULLS` clause ascending |
| transactional DDL | yes |
| identifier limit | 63, the smallest engine's, though SQLite has none |
| parameters, columns | 32766 and 2000, SQLite's defaults |
| transactions | `BEGIN` for a read, `BEGIN IMMEDIATE` for a write, `COMMIT`, `ROLLBACK` |

`BEGIN IMMEDIATE` takes the write lock at the begin. A write that has to wait
for another process waits there, in SQLite's busy handler, and never half way
through with a snapshot it cannot upgrade.

## 2. Files and pragmas

A data store is a directory with one file per tenant: the lowercase hex of the
tenant's bytes, then `.db` (AD-14 placement, as `sqlite.h` names them; the
tenant's bytes are never a path). A key store is a directory with one file per
tenant named by the same hex and `.keys.db`, so the two never share a file even
in one directory; deployments keep them in separate directories, because their
backups are separate sets (AD-19).

Every connection, after the checks of section 3, runs:

| Pragma | Why |
| --- | --- |
| `journal_mode=WAL` | readers see the last commit while a write is open; a write killed part way leaves frames with no commit, which are not the database |
| `synchronous=FULL` | a commit is on disk when it returns |
| `secure_delete=ON` (key store only) | a deleted key's bytes are zeroed in its page, not left in a free block; read back and required |
| `query_only=ON` (read connections) | a read connection cannot write |

and is opened with loadable extensions off, `SQLITE_DBCONFIG_DEFENSIVE` on and
`trusted_schema` off. After each key-store write that commits, the WAL is
checkpointed and truncated (`SQLITE_CHECKPOINT_TRUNCATE`), so what the commit
replaced is in neither file. A reader in the way leaves that to the next
write's checkpoint; the key store's reads are each one short transaction.

## 3. What a connection checks first

Before a new connection to an existing file is used for anything, the file's
catalog is read with `PRAGMA main.table_list`, which reads the schema and
writes nothing. Then:

| The file holds | Data store | Key store |
| --- | --- | --- |
| no table | a new store: a write makes it, a read finds nothing | the same |
| `dss_schema` | checked below | refused: a data store |
| `dsk_schema` | refused: a key store | checked below |
| `defiant_record`, `defiant_migration` or `record_v0` (milestone 1) | refused | refused: a data store |
| anything else | refused | refused |

A store of the right kind has its version read through the SQL layer
(`gdefssql_store_version()`): another version than this build's is refused,
`GDEFSTOR_ERR_UNSUPPORTED` when newer and `GDEFSTOR_ERR_FORMAT` when older.
Every other refusal is `GDEFSTOR_ERR_FORMAT`. A refusal happens before
`journal_mode=WAL` and before the SQL layer can create a table, so a refused
file is left byte for byte as it was. It is reported through
`GDEFSSQLITE_ModelOptions::on_refusal`, or as one line on stderr when that is
NULL, as a sentence naming the file and the cause:

```
defiant-storage-sqlite: /srv/t/7431.db holds the milestone-1 layout (a defiant_record table and no dss_schema); it is refused and left as it is
```

Nothing is released, so nothing converts an old file (SPEC, "Constraints").

## 4. Connections

Each tenant has one write connection, used by one write at a time, and read
connections of its own. A read never runs on the write connection, so a read
made while a write is open sees the last commit, as the port requires; the
write's own reads run on its connection, inside its transaction. Idle
connections are kept: up to `read_connections` readers per tenant and
`idle_connections` in the whole store, the least recently used closed beyond
that. A connection the SQL layer closes after an error, or one left inside a
transaction, is closed and never reused (AD-14).

A begin that finds another process writing the tenant waits up to
`busy_timeout_ms` (default 5 s) while SQLite's busy handler retries, then is
`GDEFSTOR_ERR_BUSY`. The SQL layer's first write to a tenant creates its six
tables in a transaction of its own; two processes racing to do it serialise on
`BEGIN IMMEDIATE`, and the second finds the tables made.

Indexes are built after the step that declares them commits, never inside it
(AD-15; defiant-storage-sql's `sql.md`, section 3): on the write connection,
outside any transaction, as a plain `CREATE INDEX IF NOT EXISTS` (SQLite builds
an index atomically, so this dialect has no `index_build`, `index_valid` or
`index_drop`), each then marked built in a transaction of its own. A build
that fails (a busy file) leaves the field reading `GDEFSTOR_INDEX_INDEXING`
until the next commit of the tenant builds it; queries answer the same either
way.

## 5. The statement cache

Each connection keeps up to `statement_cache` prepared statements (default
64), keyed by their text, prepared with `SQLITE_PREPARE_PERSISTENT`. The SQL
layer's texts repeat exactly per tenant and query shape, and a connection
belongs to one tenant, so the cache is per tenant and query (AD-14). A
finished statement is reset and its bindings cleared before it goes back; the
least recently used is finalized when the cache is full.

## 6. Counting what a statement did

`GDEFSSQLITE_ModelOptions::on_statement` is called for each statement when the
SQL layer finalizes it, with its text and what `sqlite3_stmt_status` counted:
steps of a full table scan, virtual-machine steps, sorts, rows put in an
automatic index, and the rows it returned. The scale tests use it to show,
without timing anything, that each indexed query shape at 10,000 entities
takes no full-scan step and a number of steps set by its answer, and run
`EXPLAIN QUERY PLAN` on the same text to see the index named.

What the counts show about the SQL layer's shapes on SQLite:

- A filter on an indexed field is `rid IN (SELECT ent FROM <current table>
  WHERE v = ?)`; SQLite runs the subquery on the index `(v, ent)` and seeks
  each entity by its row.
- A sort by a field joins the root current table with `LEFT JOIN`, so that an
  entity with no value sorts first. With a filter that an index serves, the
  sort is of the answer. Without one, the sorted page of a whole type reads
  every entity of the type once (the `LEFT JOIN` and the absent-first order
  keep the index from giving the order), bounded by the type's entities, not
  more.
- A page of every entity in creation order reads offset plus limit rows; its
  total counts every entity, which is its answer.
- Blind-index equality is `rid IN (SELECT b.ent FROM tN_b b JOIN tN_cG c ON
  ... vfrom ... WHERE b.fld = ? AND b.mac = ?)`: the blind table's
  `(fld, mac)` index drives it and each current version is sought by key. The
  history lookup reads the same index.
- The subject lookup ("everything about this person") reads the catalog's
  list of subject columns (a scan of `dss_name`, whose size is the schema's,
  not the data's), then each group table through its subject index
  `tN_jG_C`. Erasure deletes a subject's blind rows through `tN_bs`.

## 7. What a store opened here does not do

`gdefssqlite_model_store_create()` serves the entity port only. The key/bytes
rows of `store.h` and the files built on it (records, blobs, the outbox) have
no home in these files until they move into the entity transaction: a read
finds none, and a write that holds one is `GDEFSTOR_ERR_UNSUPPORTED` at commit.
