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
 * One SQLite file per tenant. The port owns the writer queue. A key arrives
 * here only after commit, and each put or remove is its own transaction.
 * Schema steps are numbered and forward-only. There is no down step.
 */

#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <ghoti.io/defiant-storage/query.h>
#include <ghoti.io/cutil/allocator.h>
#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/**
 * Planted by tools/check-defects.sh. When set, the step to version 1
 * deletes the rows it just copied, so Migration.OldRowSurvives fails.
 */
static int gdefssqlite_plant_drop_row = 0; /* plant:migration */

typedef struct SqlBag {
  const GDEFSSQLITE_Allocator * alloc;
  char * dir;
  sqlite3 * txn_db;
  char * txn_tenant;
  size_t txn_tenant_len;
  int puts_in_txn;
} SqlBag;

/* When positive, the Nth put inside a transaction returns an I/O error
 * before it writes. Tests set this. It is not a planted defect. */
int gdefssqlite_plant_fail_put = 0;

/* Queries answered by one SELECT, and queries the SELECT could not carry and
 * the port scanned instead. Tests read them; they are not an interface. */
int gdefssqlite_query_native = 0;
int gdefssqlite_query_fallback = 0;

static GDEFSTOR_Result from_sqlite(int rc) {
  switch (rc) {
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
  default:
    return GDEFSTOR_ERR_IO;
  }
}

static GDEFSSQLITE_Result from_port(GDEFSTOR_Result rc) {
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
  case GDEFSTOR_ERR_INTERNAL:
    return GDEFSSQLITE_ERR_INTERNAL;
  case GDEFSTOR_ERR_BUSY:
    return GDEFSSQLITE_ERR_BUSY;
  case GDEFSTOR_RESULT_COUNT:
  default:
    return GDEFSSQLITE_ERR_INTERNAL;
  }
}

static char * dup_str(const GDEFSSQLITE_Allocator * alloc, const char * s) {
  size_t n = strlen(s);
  char * out = (char *)gcu_allocator_malloc(alloc, n + 1);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, s, n + 1);
  return out;
}

static int hex_file(const char * tenant, size_t tenant_len, char * out,
    size_t cap) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  if (tenant_len > (SIZE_MAX - 4) / 2) {
    return -1;
  }
  if (cap < tenant_len * 2 + 4) {
    return -1;
  }
  for (i = 0; i < tenant_len; i++) {
    unsigned char c = (unsigned char)tenant[i];
    out[i * 2] = hex[c >> 4];
    out[i * 2 + 1] = hex[c & 0x0fu];
  }
  memcpy(out + tenant_len * 2, ".db", 4);
  return 0;
}

static char * tenant_path(const SqlBag * bag, const char * tenant,
    size_t tenant_len) {
  size_t dir_len;
  int slash;
  size_t name_len;
  size_t total;
  char * path;
  if (bag == NULL || bag->dir == NULL || tenant == NULL) {
    return NULL;
  }
  if (tenant_len > (SIZE_MAX - 4) / 2) {
    return NULL;
  }
  dir_len = strlen(bag->dir);
  slash = (dir_len == 0 || bag->dir[dir_len - 1] != '/');
  name_len = tenant_len * 2 + 3;
  if (dir_len > SIZE_MAX - (size_t)slash - name_len - 1) {
    return NULL;
  }
  total = dir_len + (size_t)slash + name_len + 1;
  path = (char *)gcu_allocator_malloc(bag->alloc, total);
  if (path == NULL) {
    return NULL;
  }
  memcpy(path, bag->dir, dir_len);
  if (slash) {
    path[dir_len] = '/';
  }
  if (hex_file(tenant, tenant_len, path + dir_len + (size_t)slash,
          name_len + 1) != 0) {
    gcu_allocator_free(bag->alloc, path);
    return NULL;
  }
  return path;
}

static int table_exists(sqlite3 * db, const char * name, int * out) {
  sqlite3_stmt * st = NULL;
  int rc;
  *out = 0;
  rc = sqlite3_prepare_v2(db,
      "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", -1,
      &st, NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(st);
    return rc;
  }
  rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    *out = 1;
    rc = SQLITE_OK;
  } else if (rc == SQLITE_DONE) {
    rc = SQLITE_OK;
  }
  sqlite3_finalize(st);
  return rc;
}

static int schema_version(sqlite3 * db, int * out) {
  sqlite3_stmt * st = NULL;
  int exists = 0;
  int rc = table_exists(db, "defiant_migration", &exists);
  *out = 0;
  if (rc != SQLITE_OK) {
    return rc;
  }
  if (!exists) {
    return SQLITE_OK;
  }
  rc = sqlite3_prepare_v2(db,
      "SELECT COALESCE(MAX(version), 0) FROM defiant_migration", -1, &st,
      NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    *out = sqlite3_column_int(st, 0);
    rc = SQLITE_OK;
  }
  sqlite3_finalize(st);
  return rc;
}

static GDEFSTOR_Result migrate(sqlite3 * db) {
  int version = 0;
  int has_v0 = 0;
  int rc;
  char * err = NULL;
  char sql[1024];
  int n;

  rc = schema_version(db, &version);
  if (rc != SQLITE_OK) {
    return from_sqlite(rc);
  }
  if (version >= 1) {
    return GDEFSTOR_OK;
  }
  rc = table_exists(db, "record_v0", &has_v0);
  if (rc != SQLITE_OK) {
    return from_sqlite(rc);
  }
  n = snprintf(sql, sizeof sql,
      "BEGIN IMMEDIATE;"
      "CREATE TABLE IF NOT EXISTS defiant_record ("
      " key TEXT PRIMARY KEY, bytes BLOB NOT NULL);"
      "%s"
      "%s"
      "DROP TABLE IF EXISTS record_v0;"
      "CREATE TABLE IF NOT EXISTS defiant_migration ("
      " version INTEGER PRIMARY KEY);"
      "INSERT INTO defiant_migration (version) VALUES (1);"
      "COMMIT;",
      has_v0 ? "INSERT INTO defiant_record (key, bytes) "
               "SELECT key, bytes FROM record_v0;"
             : "",
      gdefssqlite_plant_drop_row ? "DELETE FROM defiant_record;" : "");
  if (n < 0 || (size_t)n >= sizeof sql) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(err);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return from_sqlite(rc);
  }
  return GDEFSTOR_OK;
}

static int register_query_functions(sqlite3 * db, SqlBag * bag);

static int path_missing(const char * path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return 0;
  }
  return errno == ENOENT;
}

