/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage SQLite.
 *
 * Ghoti.io Defiant Storage SQLite is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * version 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage SQLite is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The entity port and the key store port on SQLite (model.h): the dialect
 * defiant-storage-sql needs, and the executor that runs its statements. Every
 * statement comes from that library. The only SQL text here is the dialect's
 * transaction statements and the pragmas that set up and check a connection.
 *
 * Per tenant there is one write connection and a pool of read connections;
 * each connection caches its prepared statements by their text. A new
 * connection to an existing file reads the file's catalog before anything
 * else touches it, and refuses a file that is not this layout or is a newer
 * version of it, so a refused file is never changed.
 */

#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <ghoti.io/defiant-storage-sql/defiant-storage-sql.h>
#include <ghoti.io/defiant-storage/model.h>
#include <ghoti.io/cutil/allocator.h>
#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ---- the dialect ------------------------------------------------------------ */

static const char kCommit[] = "COMMIT";

/* SQLite: INTEGER holds every int64, REAL every double exactly; BINARY, the
 * default collation, compares text by bytes; NULL sorts low. */
static void fill_dialect(GDEFSSQL_Dialect * d) {
  memset(d, 0, sizeof *d);
  d->size = sizeof *d;
  d->name = "sqlite";
  d->quote_open = "\"";
  d->quote_close = "\"";
  d->placeholder = "?";
  d->placeholder_numbered = 0;
  d->type_names[GDEFSSQL_COL_ID] = "INTEGER";
  d->type_names[GDEFSSQL_COL_INTEGER] = "INTEGER";
  d->type_names[GDEFSSQL_COL_DECIMAL] = "INTEGER";
  d->type_names[GDEFSSQL_COL_FLOAT] = "REAL";
  d->type_names[GDEFSSQL_COL_BOOLEAN] = "INTEGER";
  d->type_names[GDEFSSQL_COL_DATE] = "INTEGER";
  d->type_names[GDEFSSQL_COL_DATETIME] = "INTEGER";
  d->type_names[GDEFSSQL_COL_TEXT] = "TEXT";
  d->type_names[GDEFSSQL_COL_REFERENCE] = "TEXT";
  d->type_names[GDEFSSQL_COL_BYTES] = "BLOB";
  d->text_collation = NULL;
  d->upsert = GDEFSSQL_UPSERT_ON_CONFLICT;
  d->returning = 1;
  d->nulls_low = 1;
  d->ddl_transactional = 1;
  /* SQLite has no identifier limit; the smallest engine's is kept so that a
   * name made here is valid everywhere (AD-13). */
  d->max_identifier = 63;
  d->max_binds = 32766;  /* SQLITE_MAX_VARIABLE_NUMBER's default */
  d->max_columns = 2000; /* SQLITE_MAX_COLUMN's default */
  d->begin_read = "BEGIN";
  /* IMMEDIATE takes the write lock at the begin, so a write that has to wait
   * for another process waits there, in the busy handler, and never fails
   * half way with a snapshot that cannot be upgraded. */
  d->begin_write = "BEGIN IMMEDIATE";
  d->commit = kCommit;
  d->rollback = "ROLLBACK";
}

void gdefssqlite_dialect(GDEFSSQL_Dialect * out) {
  if (out != NULL) {
    fill_dialect(out);
  }
}

void gdefssqlite_model_options_default(GDEFSSQLITE_ModelOptions * out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  out->busy_timeout_ms = 5000;
  out->read_connections = 4;
  out->idle_connections = 32;
  out->statement_cache = 64;
}

/* ---- the executor's state --------------------------------------------------- */

struct XConn;
struct XTenant;

typedef struct XStmt {
  sqlite3_stmt * st;
  struct XConn * conn;
  char * sql;        /* the cache key */
  size_t len;
  int in_use;
  int cached;        /* this use came from the cache */
  int kept;          /* it is in the connection's cache */
  uint64_t rows;
  uint64_t used;     /* for evicting the least recently used */
} XStmt;

typedef struct XConn {
  sqlite3 * db;
  struct XTenant * t;
  unsigned id;
  int write;
  int in_use;
  uint64_t idle_since;
  XStmt ** cache;
  size_t ncache;
} XConn;

typedef struct XTenant {
  char * name;
  size_t len;
  char * path;
  XConn * writer;    /* idle or in use; NULL when none is open */
  XConn ** idle;     /* idle readers */
  size_t nidle;
  size_t nbusy;      /* readers in use */
  struct XTenant * next;
} XTenant;

typedef struct Ex {
  const GDEFSSQLITE_Allocator * alloc;
  char * dir;
  int keys;
  GDEFSSQLITE_ModelOptions opts;
  GDEFSSQL_Dialect d;
  GDEFSSQL_Executor self;
  XTenant * tenants;
  size_t idle_total;
  unsigned next_id;
  uint64_t tick;
} Ex;