static GDEFSTOR_Result open_tenant(SqlBag * bag, const char * tenant,
    size_t tenant_len, int create, sqlite3 ** out) {
  char * path;
  sqlite3 * db = NULL;
  int flags;
  int missing;
  int rc;
  GDEFSTOR_Result mapped;
  *out = NULL;
  if (bag == NULL || tenant == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  path = tenant_path(bag, tenant, tenant_len);
  if (path == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  flags = SQLITE_OPEN_READWRITE;
  if (create) {
    flags |= SQLITE_OPEN_CREATE;
  }
  missing = path_missing(path);
  rc = sqlite3_open_v2(path, &db, flags, NULL);
  gcu_allocator_free(bag->alloc, path);
  if (rc != SQLITE_OK) {
    if (db != NULL) {
      sqlite3_close(db);
    }
    if (!create && missing && rc == SQLITE_CANTOPEN) {
      return GDEFSTOR_ERR_INVALID;
    }
    return from_sqlite(rc);
  }
  sqlite3_busy_timeout(db, 0);
  sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, (int *)0);
  sqlite3_enable_load_extension(db, 0);
  rc = register_query_functions(db, bag);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return from_sqlite(rc);
  }
  mapped = migrate(db);
  if (mapped != GDEFSTOR_OK) {
    sqlite3_close(db);
    return mapped;
  }
  *out = db;
  return GDEFSTOR_OK;
}

static void clear_txn(SqlBag * bag) {
  if (bag->txn_db != NULL) {
    sqlite3_exec(bag->txn_db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(bag->txn_db);
    bag->txn_db = NULL;
  }
  gcu_allocator_free(bag->alloc, bag->txn_tenant);
  bag->txn_tenant = NULL;
  bag->txn_tenant_len = 0;
  bag->puts_in_txn = 0;
}

static void bag_destroy(void * ctx) {
  SqlBag * bag = (SqlBag *)ctx;
  if (bag == NULL) {
    return;
  }
  clear_txn(bag);
  gcu_allocator_free(bag->alloc, bag->dir);
  gcu_allocator_free(bag->alloc, bag);
}

static int exec_begin(sqlite3 * db) {
  return sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
}

static int same_tenant(const SqlBag * bag, const char * tenant,
    size_t tenant_len) {
  return bag->txn_db != NULL && bag->txn_tenant != NULL &&
      bag->txn_tenant_len == tenant_len &&
      (tenant_len == 0 || memcmp(bag->txn_tenant, tenant, tenant_len) == 0);
}

static void close_db(SqlBag * bag, sqlite3 * db) {
  if (db != NULL && db != bag->txn_db) {
    sqlite3_close(db);
  }
}

static void rollback_db(SqlBag * bag, sqlite3 * db) {
  if (db != NULL && db != bag->txn_db) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  }
}

static GDEFSTOR_Result connect(SqlBag * bag, const char * tenant,
    size_t tenant_len, int create, sqlite3 ** out) {
  if (bag->txn_db != NULL) {
    if (!same_tenant(bag, tenant, tenant_len)) {
      return GDEFSTOR_ERR_BUSY;
    }
    *out = bag->txn_db;
    return GDEFSTOR_OK;
  }
  return open_tenant(bag, tenant, tenant_len, create, out);
}

static GDEFSTOR_Result bag_put(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len, const uint8_t * bytes,
    size_t len) {
  SqlBag * bag = (SqlBag *)ctx;
  sqlite3 * db = NULL;
  sqlite3_stmt * st = NULL;
  GDEFSTOR_Result mapped;
  int rc;
  if (bag == NULL || tenant == NULL || key == NULL ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (len > INT_MAX || key_len > INT_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  mapped = connect(bag, tenant, tenant_len, 1, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  if (db == bag->txn_db && gdefssqlite_plant_fail_put > 0) {
    bag->puts_in_txn += 1;
    if (bag->puts_in_txn >= gdefssqlite_plant_fail_put) {
      return GDEFSTOR_ERR_IO;
    }
  }
  rc = db == bag->txn_db ? SQLITE_OK : exec_begin(db);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_prepare_v2(db,
      "INSERT INTO defiant_record (key, bytes) VALUES (?, ?) "
      "ON CONFLICT(key) DO UPDATE SET bytes = excluded.bytes",
      -1, &st, NULL);
  if (rc != SQLITE_OK) {
    rollback_db(bag, db);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_blob(st, 2, len == 0 ? "" : (const void *)bytes,
        (int)len, SQLITE_STATIC);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_step(st);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    rollback_db(bag, db);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = db == bag->txn_db ? SQLITE_OK
                        : sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    rollback_db(bag, db);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  close_db(bag, db);
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_get(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len, uint8_t ** out_bytes,
    size_t * out_len) {
  SqlBag * bag = (SqlBag *)ctx;
  sqlite3 * db = NULL;
  sqlite3_stmt * st = NULL;
  GDEFSTOR_Result mapped;
  int rc;
  const void * blob;
  int kind;
  int n;
  uint8_t * copy;
  if (bag == NULL || tenant == NULL || key == NULL || out_bytes == NULL ||
      out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (key_len > INT_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  mapped = connect(bag, tenant, tenant_len, 0, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = sqlite3_prepare_v2(db,
      "SELECT bytes FROM defiant_record WHERE key = ?", -1, &st, NULL);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(st);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_step(st);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(st);
    close_db(bag, db);
    return GDEFSTOR_ERR_INVALID;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  kind = sqlite3_column_type(st, 0);
  blob = sqlite3_column_blob(st, 0);
  n = sqlite3_column_bytes(st, 0);
  if (sqlite3_errcode(db) == SQLITE_NOMEM) {
    sqlite3_finalize(st);
    close_db(bag, db);
    return GDEFSTOR_ERR_OOM;
  }
  if (kind == SQLITE_NULL || n < 0 || (n > 0 && blob == NULL)) {
    sqlite3_finalize(st);
    close_db(bag, db);
    return GDEFSTOR_ERR_CORRUPT;
  }
  copy = (uint8_t *)gcu_allocator_malloc(bag->alloc, n == 0 ? 1 : (size_t)n);
  if (copy == NULL) {
    sqlite3_finalize(st);
    close_db(bag, db);
    return GDEFSTOR_ERR_OOM;
  }
  if (n > 0) {
    memcpy(copy, blob, (size_t)n);
  }
  sqlite3_finalize(st);
  close_db(bag, db);
  *out_bytes = copy;
  *out_len = (size_t)n;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_remove(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len) {
  SqlBag * bag = (SqlBag *)ctx;
  sqlite3 * db = NULL;
  sqlite3_stmt * st = NULL;
  GDEFSTOR_Result mapped;
  int rc;
  if (bag == NULL || tenant == NULL || key == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (key_len > INT_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  mapped = connect(bag, tenant, tenant_len, 0, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = db == bag->txn_db ? SQLITE_OK : exec_begin(db);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_prepare_v2(db, "DELETE FROM defiant_record WHERE key = ?", -1,
      &st, NULL);
  if (rc != SQLITE_OK) {
    rollback_db(bag, db);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
  if (rc == SQLITE_OK) {
    rc = sqlite3_step(st);
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    rollback_db(bag, db);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  if (sqlite3_changes(db) == 0) {
    rollback_db(bag, db);
    close_db(bag, db);
    return GDEFSTOR_ERR_INVALID;
  }
  rc = db == bag->txn_db ? SQLITE_OK
                        : sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    rollback_db(bag, db);
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  close_db(bag, db);
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_list(void * ctx, const char * tenant,
    size_t tenant_len, const char * prefix, size_t prefix_len,
    char *** out_keys, size_t ** out_lens, size_t * out_count) {
  SqlBag * bag = (SqlBag *)ctx;
  sqlite3 * db = NULL;
  sqlite3_stmt * st = NULL;
  GDEFSTOR_Result mapped;
  int rc;
  size_t count = 0;
  size_t i = 0;
  char ** keys = NULL;
  size_t * lens = NULL;
  if (bag == NULL || tenant == NULL || out_keys == NULL || out_lens == NULL ||
      out_count == NULL || (prefix == NULL && prefix_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  mapped = connect(bag, tenant, tenant_len, 0, &db);
  if (mapped == GDEFSTOR_ERR_INVALID) {
    *out_keys = NULL;
    *out_lens = NULL;
    *out_count = 0;
    return GDEFSTOR_OK;
  }
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = db == bag->txn_db ? SQLITE_OK
                        : sqlite3_exec(db, "BEGIN DEFERRED", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_prepare_v2(db, "SELECT key FROM defiant_record ORDER BY key",
      -1, &st, NULL);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    rollback_db(bag, db);
    close_db(bag, db);
    return mapped;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    int n = sqlite3_column_bytes(st, 0);
    const unsigned char * text;
    if (n < 0) {
      sqlite3_finalize(st);
      rollback_db(bag, db);
      close_db(bag, db);
      return GDEFSTOR_ERR_CORRUPT;
    }
    text = sqlite3_column_text(st, 0);
    if ((size_t)n >= prefix_len &&
        (prefix_len == 0 ||
            memcmp(text, prefix, prefix_len) == 0)) {
      count += 1;
    }
  }
  if (rc != SQLITE_DONE) {
    sqlite3_finalize(st);
    mapped = from_sqlite(rc);
    rollback_db(bag, db);
    close_db(bag, db);
    return mapped;
  }
  rc = sqlite3_reset(st);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(st);
    mapped = from_sqlite(rc);
    rollback_db(bag, db);
    close_db(bag, db);
    return mapped;
  }
  if (count == 0) {
    sqlite3_finalize(st);
    rc = db == bag->txn_db ? SQLITE_OK
                          : sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
      rollback_db(bag, db);
      close_db(bag, db);
      return from_sqlite(rc);
    }
    close_db(bag, db);
    *out_keys = NULL;
    *out_lens = NULL;
    *out_count = 0;
    return GDEFSTOR_OK;
  }
  keys = (char **)gcu_allocator_calloc(bag->alloc, count, sizeof *keys);
  lens = (size_t *)gcu_allocator_calloc(bag->alloc, count, sizeof *lens);
  if (keys == NULL || lens == NULL) {
    gcu_allocator_free(bag->alloc, keys);
    gcu_allocator_free(bag->alloc, lens);
    sqlite3_finalize(st);
    rollback_db(bag, db);
    close_db(bag, db);
    return GDEFSTOR_ERR_OOM;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    int n = sqlite3_column_bytes(st, 0);
    const unsigned char * text = sqlite3_column_text(st, 0);
    char * copy;
    size_t j;
    if ((size_t)n < prefix_len ||
        (prefix_len != 0 && memcmp(text, prefix, prefix_len) != 0)) {
      continue;
    }
    if (i >= count) {
      for (j = 0; j < i; j++) {
        gcu_allocator_free(bag->alloc, keys[j]);
      }
      gcu_allocator_free(bag->alloc, keys);
      gcu_allocator_free(bag->alloc, lens);
      sqlite3_finalize(st);
      rollback_db(bag, db);
      close_db(bag, db);
      return GDEFSTOR_ERR_INTERNAL;
    }
    copy = (char *)gcu_allocator_malloc(bag->alloc, (size_t)n + 1);
    if (copy == NULL) {
      for (j = 0; j < i; j++) {
        gcu_allocator_free(bag->alloc, keys[j]);
      }
      gcu_allocator_free(bag->alloc, keys);
      gcu_allocator_free(bag->alloc, lens);
      sqlite3_finalize(st);
      rollback_db(bag, db);
      close_db(bag, db);
      return GDEFSTOR_ERR_OOM;
    }
    if (n > 0 && text != NULL) {
      memcpy(copy, text, (size_t)n);
    }
    copy[n] = '\0';
    keys[i] = copy;
    lens[i] = (size_t)n;
    i += 1;
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    size_t j;
    for (j = 0; j < i; j++) {
      gcu_allocator_free(bag->alloc, keys[j]);
    }
    gcu_allocator_free(bag->alloc, keys);
    gcu_allocator_free(bag->alloc, lens);
    rollback_db(bag, db);
    close_db(bag, db);
    return from_sqlite(rc);
  }
  rc = db == bag->txn_db ? SQLITE_OK
                        : sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    size_t j;
    for (j = 0; j < i; j++) {
      gcu_allocator_free(bag->alloc, keys[j]);
    }
    gcu_allocator_free(bag->alloc, keys);
    gcu_allocator_free(bag->alloc, lens);
    rollback_db(bag, db);
    close_db(bag, db);
    return from_sqlite(rc);
  }
  close_db(bag, db);
  *out_keys = keys;
  *out_lens = lens;
  *out_count = i;
  return GDEFSTOR_OK;
}


/* ---- queries ------------------------------------------------------------- */

/*
 * A query (query.h) is compiled into one SELECT over defiant_record and run.
 * The statement is built from the fixed vocabulary below and `?`. Every
 * value reaches it as a bound parameter: the key range of the type, the
 * constants of a comparison, the key of a joined row, the page. Nothing from
 * the query is ever written into the text of the statement, and column and
 * type names are never in it at all: the leaves are evaluated by two functions
 * registered on the connection, gdq_leaf and gdq_key, which read the encoded
 * row the way the port's scan does and are handed the node (as a pointer
 * parameter) that says which column.
 *
 * Pushed down: the whole predicate tree (and, or, not, every comparison,
 * constants, the join to another projection's row by primary key), the sort and
 * its tiebreak, the offset and the limit, and the total. The key range of the
 * type is an index range scan on the primary key. Not pushed down: any use of
 * an index on a column. A row is stored as one encoded value, so every row of
 * the type is read and its predicate evaluated; the cost is linear in the rows
 * of the type, as the scan's is, but without copying every row out.
 *
 * A tree that needs more bound values or more function arguments than the
 * connection allows is not compiled: the backend answers ::GDEFSTOR_ERR_UNSUPPORTED
 * and the port scans.
 */

#define QUERY_POINTER_TYPE "gdefstor_query"

typedef struct SqlBuf {
  const GDEFSSQLITE_Allocator * alloc;
  char * p;
  size_t n;
  size_t cap;
  int oom;
} SqlBuf;

static void sb_add(SqlBuf * b, const char * s, size_t n) {
  if (b->oom || n == 0) {
    return;
  }
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap == 0 ? 256 : b->cap;
    char * grown;
    while (cap < b->n + n + 1) {
      cap *= 2;
    }
    grown = (char *)gcu_allocator_realloc(b->alloc, b->p, cap);
    if (grown == NULL) {
      b->oom = 1;
      return;
    }
    b->p = grown;
    b->cap = cap;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = '\0';
}

static void sb_str(SqlBuf * b, const char * s) {
  sb_add(b, s, strlen(s));
}

enum { kBindPointer = 1, kBindBlob, kBindText, kBindInt };

typedef struct Bind {
  int kind;
  const void * ptr;
  size_t len;
  sqlite3_int64 value;
} Bind;

typedef struct Qb {
  const GDEFSSQLITE_Allocator * alloc;
  SqlBuf sql;
  Bind * binds;
  size_t nbinds;
  size_t cap;
  char ** owned;
  size_t nowned;
  size_t owned_cap;
  size_t prefix_len;       /* of the type being queried: the outer row's */
  int in_exists;           /* inside an EXISTS: rows are t, the outer is r */
  size_t exists_prefix_len;/* of the type an EXISTS looks at */
  int max_vars;
  int max_args;
  int unsupported;
  int oom;
} Qb;

static void qb_bind(Qb * q, int kind, const void * ptr, size_t len,
    sqlite3_int64 value) {
  if (q->oom) {
    return;
  }
  if (q->nbinds == q->cap) {
    size_t cap = q->cap == 0 ? 16 : q->cap * 2;
    Bind * grown = (Bind *)gcu_allocator_realloc(q->alloc, q->binds,
        cap * sizeof *grown);
    if (grown == NULL) {
      q->oom = 1;
      return;
    }
    q->binds = grown;
    q->cap = cap;
  }
  q->binds[q->nbinds].kind = kind;
  q->binds[q->nbinds].ptr = ptr;
  q->binds[q->nbinds].len = len;
  q->binds[q->nbinds].value = value;
  q->nbinds += 1;
  sb_str(&q->sql, "?");
  if (q->nbinds > (size_t)q->max_vars) {
    q->unsupported = 1;
  }
}

/* A string the statement's parameters point into, kept until it is done. */
static char * qb_keep(Qb * q, char * s) {
  if (q->nowned == q->owned_cap) {
    size_t cap = q->owned_cap == 0 ? 8 : q->owned_cap * 2;
    char ** grown = (char **)gcu_allocator_realloc(q->alloc, q->owned,
        cap * sizeof *grown);
    if (grown == NULL) {
      gcu_allocator_free(q->alloc, s);
      q->oom = 1;
      return NULL;
    }
    q->owned = grown;
    q->owned_cap = cap;
  }
  q->owned[q->nowned++] = s;
  return s;
}

static void qb_free(Qb * q) {
  size_t i;
  for (i = 0; i < q->nowned; i++) {
    gcu_allocator_free(q->alloc, q->owned[i]);
  }
  gcu_allocator_free(q->alloc, q->owned);
  gcu_allocator_free(q->alloc, q->binds);
  gcu_allocator_free(q->alloc, q->sql.p);
}

/* The row of another projection type with the outer row's entity, by its
 * primary key: the join type's key prefix followed by the entity's hex, which
 * is what the outer key has after its own prefix. */
static void emit_join(Qb * q, const GDEFSTOR_QueryOperand * o) {
  char * prefix = NULL;
  size_t prefix_len = 0;
  /* A joined row is the one of the other type with the entity of the row the
   * operand is of: the outer row (alias r) for an OUTER operand, else the row
   * the statement is looking at (r, or t inside an EXISTS). */
  int outer = (o->flags & GDEFSTOR_QUERY_OUTER) != 0;
  const char * alias = (outer || !q->in_exists) ? "r" : "t";
  size_t base_prefix = (outer || !q->in_exists) ? q->prefix_len : q->exists_prefix_len;
  if (o->kind != GDEFSTOR_QUERY_COLUMN || o->join_type == NULL) {
    sb_str(&q->sql, "NULL");
    return;
  }
  if (gdefstor_projection_key_prefix(q->alloc, o->join_type, o->join_type_len,
          &prefix, &prefix_len) != GDEFSTOR_OK) {
    q->oom = 1;
    return;
  }
  if (qb_keep(q, prefix) == NULL) {
    return;
  }
  sb_str(&q->sql, "(SELECT j.bytes FROM defiant_record j WHERE j.key = ");
  qb_bind(q, kBindText, prefix, prefix_len, 0);
  sb_str(&q->sql, " || substr(");
  sb_str(&q->sql, alias);
  sb_str(&q->sql, ".key, ");
  qb_bind(q, kBindInt, NULL, 0, (sqlite3_int64)base_prefix + 1);
  sb_str(&q->sql, "))");
}

static void emit_texts(Qb * q, const GDEFSTOR_QueryOperand * o) {
  size_t i;
  if (o->kind != GDEFSTOR_QUERY_CONST) {
    return;
  }
  for (i = 0; i < o->text_count; i++) {
    sb_str(&q->sql, ", ");
    qb_bind(q, kBindBlob, o->texts[i].bytes, o->texts[i].len, 0);
  }
}

static void emit_node(Qb * q, const GDEFSTOR_QueryNode * n);
static char * hi_bound(const GDEFSSQLITE_Allocator * alloc, const char * prefix,
    size_t prefix_len);

/* A balanced chain, so the depth of the expression the parser sees is the
 * logarithm of the number of children and not the number. */
static void emit_chain(Qb * q, const GDEFSTOR_QueryNode * n, size_t lo,
    size_t hi) {
  if (hi - lo == 1) {
    emit_node(q, n->children[lo]);
    return;
  }
  {
    size_t mid = lo + (hi - lo) / 2;
    sb_str(&q->sql, "(");
    emit_chain(q, n, lo, mid);
    sb_str(&q->sql, n->op == GDEFSTOR_QUERY_AND ? " AND " : " OR ");
    emit_chain(q, n, mid, hi);
    sb_str(&q->sql, ")");
  }
}

static void emit_node(Qb * q, const GDEFSTOR_QueryNode * n) {
  size_t nargs;
  if (q->oom || q->unsupported) {
    return;
  }
  switch (n->op) {
    case GDEFSTOR_QUERY_TRUE:
      sb_str(&q->sql, "1");
      return;
    case GDEFSTOR_QUERY_FALSE:
      sb_str(&q->sql, "0");
      return;
    case GDEFSTOR_QUERY_AND:
      if (n->child_count == 0) {
        sb_str(&q->sql, "1");
      } else {
        emit_chain(q, n, 0, n->child_count);
      }
      return;
    case GDEFSTOR_QUERY_OR:
      if (n->child_count == 0) {
        sb_str(&q->sql, "0");
      } else {
        emit_chain(q, n, 0, n->child_count);
      }
      return;
    case GDEFSTOR_QUERY_NOT:
      sb_str(&q->sql, "(NOT ");
      emit_node(q, n->children[0]);
      sb_str(&q->sql, ")");
      return;
    case GDEFSTOR_QUERY_EXISTS: {
      /* A semi-join: some row of the other type, by its key range, for which
       * the child holds, with the row being decided as the outer one. */
      char * prefix = NULL;
      size_t prefix_len = 0;
      char * hi;
      if (gdefstor_projection_key_prefix(q->alloc, n->exists_type,
              n->exists_type_len, &prefix, &prefix_len) != GDEFSTOR_OK) {
        q->oom = 1;
        return;
      }
      if (qb_keep(q, prefix) == NULL) {
        return;
      }
      hi = hi_bound(q->alloc, prefix, prefix_len);
      if (hi == NULL || qb_keep(q, hi) == NULL) {
        q->oom = 1;
        return;
      }
      sb_str(&q->sql, "EXISTS (SELECT 1 FROM defiant_record t WHERE t.key >= ");
      qb_bind(q, kBindText, prefix, prefix_len, 0);
      sb_str(&q->sql, " AND t.key < ");
      qb_bind(q, kBindText, hi, prefix_len, 0);
      sb_str(&q->sql, " AND (");
      q->in_exists = 1;
      q->exists_prefix_len = prefix_len;
      emit_node(q, n->children[0]);
      q->in_exists = 0;
      sb_str(&q->sql, "))");
      return;
    }
    default:
      break;
  }
  nargs = 7 + (n->left.kind == GDEFSTOR_QUERY_CONST ? n->left.text_count : 0) +
          (n->right.kind == GDEFSTOR_QUERY_CONST ? n->right.text_count : 0);
  if (nargs > (size_t)q->max_args) {
    q->unsupported = 1;
    return;
  }
  sb_str(&q->sql, "gdq_leaf(");
  qb_bind(q, kBindPointer, n, 0, 0);
  sb_str(&q->sql, q->in_exists ? ", t.bytes, t.key, r.bytes, r.key, "
                               : ", r.bytes, r.key, NULL, NULL, ");
  emit_join(q, &n->left);
  sb_str(&q->sql, ", ");
  emit_join(q, &n->right);
  emit_texts(q, &n->left);
  emit_texts(q, &n->right);
  sb_str(&q->sql, ")");
}

static void emit_range(Qb * q, const char * lo, size_t lo_len, const char * hi,
    size_t hi_len) {
  sb_str(&q->sql, " WHERE r.key >= ");
  qb_bind(q, kBindText, lo, lo_len, 0);
  sb_str(&q->sql, " AND r.key < ");
  qb_bind(q, kBindText, hi, hi_len, 0);
  sb_str(&q->sql, " AND (");
}

static int apply_binds(sqlite3_stmt * st, const Bind * binds, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    int idx = (int)i + 1;
    int rc;
    switch (binds[i].kind) {
      case kBindPointer:
        rc = sqlite3_bind_pointer(st, idx, (void *)binds[i].ptr,
            QUERY_POINTER_TYPE, NULL);
        break;
      case kBindBlob:
        rc = sqlite3_bind_blob(st, idx,
            binds[i].len == 0 ? (const void *)"" : binds[i].ptr,
            (int)binds[i].len, SQLITE_STATIC);
        break;
      case kBindText:
        rc = sqlite3_bind_text(st, idx, (const char *)binds[i].ptr,
            (int)binds[i].len, SQLITE_STATIC);
        break;
      default:
        rc = sqlite3_bind_int64(st, idx, binds[i].value);
        break;
    }
    if (rc != SQLITE_OK) {
      return rc;
    }
  }
  return SQLITE_OK;
}

/* The entity in a key: the hex after the last colon. */
static GDEFSTOR_Result key_entity(const GDEFSSQLITE_Allocator * alloc,
    const char * key, size_t key_len, char ** out, size_t * out_len) {
  size_t at = key_len;
  while (at > 0 && key[at - 1] != ':') {
    at -= 1;
  }
  if (at == 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  return gdefstor_projection_key_entity(alloc, key, key_len, at, out, out_len);
}

static void udf_fail(sqlite3_context * c, GDEFSTOR_Result rc) {
  if (rc == GDEFSTOR_ERR_OOM) {
    sqlite3_result_error_nomem(c);
  } else if (rc == GDEFSTOR_ERR_CORRUPT) {
    sqlite3_result_error(c, "corrupt row", -1);
    sqlite3_result_error_code(c, SQLITE_CORRUPT);
  } else {
    sqlite3_result_error(c, "query function failed", -1);
    sqlite3_result_error_code(c, SQLITE_ERROR);
  }
}

static GDEFSTOR_QueryRowRef value_row(sqlite3_value * v) {
  GDEFSTOR_QueryRowRef ref;
  ref.bytes = NULL;
  ref.len = 0;
  if (sqlite3_value_type(v) == SQLITE_BLOB) {
    ref.bytes = (const uint8_t *)sqlite3_value_blob(v);
    ref.len = (size_t)sqlite3_value_bytes(v);
    if (ref.bytes == NULL) {
      ref.bytes = (const uint8_t *)"";
    }
  }
  return ref;
}

/* gdq_leaf(node, row, key, outer_row, outer_key, left_join, right_join,
 * constants...) */
static void udf_leaf(sqlite3_context * c, int argc, sqlite3_value ** argv) {
  SqlBag * bag = (SqlBag *)sqlite3_user_data(c);
  const GDEFSTOR_QueryNode * node;
  GDEFSTOR_QueryNode copy;
  GDEFSTOR_QueryText * texts = NULL;
  GDEFSTOR_QueryEnv env;
  size_t nl;
  size_t nr;
  size_t i;
  char * entity = NULL;
  size_t entity_len = 0;
  char * outer_entity = NULL;
  size_t outer_entity_len = 0;
  int holds = 0;
  GDEFSTOR_Result rc;
  node = (const GDEFSTOR_QueryNode *)sqlite3_value_pointer(argv[0],
      QUERY_POINTER_TYPE);
  if (node == NULL || argc < 7) {
    udf_fail(c, GDEFSTOR_ERR_INTERNAL);
    return;
  }
  copy = *node;
  nl = node->left.kind == GDEFSTOR_QUERY_CONST ? node->left.text_count : 0;
  nr = node->right.kind == GDEFSTOR_QUERY_CONST ? node->right.text_count : 0;
  if ((size_t)argc != 7 + nl + nr) {
    udf_fail(c, GDEFSTOR_ERR_INTERNAL);
    return;
  }
  /* The constants are the bound values, not the node's copy of them. */
  if (nl + nr > 0) {
    texts = (GDEFSTOR_QueryText *)gcu_allocator_calloc(bag->alloc, nl + nr,
        sizeof *texts);
    if (texts == NULL) {
      sqlite3_result_error_nomem(c);
      return;
    }
    for (i = 0; i < nl + nr; i++) {
      sqlite3_value * v = argv[7 + i];
      const void * b = sqlite3_value_blob(v);
      texts[i].bytes = b == NULL ? (const uint8_t *)"" : (const uint8_t *)b;
      texts[i].len = (size_t)sqlite3_value_bytes(v);
    }
    if (nl > 0) {
      copy.left.texts = texts;
    }
    if (nr > 0) {
      copy.right.texts = texts + nl;
    }
  }
  memset(&env, 0, sizeof env);
  env.main = value_row(argv[1]);
  env.outer = value_row(argv[3]);
  env.left_join = value_row(argv[5]);
  env.right_join = value_row(argv[6]);
  rc = key_entity(bag->alloc, (const char *)sqlite3_value_text(argv[2]),
      (size_t)sqlite3_value_bytes(argv[2]), &entity, &entity_len);
  if (rc == GDEFSTOR_OK && env.outer.bytes != NULL) {
    rc = key_entity(bag->alloc, (const char *)sqlite3_value_text(argv[4]),
        (size_t)sqlite3_value_bytes(argv[4]), &outer_entity, &outer_entity_len);
  }
  if (rc == GDEFSTOR_OK) {
    env.entity = (const uint8_t *)entity;
    env.entity_len = entity_len;
    env.outer_entity = (const uint8_t *)outer_entity;
    env.outer_entity_len = outer_entity_len;
    rc = gdefstor_query_leaf(bag->alloc, &copy, &env, &holds);
  }
  gcu_allocator_free(bag->alloc, outer_entity);
  gcu_allocator_free(bag->alloc, entity);
  gcu_allocator_free(bag->alloc, texts);
  if (rc != GDEFSTOR_OK) {
    udf_fail(c, rc);
    return;
  }
  sqlite3_result_int(c, holds ? 1 : 0);
}

/* gdq_key(sort, row, key): NULL, an integer or a blob, for ORDER BY. */
static void udf_key(sqlite3_context * c, int argc, sqlite3_value ** argv) {
  SqlBag * bag = (SqlBag *)sqlite3_user_data(c);
  const GDEFSTOR_QuerySort * sort;
  GDEFSTOR_QueryEnv env;
  GDEFSTOR_QueryKey key;
  char * entity = NULL;
  size_t entity_len = 0;
  GDEFSTOR_Result rc;
  sort = (const GDEFSTOR_QuerySort *)sqlite3_value_pointer(argv[0],
      QUERY_POINTER_TYPE);
  if (sort == NULL || argc != 3) {
    udf_fail(c, GDEFSTOR_ERR_INTERNAL);
    return;
  }
  memset(&env, 0, sizeof env);
  env.main = value_row(argv[1]);
  rc = key_entity(bag->alloc, (const char *)sqlite3_value_text(argv[2]),
      (size_t)sqlite3_value_bytes(argv[2]), &entity, &entity_len);
  if (rc != GDEFSTOR_OK) {
    udf_fail(c, rc);
    return;
  }
  env.entity = (const uint8_t *)entity;
  env.entity_len = entity_len;
  rc = gdefstor_query_key_make(bag->alloc, sort, &env, &key);
  gcu_allocator_free(bag->alloc, entity);
  if (rc != GDEFSTOR_OK) {
    udf_fail(c, rc);
    return;
  }
  if (!key.present) {
    sqlite3_result_null(c);
  } else if (key.is_int) {
    sqlite3_result_int64(c, (sqlite3_int64)key.value);
  } else {
    sqlite3_result_blob(c, key.len == 0 ? (const void *)"" : key.bytes,
        (int)key.len, SQLITE_TRANSIENT);
  }
  gdefstor_query_key_free(bag->alloc, &key);
}

static int register_query_functions(sqlite3 * db, SqlBag * bag) {
  int rc = sqlite3_create_function(db, "gdq_leaf", -1, SQLITE_UTF8, bag,
      udf_leaf, NULL, NULL);
  if (rc == SQLITE_OK) {
    rc = sqlite3_create_function(db, "gdq_key", 3, SQLITE_UTF8, bag, udf_key,
        NULL, NULL);
  }
  return rc;
}

static char * hi_bound(const GDEFSSQLITE_Allocator * alloc, const char * prefix,
    size_t prefix_len) {
  /* Every key of the type begins with the prefix, which ends in a colon; the
   * first string past them all is the prefix with that colon made a semicolon. */
  char * hi = (char *)gcu_allocator_malloc(alloc, prefix_len + 1);
  if (hi == NULL) {
    return NULL;
  }
  memcpy(hi, prefix, prefix_len);
  hi[prefix_len - 1] = (char)(prefix[prefix_len - 1] + 1);
  hi[prefix_len] = '\0';
  return hi;
}

static void free_rows(const GDEFSSQLITE_Allocator * alloc,
    GDEFSTOR_QueryRows * rows) {
  size_t i;
  for (i = 0; i < rows->count; i++) {
    gcu_allocator_free(alloc, rows->entities[i]);
    gcu_allocator_free(alloc, rows->rows[i]);
  }
  gcu_allocator_free(alloc, rows->entities);
  gcu_allocator_free(alloc, rows->entity_lens);
  gcu_allocator_free(alloc, rows->rows);
  gcu_allocator_free(alloc, rows->row_lens);
  memset(rows, 0, sizeof *rows);
}

static GDEFSTOR_Result add_row(const GDEFSSQLITE_Allocator * alloc,
    GDEFSTOR_QueryRows * rows, size_t * cap, const char * key, size_t key_len,
    size_t prefix_len, const void * bytes, size_t len) {
  char * entity = NULL;
  size_t entity_len = 0;
  uint8_t * copy;
  GDEFSTOR_Result rc;
  if (rows->count == *cap) {
    size_t grown = *cap == 0 ? 32 : *cap * 2;
    char ** e = (char **)gcu_allocator_realloc(alloc, rows->entities,
        grown * sizeof *e);
    size_t * el;
    uint8_t ** rw;
    size_t * rl;
    if (e == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    rows->entities = e;
    el = (size_t *)gcu_allocator_realloc(alloc, rows->entity_lens,
        grown * sizeof *el);
    if (el == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    rows->entity_lens = el;
    rw = (uint8_t **)gcu_allocator_realloc(alloc, rows->rows, grown * sizeof *rw);
    if (rw == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    rows->rows = rw;
    rl = (size_t *)gcu_allocator_realloc(alloc, rows->row_lens,
        grown * sizeof *rl);
    if (rl == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    rows->row_lens = rl;
    *cap = grown;
  }
  rc = gdefstor_projection_key_entity(alloc, key, key_len, prefix_len, &entity,
      &entity_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  copy = (uint8_t *)gcu_allocator_malloc(alloc, len == 0 ? 1 : len);
  if (copy == NULL) {
    gcu_allocator_free(alloc, entity);
    return GDEFSTOR_ERR_OOM;
  }
  if (len > 0) {
    memcpy(copy, bytes, len);
  }
  rows->entities[rows->count] = entity;
  rows->entity_lens[rows->count] = entity_len;
  rows->rows[rows->count] = copy;
  rows->row_lens[rows->count] = len;
  rows->count += 1;
  return GDEFSTOR_OK;
}

/* Prepare and run a statement built in @p q; @p collect is called for each
 * row. */
typedef struct RunCtx {
  const GDEFSSQLITE_Allocator * alloc;
  GDEFSTOR_QueryRows * rows;
  size_t rows_cap;
  size_t prefix_len;
  sqlite3_int64 count;
  int want_rows;
} RunCtx;

static GDEFSTOR_Result run_statement(sqlite3 * db, Qb * q, RunCtx * rc_ctx) {
  sqlite3_stmt * st = NULL;
  int rc;
  GDEFSTOR_Result mapped = GDEFSTOR_OK;
  if (q->sql.oom || q->oom) {
    return GDEFSTOR_ERR_OOM;
  }
  rc = sqlite3_prepare_v2(db, q->sql.p, (int)q->sql.n, &st, NULL);
  if (rc != SQLITE_OK) {
    return from_sqlite(rc);
  }
  rc = apply_binds(st, q->binds, q->nbinds);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(st);
    return from_sqlite(rc);
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (rc_ctx->want_rows) {
      const char * key = (const char *)sqlite3_column_text(st, 0);
      int key_len = sqlite3_column_bytes(st, 0);
      const void * bytes = sqlite3_column_blob(st, 1);
      int len = sqlite3_column_bytes(st, 1);
      if (key == NULL || key_len < 0 || len < 0) {
        mapped = GDEFSTOR_ERR_CORRUPT;
        break;
      }
      mapped = add_row(rc_ctx->alloc, rc_ctx->rows, &rc_ctx->rows_cap, key,
          (size_t)key_len, rc_ctx->prefix_len, bytes, (size_t)len);
      if (mapped != GDEFSTOR_OK) {
        break;
      }
    } else {
      rc_ctx->count = sqlite3_column_int64(st, 0);
    }
  }
  if (mapped == GDEFSTOR_OK && rc != SQLITE_DONE) {
    mapped = from_sqlite(rc);
    if (mapped == GDEFSTOR_OK) {
      mapped = GDEFSTOR_ERR_IO;
    }
  }
  sqlite3_finalize(st);
  return mapped;
}

static GDEFSTOR_Result bag_query(void * ctx, const char * tenant,
    size_t tenant_len, const GDEFSTOR_QueryPlan * plan,
    GDEFSTOR_QueryRows * out) {
  SqlBag * bag = (SqlBag *)ctx;
  sqlite3 * db = NULL;
  GDEFSTOR_Result mapped;
  GDEFSTOR_Result result = GDEFSTOR_OK;
  const GDEFSTOR_Query * query;
  Qb q;
  RunCtx rctx;
  char * hi = NULL;
  GDEFSTOR_QuerySort tie[2];
  size_t ntie = 0;
  size_t i;
  int rc;
  if (bag == NULL || tenant == NULL || plan == NULL || out == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  query = plan->query;
  memset(out, 0, sizeof *out);
  mapped = connect(bag, tenant, tenant_len, 0, &db);
  if (mapped == GDEFSTOR_ERR_INVALID) {
    return GDEFSTOR_OK; /* no such tenant: no rows */
  }
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = db == bag->txn_db ? SQLITE_OK
                        : sqlite3_exec(db, "BEGIN DEFERRED", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    close_db(bag, db);
    return mapped;
  }
  hi = hi_bound(bag->alloc, plan->prefix, plan->prefix_len);
  if (hi == NULL) {
    rollback_db(bag, db);
    close_db(bag, db);
    return GDEFSTOR_ERR_OOM;
  }
  memset(&rctx, 0, sizeof rctx);
  rctx.alloc = bag->alloc;
  rctx.rows = out;
  rctx.prefix_len = plan->prefix_len;

  /* The total: every match, before the offset and the limit. */
  memset(&q, 0, sizeof q);
  q.alloc = bag->alloc;
  q.sql.alloc = bag->alloc;
  q.prefix_len = plan->prefix_len;
  q.max_vars = sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1);
  q.max_args = sqlite3_limit(db, SQLITE_LIMIT_FUNCTION_ARG, -1);
  sb_str(&q.sql, "SELECT count(*) FROM defiant_record r");
  emit_range(&q, plan->prefix, plan->prefix_len, hi, plan->prefix_len);
  if (query->where != NULL) {
    emit_node(&q, query->where);
  } else {
    sb_str(&q.sql, "1");
  }
  sb_str(&q.sql, ")");
  if (q.unsupported) {
    result = GDEFSTOR_ERR_UNSUPPORTED;
  } else {
    rctx.want_rows = 0;
    result = run_statement(db, &q, &rctx);
    out->total = (uint64_t)rctx.count;
  }
  qb_free(&q);

  if (result == GDEFSTOR_OK && !plan->count_only) {
    memset(&q, 0, sizeof q);
    q.alloc = bag->alloc;
    q.sql.alloc = bag->alloc;
    q.prefix_len = plan->prefix_len;
    q.max_vars = sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1);
    q.max_args = sqlite3_limit(db, SQLITE_LIMIT_FUNCTION_ARG, -1);
    sb_str(&q.sql, "SELECT r.key, r.bytes FROM defiant_record r");
    emit_range(&q, plan->prefix, plan->prefix_len, hi, plan->prefix_len);
    if (query->where != NULL) {
      emit_node(&q, query->where);
    } else {
      sb_str(&q.sql, "1");
    }
    sb_str(&q.sql, ") ORDER BY ");
    /* The entity argument, ascending, is the final tiebreak. */
    memset(tie, 0, sizeof tie);
    tie[0].key.kind = GDEFSTOR_QUERY_ENTITY;
    tie[1].key.kind = GDEFSTOR_QUERY_ENTITY;
    if (query->entity_numeric) {
      tie[0].numeric = 1;
      ntie = 2;
    } else {
      ntie = 1;
    }
    for (i = 0; i < query->sort_count + ntie; i++) {
      const GDEFSTOR_QuerySort * s =
          i < query->sort_count ? &query->sorts[i]
                                : &tie[(i - query->sort_count) + (2 - ntie)];
      if (i > 0) {
        sb_str(&q.sql, ", ");
      }
      sb_str(&q.sql, "gdq_key(");
      qb_bind(&q, kBindPointer, s, 0, 0);
      sb_str(&q.sql, ", r.bytes, r.key)");
      if (s->descending) {
        sb_str(&q.sql, " DESC");
      }
    }
    sb_str(&q.sql, " LIMIT ");
    qb_bind(&q, kBindInt, NULL, 0,
        plan->limit == UINT64_MAX ? -1 : (sqlite3_int64)plan->limit);
    sb_str(&q.sql, " OFFSET ");
    qb_bind(&q, kBindInt, NULL, 0,
        query->offset > (uint64_t)INT64_MAX ? INT64_MAX
                                            : (sqlite3_int64)query->offset);
    if (q.unsupported) {
      result = GDEFSTOR_ERR_UNSUPPORTED;
    } else {
      rctx.want_rows = 1;
      result = run_statement(db, &q, &rctx);
    }
    qb_free(&q);
  }
  gcu_allocator_free(bag->alloc, hi);
  rc = db == bag->txn_db ? SQLITE_OK
                        : sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK && result == GDEFSTOR_OK) {
    result = from_sqlite(rc);
    rollback_db(bag, db);
  }
  close_db(bag, db);
  if (result == GDEFSTOR_ERR_UNSUPPORTED) {
    gdefssqlite_query_fallback += 1;
  } else if (result == GDEFSTOR_OK) {
    gdefssqlite_query_native += 1;
  }
  if (result != GDEFSTOR_OK) {
    free_rows(bag->alloc, out);
  }
  return result;
}

static GDEFSTOR_Result bag_begin(void * ctx, const char * tenant,
    size_t tenant_len) {
  SqlBag * bag = (SqlBag *)ctx;
  sqlite3 * db = NULL;
  GDEFSTOR_Result mapped;
  int rc;
  if (bag == NULL || tenant == NULL || tenant_len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (bag->txn_db != NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  mapped = open_tenant(bag, tenant, tenant_len, 1, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = exec_begin(db);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return from_sqlite(rc);
  }
  bag->txn_tenant = (char *)gcu_allocator_malloc(bag->alloc, tenant_len + 1);
  if (bag->txn_tenant == NULL) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    return GDEFSTOR_ERR_OOM;
  }
  memcpy(bag->txn_tenant, tenant, tenant_len);
  bag->txn_tenant[tenant_len] = '\0';
  bag->txn_tenant_len = tenant_len;
  bag->txn_db = db;
  bag->puts_in_txn = 0;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_commit(void * ctx) {
  SqlBag * bag = (SqlBag *)ctx;
  int rc;
  if (bag == NULL || bag->txn_db == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = sqlite3_exec(bag->txn_db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    return from_sqlite(rc);
  }
  sqlite3_close(bag->txn_db);
  bag->txn_db = NULL;
  gcu_allocator_free(bag->alloc, bag->txn_tenant);
  bag->txn_tenant = NULL;
  bag->txn_tenant_len = 0;
  bag->puts_in_txn = 0;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_abort(void * ctx) {
  SqlBag * bag = (SqlBag *)ctx;
  if (bag == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  clear_txn(bag);
  return GDEFSTOR_OK;
}

GDEFSSQLITE_Result gdefssqlite_store_create(const char * directory,
    const GDEFSSQLITE_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store) {
  const GDEFSSQLITE_Allocator * use = gdefssqlite_allocator_default();
  SqlBag * bag;
  GDEFSTOR_Backend backend;
  GDEFSTOR_Result rc;
  if (allocator != NULL) {
    use = allocator;
  }
  if (out_store == NULL || directory == NULL || directory[0] == '\0') {
    return GDEFSSQLITE_ERR_INVALID;
  }
  bag = (SqlBag *)gcu_allocator_calloc(use, 1, sizeof *bag);
  if (bag == NULL) {
    return GDEFSSQLITE_ERR_OOM;
  }
  bag->alloc = use;
  bag->dir = dup_str(use, directory);
  if (bag->dir == NULL) {
    gcu_allocator_free(use, bag);
    return GDEFSSQLITE_ERR_OOM;
  }
  memset(&backend, 0, sizeof backend);
  backend.ctx = bag;
  backend.destroy = bag_destroy;
  backend.put = bag_put;
  backend.get = bag_get;
  backend.remove = bag_remove;
  backend.list = bag_list;
  backend.begin = bag_begin;
  backend.commit = bag_commit;
  backend.abort = bag_abort;
  backend.query = bag_query;
  rc = gdefstor_store_create(&backend, use, limits, out_store);
  if (rc != GDEFSTOR_OK) {
    bag_destroy(bag);
  }
  return from_port(rc);
}