static GDEFSTOR_Result from_sqlite(int rc) {
  switch (rc & 0xff) {
  case SQLITE_OK:
  case SQLITE_DONE:
  case SQLITE_ROW:
    return GDEFSTOR_OK;
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    return GDEFSTOR_ERR_BUSY;
  case SQLITE_NOMEM:
    return GDEFSTOR_ERR_OOM;
  case SQLITE_CORRUPT:
  case SQLITE_NOTADB:
    return GDEFSTOR_ERR_CORRUPT;
  case SQLITE_TOOBIG:
    return GDEFSTOR_ERR_LIMIT;
  default:
    return GDEFSTOR_ERR_IO;
  }
}

static GDEFSSQLITE_Result to_lib(GDEFSTOR_Result rc) {
  switch (rc) {
  case GDEFSTOR_OK:
    return GDEFSSQLITE_OK;
  case GDEFSTOR_ERR_IO:
    return GDEFSSQLITE_ERR_IO;
  case GDEFSTOR_ERR_FORMAT:
    return GDEFSSQLITE_ERR_FORMAT;
  case GDEFSTOR_ERR_UNSUPPORTED:
    return GDEFSSQLITE_ERR_UNSUPPORTED;
  case GDEFSTOR_ERR_LIMIT:
    return GDEFSSQLITE_ERR_LIMIT;
  case GDEFSTOR_ERR_CORRUPT:
    return GDEFSSQLITE_ERR_CORRUPT;
  case GDEFSTOR_ERR_OOM:
    return GDEFSSQLITE_ERR_OOM;
  case GDEFSTOR_ERR_INVALID:
    return GDEFSSQLITE_ERR_INVALID;
  case GDEFSTOR_ERR_BUSY:
    return GDEFSSQLITE_ERR_BUSY;
  default:
    return GDEFSSQLITE_ERR_INTERNAL;
  }
}

static GDEFSSQLITE_Result from_sql(GDEFSSQL_Result rc) {
  return to_lib(gdefssql_result_to_port(rc));
}

static char * dup_bytes(const GDEFSSQLITE_Allocator * a, const char * p,
    size_t n) {
  char * c = (char *)gcu_allocator_malloc(a, n + 1);
  if (c == NULL) {
    return NULL;
  }
  if (n > 0) {
    memcpy(c, p, n);
  }
  c[n] = '\0';
  return c;
}

/* <dir>/<hex of the tenant><suffix>: never the tenant's bytes as a path. */
static char * tenant_path(const Ex * ex, const char * tenant, size_t len) {
  static const char hex[] = "0123456789abcdef";
  const char * suffix = ex->keys ? ".keys.db" : ".db";
  size_t dir_len = strlen(ex->dir);
  size_t suf_len = strlen(suffix);
  int slash = dir_len == 0 || ex->dir[dir_len - 1] != '/';
  size_t total;
  size_t i;
  char * path;
  char * p;
  if (len > (SIZE_MAX - dir_len - suf_len - 2) / 2) {
    return NULL;
  }
  total = dir_len + (size_t)slash + len * 2 + suf_len + 1;
  path = (char *)gcu_allocator_malloc(ex->alloc, total);
  if (path == NULL) {
    return NULL;
  }
  memcpy(path, ex->dir, dir_len);
  p = path + dir_len;
  if (slash) {
    *p++ = '/';
  }
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)tenant[i];
    *p++ = hex[c >> 4];
    *p++ = hex[c & 0x0fu];
  }
  memcpy(p, suffix, suf_len + 1);
  return path;
}

static int path_missing(const char * path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return 0;
  }
  return errno == ENOENT;
}

/* ---- refusals ----------------------------------------------------------------- */

static void refuse(Ex * ex, GDEFSTOR_Result rc, const char * path,
    const char * cause, const char * detail) {
  char line[1024];
  int n = snprintf(line, sizeof line,
      "defiant-storage-sqlite: %s %s%s; it is refused and left as it is",
      path, cause, detail != NULL ? detail : "");
  size_t len;
  if (n < 0) {
    return;
  }
  len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
  if (ex->opts.on_refusal != NULL) {
    ex->opts.on_refusal(ex->opts.on_refusal_ctx, (int)rc, line, len);
  } else {
    fprintf(stderr, "%s\n", line);
  }
}

/* ---- statements ------------------------------------------------------------- */

static void report(Ex * ex, XConn * c, const char * sql, size_t len,
    sqlite3_stmt * st, int exec, int cached, uint64_t rows) {
  GDEFSSQLITE_StatementStats s;
  if (ex->opts.on_statement == NULL) {
    return;
  }
  memset(&s, 0, sizeof s);
  s.size = sizeof s;
  s.sql = sql;
  s.sql_len = len;
  s.connection = c->id;
  s.write = c->write;
  s.keys = ex->keys;
  s.exec = exec;
  s.cached = cached;
  s.rows = rows;
  if (!exec && st != NULL) {
    s.fullscan_steps =
        (uint64_t)sqlite3_stmt_status(st, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0);
    s.vm_steps = (uint64_t)sqlite3_stmt_status(st, SQLITE_STMTSTATUS_VM_STEP, 0);
    s.sorts = (uint64_t)sqlite3_stmt_status(st, SQLITE_STMTSTATUS_SORT, 0);
    s.autoindexes =
        (uint64_t)sqlite3_stmt_status(st, SQLITE_STMTSTATUS_AUTOINDEX, 0);
  }
  ex->opts.on_statement(ex->opts.on_statement_ctx, &s);
}

static void stmt_free(Ex * ex, XStmt * s) {
  sqlite3_finalize(s->st);
  gcu_allocator_free(ex->alloc, s->sql);
  gcu_allocator_free(ex->alloc, s);
}

/* Run one statement that takes no parameter and returns no row: exactly one,
 * the whole text. */
static int run_plain(sqlite3 * db, const char * sql, size_t len) {
  sqlite3_stmt * st = NULL;
  const char * tail = NULL;
  int rc;
  if (len > INT_MAX) {
    return SQLITE_TOOBIG;
  }
  rc = sqlite3_prepare_v2(db, sql, (int)len, &st, &tail);
  if (rc != SQLITE_OK) {
    return rc;
  }
  if (st == NULL || tail != sql + len) {
    sqlite3_finalize(st);
    return SQLITE_MISUSE;
  }
  do {
    rc = sqlite3_step(st);
  } while (rc == SQLITE_ROW);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

/* A pragma that answers one text, e.g. "wal". *out is "" for no row. */
static int pragma_text(sqlite3 * db, const char * sql, char * out,
    size_t cap) {
  sqlite3_stmt * st = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
  out[0] = '\0';
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    const unsigned char * t = sqlite3_column_text(st, 0);
    if (t != NULL) {
      snprintf(out, cap, "%s", (const char *)t);
    }
    while (rc == SQLITE_ROW) {
      rc = sqlite3_step(st);
    }
  }
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

/* ---- connections ------------------------------------------------------------ */

static void conn_close(Ex * ex, XConn * c) {
  size_t i;
  for (i = 0; i < c->ncache; i++) {
    stmt_free(ex, c->cache[i]);
  }
  gcu_allocator_free(ex->alloc, c->cache);
  sqlite3_close_v2(c->db);
  gcu_allocator_free(ex->alloc, c);
}

/* What the catalog of a file says it is. */
enum { FILE_EMPTY, FILE_OURS, FILE_REFUSED };

typedef struct Catalog {
  int tables;            /* not counting SQLite's own */
  int data;              /* dss_schema */
  int keys;              /* dsk_schema */
  char first[64];        /* the first table that is none of these */
} Catalog;

static int read_catalog(sqlite3 * db, Catalog * cat) {
  sqlite3_stmt * st = NULL;
  int rc;
  memset(cat, 0, sizeof *cat);
  /* A pragma, not a query: it reads the schema and nothing else, takes only
   * a shared lock and writes nothing. */
  rc = sqlite3_prepare_v2(db, "PRAGMA main.table_list", -1, &st, NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const char * name = (const char *)sqlite3_column_text(st, 1);
    if (name == NULL || strncmp(name, "sqlite_", 7) == 0) {
      continue;
    }
    cat->tables += 1;
    if (strcmp(name, "dss_schema") == 0) {
      cat->data = 1;
    } else if (strcmp(name, "dsk_schema") == 0) {
      cat->keys = 1;
    } else if (cat->first[0] == '\0' && strncmp(name, "dss_", 4) != 0 &&
        strncmp(name, "dsk_", 4) != 0 && name[0] != 't') {
      snprintf(cat->first, sizeof cat->first, "%s", name);
    }
  }
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

/* Decide whether this connection's file may be used, before anything writes
 * to it. Reports and returns the refusal. */
static GDEFSTOR_Result classify(Ex * ex, XConn * c, int * out_state) {
  Catalog cat;
  int64_t version = 0;
  int64_t want = ex->keys ? GDEFSSQL_KEYSTORE_VERSION : GDEFSSQL_SCHEMA_VERSION;
  GDEFSTOR_Result rc;
  int src = read_catalog(c->db, &cat);
  *out_state = FILE_REFUSED;
  if (src != SQLITE_OK) {
    return from_sqlite(src);
  }
  if (!ex->keys && cat.keys) {
    refuse(ex, GDEFSTOR_ERR_FORMAT, c->t->path,
        "is a key store (it has dsk_schema), not a data store", NULL);
    return GDEFSTOR_ERR_FORMAT;
  }
  if (ex->keys && cat.data) {
    refuse(ex, GDEFSTOR_ERR_FORMAT, c->t->path,
        "is a data store, not a key store", NULL);
    return GDEFSTOR_ERR_FORMAT;
  }
  if (!(ex->keys ? cat.keys : cat.data)) {
    if (cat.tables == 0) {
      *out_state = FILE_EMPTY;
      return GDEFSTOR_OK;
    }
    refuse(ex, GDEFSTOR_ERR_FORMAT, c->t->path,
        ex->keys ? "holds tables but no dsk_schema"
                 : "holds tables but no dss_schema",
        NULL);
    return GDEFSTOR_ERR_FORMAT;
  }
  rc = gdefssql_store_version(&ex->d, &ex->self, c, NULL,
      ex->keys ? GDEFSSQL_STORE_KEYS : GDEFSSQL_STORE_DATA, &version);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (version != want) {
    char detail[96];
    GDEFSTOR_Result why =
        version > want ? GDEFSTOR_ERR_UNSUPPORTED : GDEFSTOR_ERR_FORMAT;
    snprintf(detail, sizeof detail, " %lld; this build reads version %lld",
        (long long)version, (long long)want);
    refuse(ex, why, c->t->path, "records layout version", detail);
    return why;
  }
  *out_state = FILE_OURS;
  return GDEFSTOR_OK;
}

/* The pragmas every accepted connection runs, in this order: nothing before
 * classify() may write, and journal_mode=WAL writes the file header. */
static GDEFSTOR_Result setup(Ex * ex, XConn * c) {
  char got[32];
  int rc = pragma_text(c->db, "PRAGMA journal_mode=WAL", got, sizeof got);
  if (rc != SQLITE_OK) {
    return from_sqlite(rc);
  }
  if (strcmp(got, "wal") != 0) {
    return GDEFSTOR_ERR_IO;
  }
  /* Read back, as secure_delete is: SQLite ignores a pragma it cannot apply,
   * so a store that asked for FULL would otherwise run at whatever it got. */
  if (ex->opts.synchronous == GDEFSSQLITE_SYNC_OFF) {
    rc = run_plain(c->db, "PRAGMA synchronous=OFF", 22);
  } else {
    rc = run_plain(c->db, "PRAGMA synchronous=FULL", 23);
  }
  if (rc == SQLITE_OK) {
    rc = pragma_text(c->db, "PRAGMA synchronous", got, sizeof got);
    if (rc == SQLITE_OK &&
        strcmp(got, ex->opts.synchronous == GDEFSSQLITE_SYNC_OFF ? "0" : "2") !=
            0) {
      return GDEFSTOR_ERR_IO;
    }
  }
  if (rc == SQLITE_OK && ex->keys) {
    /* A destroyed key's bytes are overwritten, not left in a free page. */
    rc = pragma_text(c->db, "PRAGMA secure_delete=ON", got, sizeof got);
    if (rc == SQLITE_OK && strcmp(got, "1") != 0) {
      return GDEFSTOR_ERR_IO;
    }
  }
  if (rc == SQLITE_OK && !c->write) {
    rc = run_plain(c->db, "PRAGMA query_only=ON", 20);
  }
  return from_sqlite(rc);
}

/* A new connection to @p t's file. *out NULL, with OK, when a read finds no
 * store: the file is missing or holds nothing. */
static GDEFSTOR_Result conn_open(Ex * ex, XTenant * t, int write,
    XConn ** out) {
  XConn * c;
  sqlite3 * db = NULL;
  int flags = SQLITE_OPEN_READWRITE;
  int state = FILE_REFUSED;
  int rc;
  GDEFSTOR_Result r;
  *out = NULL;
  if (!write && path_missing(t->path)) {
    return GDEFSTOR_OK;
  }
  if (write) {
    flags |= SQLITE_OPEN_CREATE;
  }
  rc = sqlite3_open_v2(t->path, &db, flags, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close_v2(db);
    if (!write && rc == SQLITE_CANTOPEN && path_missing(t->path)) {
      return GDEFSTOR_OK;
    }
    return from_sqlite(rc);
  }
  sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, (int *)0);
  sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, (int *)0);
  sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, (int *)0);
  sqlite3_busy_timeout(db, ex->opts.busy_timeout_ms > INT_MAX
                               ? INT_MAX
                               : (int)ex->opts.busy_timeout_ms);
  c = (XConn *)gcu_allocator_calloc(ex->alloc, 1, sizeof *c);
  if (c == NULL) {
    sqlite3_close_v2(db);
    return GDEFSTOR_ERR_OOM;
  }
  c->db = db;
  c->t = t;
  c->write = write;
  c->id = ++ex->next_id;
  r = classify(ex, c, &state);
  if (r == GDEFSTOR_OK && state == FILE_EMPTY && !write) {
    conn_close(ex, c);
    return GDEFSTOR_OK; /* a file with nothing in it holds nothing */
  }
  if (r == GDEFSTOR_OK) {
    r = setup(ex, c);
  }
  if (r != GDEFSTOR_OK) {
    conn_close(ex, c);
    return r;
  }
  *out = c;
  return GDEFSTOR_OK;
}

/* ---- tenants and the pool ---------------------------------------------------- */

static XTenant * tenant_get(Ex * ex, const char * name, size_t len,
    GDEFSTOR_Result * rc) {
  XTenant * t;
  for (t = ex->tenants; t != NULL; t = t->next) {
    if (t->len == len && memcmp(t->name, name, len) == 0) {
      return t;
    }
  }
  t = (XTenant *)gcu_allocator_calloc(ex->alloc, 1, sizeof *t);
  if (t == NULL) {
    *rc = GDEFSTOR_ERR_OOM;
    return NULL;
  }
  t->name = dup_bytes(ex->alloc, name, len);
  t->path = t->name == NULL ? NULL : tenant_path(ex, name, len);
  if (ex->opts.read_connections > 0 && t->path != NULL) {
    t->idle = (XConn **)gcu_allocator_calloc(ex->alloc,
        ex->opts.read_connections, sizeof *t->idle);
  }
  if (t->name == NULL || t->path == NULL ||
      (ex->opts.read_connections > 0 && t->idle == NULL)) {
    gcu_allocator_free(ex->alloc, t->idle);
    gcu_allocator_free(ex->alloc, t->path);
    gcu_allocator_free(ex->alloc, t->name);
    gcu_allocator_free(ex->alloc, t);
    *rc = GDEFSTOR_ERR_OOM;
    return NULL;
  }
  t->len = len;
  t->next = ex->tenants;
  ex->tenants = t;
  return t;
}

/* Forget a tenant that has no connection at all. */
static void tenant_gc(Ex * ex, XTenant * t) {
  XTenant ** pp;
  if (t->writer != NULL || t->nidle != 0 || t->nbusy != 0) {
    return;
  }
  for (pp = &ex->tenants; *pp != NULL; pp = &(*pp)->next) {
    if (*pp == t) {
      *pp = t->next;
      break;
    }
  }
  gcu_allocator_free(ex->alloc, t->idle);
  gcu_allocator_free(ex->alloc, t->path);
  gcu_allocator_free(ex->alloc, t->name);
  gcu_allocator_free(ex->alloc, t);
}

/* Close idle connections, least recently used first, until at most the
 * store's cap are open and idle. */
static void trim(Ex * ex) {
  while (ex->idle_total > ex->opts.idle_connections) {
    XTenant * t;
    XTenant * best_t = NULL;
    size_t best_i = 0;
    int best_writer = 0;
    uint64_t best = UINT64_MAX;
    for (t = ex->tenants; t != NULL; t = t->next) {
      size_t i;
      if (t->writer != NULL && !t->writer->in_use &&
          t->writer->idle_since < best) {
        best = t->writer->idle_since;
        best_t = t;
        best_writer = 1;
      }
      for (i = 0; i < t->nidle; i++) {
        if (t->idle[i]->idle_since < best) {
          best = t->idle[i]->idle_since;
          best_t = t;
          best_i = i;
          best_writer = 0;
        }
      }
    }
    if (best_t == NULL) {
      return;
    }
    if (best_writer) {
      conn_close(ex, best_t->writer);
      best_t->writer = NULL;
    } else {
      conn_close(ex, best_t->idle[best_i]);
      best_t->idle[best_i] = best_t->idle[best_t->nidle - 1];
      best_t->nidle -= 1;
    }
    ex->idle_total -= 1;
    tenant_gc(ex, best_t);
  }
}

static GDEFSTOR_Result x_open(void * ctx, const char * tenant,
    size_t tenant_len, int write, void ** out_conn, const char ** out_qual) {
  Ex * ex = (Ex *)ctx;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  XTenant * t;
  XConn * c = NULL;
  *out_conn = NULL;
  *out_qual = NULL;
  if (tenant == NULL || tenant_len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  t = tenant_get(ex, tenant, tenant_len, &rc);
  if (t == NULL) {
    return rc;
  }
  if (write) {
    if (t->writer != NULL) {
      if (t->writer->in_use) {
        return GDEFSTOR_ERR_INTERNAL; /* one write per tenant at a time */
      }
      c = t->writer;
      ex->idle_total -= 1;
    } else {
      rc = conn_open(ex, t, 1, &c);
      if (rc == GDEFSTOR_OK) {
        t->writer = c;
      }
    }
  } else if (t->nidle > 0) {
    /* A read has a connection of its own: never the writer's, which may be
     * in a write that is not committed. */
    t->nidle -= 1;
    c = t->idle[t->nidle];
    ex->idle_total -= 1;
  } else {
    rc = conn_open(ex, t, 0, &c);
  }
  if (rc != GDEFSTOR_OK || c == NULL) {
    tenant_gc(ex, t);
    return rc;
  }
  c->in_use = 1;
  if (!c->write) {
    t->nbusy += 1;
  }
  *out_conn = c;
  return GDEFSTOR_OK;
}

static void x_close(void * ctx, void * conn, int discard) {
  Ex * ex = (Ex *)ctx;
  XConn * c = (XConn *)conn;
  XTenant * t;
  if (c == NULL) {
    return;
  }
  t = c->t;
  c->in_use = 0;
  if (!c->write) {
    t->nbusy -= 1;
  }
  /* A connection left inside a transaction is not trusted again either. */
  if (!discard && !sqlite3_get_autocommit(c->db)) {
    discard = 1;
  }
  if (discard) {
    if (c->write) {
      t->writer = NULL;
    }
    conn_close(ex, c);
  } else if (c->write) {
    c->idle_since = ++ex->tick;
    ex->idle_total += 1;
  } else if (t->nidle < ex->opts.read_connections) {
    c->idle_since = ++ex->tick;
    t->idle[t->nidle++] = c;
    ex->idle_total += 1;
  } else {
    conn_close(ex, c);
  }
  tenant_gc(ex, t);
  trim(ex);
}

static GDEFSTOR_Result x_exec(void * ctx, void * conn, const char * sql,
    size_t sql_len) {
  Ex * ex = (Ex *)ctx;
  XConn * c = (XConn *)conn;
  int rc = run_plain(c->db, sql, sql_len);
  report(ex, c, sql, sql_len, NULL, 1, 0, 0);
  if (rc == SQLITE_OK && ex->keys && c->write &&
      sql_len == sizeof kCommit - 1 && memcmp(sql, kCommit, sql_len) == 0) {
    /* What the commit superseded (a destroyed key's page) is still in the
     * WAL: copy the WAL into the file and empty it. A reader in the way
     * leaves it for the next write's checkpoint. */
    sqlite3_wal_checkpoint_v2(c->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL,
        NULL);
  }
  return from_sqlite(rc);
}

static GDEFSTOR_Result x_prepare(void * ctx, void * conn, const char * sql,
    size_t sql_len, void ** out_stmt) {
  Ex * ex = (Ex *)ctx;
  XConn * c = (XConn *)conn;
  XStmt * s = NULL;
  const char * tail = NULL;
  size_t i;
  int rc;
  *out_stmt = NULL;
  if (sql_len > INT_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  for (i = 0; i < c->ncache; i++) {
    XStmt * k = c->cache[i];
    if (!k->in_use && k->len == sql_len && memcmp(k->sql, sql, sql_len) == 0) {
      s = k;
      s->cached = 1;
      break;
    }
  }
  if (s == NULL) {
    sqlite3_stmt * st = NULL;
    s = (XStmt *)gcu_allocator_calloc(ex->alloc, 1, sizeof *s);
    if (s == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->sql = dup_bytes(ex->alloc, sql, sql_len);
    if (s->sql == NULL) {
      gcu_allocator_free(ex->alloc, s);
      return GDEFSTOR_ERR_OOM;
    }
    s->len = sql_len;
    s->conn = c;
    rc = sqlite3_prepare_v3(c->db, sql, (int)sql_len,
        ex->opts.statement_cache > 0 ? SQLITE_PREPARE_PERSISTENT : 0, &st,
        &tail);
    if (rc != SQLITE_OK || st == NULL || tail != sql + sql_len) {
      sqlite3_finalize(st);
      gcu_allocator_free(ex->alloc, s->sql);
      gcu_allocator_free(ex->alloc, s);
      return rc != SQLITE_OK ? from_sqlite(rc) : GDEFSTOR_ERR_INTERNAL;
    }
    s->st = st;
    if (ex->opts.statement_cache > 0) {
      if (c->ncache == ex->opts.statement_cache) {
        /* Full: drop the least recently used idle one. */
        size_t lru = SIZE_MAX;
        for (i = 0; i < c->ncache; i++) {
          if (!c->cache[i]->in_use &&
              (lru == SIZE_MAX || c->cache[i]->used < c->cache[lru]->used)) {
            lru = i;
          }
        }
        if (lru != SIZE_MAX) {
          stmt_free(ex, c->cache[lru]);
          c->cache[lru] = c->cache[c->ncache - 1];
          c->ncache -= 1;
        }
      }
      if (c->cache == NULL) {
        c->cache = (XStmt **)gcu_allocator_calloc(ex->alloc,
            ex->opts.statement_cache, sizeof *c->cache);
      }
      if (c->cache != NULL && c->ncache < ex->opts.statement_cache) {
        c->cache[c->ncache++] = s;
        s->kept = 1;
      }
      /* Not kept (no room for the array): it is finalized after its one use. */
    }
  }
  s->in_use = 1;
  s->rows = 0;
  sqlite3_stmt_status(s->st, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
  sqlite3_stmt_status(s->st, SQLITE_STMTSTATUS_VM_STEP, 1);
  sqlite3_stmt_status(s->st, SQLITE_STMTSTATUS_SORT, 1);
  sqlite3_stmt_status(s->st, SQLITE_STMTSTATUS_AUTOINDEX, 1);
  *out_stmt = s;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result x_bind(void * ctx, void * stmt, unsigned index,
    const GDEFSSQL_Datum * v) {
  XStmt * s = (XStmt *)stmt;
  int rc;
  (void)ctx;
  if (index == 0 || index > INT_MAX || v == NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  /* The SQL layer keeps the bytes alive until it finalizes the statement,
   * and finalize clears the bindings, so nothing here is copied. */
  switch (v->kind) {
  case GDEFSSQL_DATUM_INT:
    rc = sqlite3_bind_int64(s->st, (int)index, (sqlite3_int64)v->integer);
    break;
  case GDEFSSQL_DATUM_REAL:
    rc = sqlite3_bind_double(s->st, (int)index, v->real);
    break;
  case GDEFSSQL_DATUM_TEXT:
    rc = sqlite3_bind_text64(s->st, (int)index,
        v->len == 0 ? "" : (const char *)v->bytes, (sqlite3_uint64)v->len,
        SQLITE_STATIC, SQLITE_UTF8);
    break;
  case GDEFSSQL_DATUM_BYTES:
    /* A NULL pointer would bind SQL NULL: an empty value is "" of length 0. */
    rc = sqlite3_bind_blob64(s->st, (int)index,
        v->len == 0 ? (const void *)"" : (const void *)v->bytes,
        (sqlite3_uint64)v->len, SQLITE_STATIC);
    break;
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
  return from_sqlite(rc);
}

static GDEFSTOR_Result x_step(void * ctx, void * stmt, int * out_row) {
  XStmt * s = (XStmt *)stmt;
  int rc;
  (void)ctx;
  *out_row = 0;
  rc = sqlite3_step(s->st);
  if (rc == SQLITE_ROW) {
    s->rows += 1;
    *out_row = 1;
    return GDEFSTOR_OK;
  }
  return rc == SQLITE_DONE ? GDEFSTOR_OK : from_sqlite(rc);
}

static GDEFSTOR_Result x_column(void * ctx, void * stmt, unsigned index,
    int kind, GDEFSSQL_Datum * out) {
  XStmt * s = (XStmt *)stmt;
  int type;
  (void)ctx;
  memset(out, 0, sizeof *out);
  if (index >= (unsigned)sqlite3_column_count(s->st)) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  type = sqlite3_column_type(s->st, (int)index);
  if (type == SQLITE_NULL) {
    out->kind = GDEFSSQL_DATUM_NULL;
    return GDEFSTOR_OK;
  }
  switch (kind) {
  case GDEFSSQL_DATUM_INT:
    if (type != SQLITE_INTEGER) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    out->integer = (int64_t)sqlite3_column_int64(s->st, (int)index);
    break;
  case GDEFSSQL_DATUM_REAL:
    /* A REAL column may keep an integral value as an integer on disk. */
    if (type == SQLITE_INTEGER) {
      out->real = (double)sqlite3_column_int64(s->st, (int)index);
    } else if (type == SQLITE_FLOAT) {
      out->real = sqlite3_column_double(s->st, (int)index);
    } else {
      return GDEFSTOR_ERR_CORRUPT;
    }
    break;
  case GDEFSSQL_DATUM_TEXT:
    if (type != SQLITE_TEXT) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    out->bytes = sqlite3_column_text(s->st, (int)index);
    out->len = (size_t)sqlite3_column_bytes(s->st, (int)index);
    if (out->bytes == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    break;
  case GDEFSSQL_DATUM_BYTES:
    if (type != SQLITE_BLOB) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    out->bytes = (const uint8_t *)sqlite3_column_blob(s->st, (int)index);
    out->len = (size_t)sqlite3_column_bytes(s->st, (int)index);
    if (out->bytes == NULL && out->len != 0) {
      return GDEFSTOR_ERR_OOM;
    }
    break;
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
  out->kind = kind;
  return GDEFSTOR_OK;
}

static void x_finalize(void * ctx, void * stmt) {
  Ex * ex = (Ex *)ctx;
  XStmt * s = (XStmt *)stmt;
  if (s == NULL) {
    return;
  }
  report(ex, s->conn, s->sql, s->len, s->st, 0, s->cached, s->rows);
  s->in_use = 0;
  s->cached = 0;
  if (s->kept) {
    sqlite3_reset(s->st);
    sqlite3_clear_bindings(s->st);
    s->used = ++ex->tick;
  } else {
    stmt_free(ex, s);
  }
}

static void ex_destroy(void * ctx) {
  Ex * ex = (Ex *)ctx;
  if (ex == NULL) {
    return;
  }
  while (ex->tenants != NULL) {
    XTenant * t = ex->tenants;
    size_t i;
    ex->tenants = t->next;
    for (i = 0; i < t->nidle; i++) {
      conn_close(ex, t->idle[i]);
    }
    if (t->writer != NULL) {
      conn_close(ex, t->writer);
    }
    gcu_allocator_free(ex->alloc, t->idle);
    gcu_allocator_free(ex->alloc, t->path);
    gcu_allocator_free(ex->alloc, t->name);
    gcu_allocator_free(ex->alloc, t);
  }
  gcu_allocator_free(ex->alloc, ex->dir);
  gcu_allocator_free(ex->alloc, ex);
}

static GDEFSSQLITE_Result ex_create(const char * directory, int keys,
    const GDEFSSQLITE_ModelOptions * options,
    const GDEFSSQLITE_Allocator * allocator, Ex ** out) {
  const GDEFSSQLITE_Allocator * alloc =
      allocator != NULL ? allocator : gdefssqlite_allocator_default();
  Ex * ex;
  *out = NULL;
  if (directory == NULL || directory[0] == '\0' ||
      (options != NULL && options->size != sizeof *options) ||
      (options != NULL && options->synchronous != GDEFSSQLITE_SYNC_FULL &&
          options->synchronous != GDEFSSQLITE_SYNC_OFF)) {
    return GDEFSSQLITE_ERR_INVALID;
  }
  ex = (Ex *)gcu_allocator_calloc(alloc, 1, sizeof *ex);
  if (ex == NULL) {
    return GDEFSSQLITE_ERR_OOM;
  }
  ex->alloc = alloc;
  ex->keys = keys;
  ex->dir = dup_bytes(alloc, directory, strlen(directory));
  if (ex->dir == NULL) {
    gcu_allocator_free(alloc, ex);
    return GDEFSSQLITE_ERR_OOM;
  }
  if (options != NULL) {
    ex->opts = *options;
  } else {
    gdefssqlite_model_options_default(&ex->opts);
  }
  fill_dialect(&ex->d);
  memset(&ex->self, 0, sizeof ex->self);
  ex->self.size = sizeof ex->self;
  ex->self.ctx = ex;
  ex->self.destroy = ex_destroy;
  ex->self.open = x_open;
  ex->self.close = x_close;
  ex->self.exec = x_exec;
  ex->self.prepare = x_prepare;
  ex->self.bind = x_bind;
  ex->self.step = x_step;
  ex->self.column = x_column;
  ex->self.finalize = x_finalize;
  *out = ex;
  return GDEFSSQLITE_OK;
}

/* ---- the stores --------------------------------------------------------------- */

GDEFSSQLITE_Result gdefssqlite_model_backend(const char * directory,
    const GDEFSSQLITE_ModelOptions * options,
    const GDEFSSQLITE_Allocator * allocator, GDEFSTOR_ModelBackend * out) {
  Ex * ex = NULL;
  GDEFSSQLITE_Result rc;
  if (out == NULL) {
    return GDEFSSQLITE_ERR_INVALID;
  }
  rc = ex_create(directory, 0, options, allocator, &ex);
  if (rc != GDEFSSQLITE_OK) {
    return rc;
  }
  rc = from_sql(gdefssql_model_backend(&ex->d, &ex->self, ex->alloc, out));
  if (rc != GDEFSSQLITE_OK) {
    ex_destroy(ex);
  }
  return rc;
}

GDEFSSQLITE_Result gdefssqlite_model_store_create(const char * directory,
    const GDEFSSQLITE_ModelOptions * options,
    const GDEFSSQLITE_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store) {
  const GDEFSSQLITE_Allocator * alloc =
      allocator != NULL ? allocator : gdefssqlite_allocator_default();
  GDEFSTOR_ModelBackend model;
  GDEFSSQLITE_Result rc;
  GDEFSTOR_Result prc;
  if (out_store == NULL) {
    return GDEFSSQLITE_ERR_INVALID;
  }
  rc = gdefssqlite_model_backend(directory, options, alloc, &model);
  if (rc != GDEFSSQLITE_OK) {
    return rc;
  }
  prc = gdefstor_store_open(&model, alloc, limits, out_store);
  if (prc != GDEFSTOR_OK) {
    model.destroy(model.ctx);
  }
  return to_lib(prc);
}

GDEFSSQLITE_Result gdefssqlite_keystore_create(const char * directory,
    const GDEFSTOR_KeyCipher * cipher, const GDEFSSQLITE_ModelOptions * options,
    const GDEFSSQLITE_Allocator * allocator, GDEFSTOR_KeyStore ** out_keystore) {
  Ex * ex = NULL;
  GDEFSTOR_KeyStoreBackend kb;
  GDEFSSQLITE_Result rc;
  GDEFSTOR_Result prc;
  if (out_keystore == NULL || cipher == NULL) {
    return GDEFSSQLITE_ERR_INVALID;
  }
  rc = ex_create(directory, 1, options, allocator, &ex);
  if (rc != GDEFSSQLITE_OK) {
    return rc;
  }
  rc = from_sql(gdefssql_keystore_backend(&ex->d, &ex->self, cipher, ex->alloc,
      &kb));
  if (rc != GDEFSSQLITE_OK) {
    ex_destroy(ex);
    return rc;
  }
  prc = gdefstor_keystore_create(&kb, ex->alloc, out_keystore);
  if (prc != GDEFSTOR_OK) {
    kb.destroy(kb.ctx);
  }
  return to_lib(prc);
}
