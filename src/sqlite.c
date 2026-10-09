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
#include <ghoti.io/cutil/allocator.h>
#include <sqlite3.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * Planted by tools/check-defects.sh. When set, the step to version 1
 * deletes the rows it just copied, so Migration.OldRowSurvives fails.
 */
static int gdefssqlite_plant_drop_row = 0; /* plant:migration */

typedef struct SqlBag {
  const GDEFSSQLITE_Allocator * alloc;
  char * dir;
} SqlBag;

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
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
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

static GDEFSTOR_Result open_tenant(SqlBag * bag, const char * tenant,
    size_t tenant_len, sqlite3 ** out) {
  char * path;
  sqlite3 * db = NULL;
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
  rc = sqlite3_open_v2(path, &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  gcu_allocator_free(bag->alloc, path);
  if (rc != SQLITE_OK) {
    if (db != NULL) {
      sqlite3_close(db);
    }
    return from_sqlite(rc);
  }
  sqlite3_busy_timeout(db, 0);
  sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, (int *)0);
  sqlite3_enable_load_extension(db, 0);
  mapped = migrate(db);
  if (mapped != GDEFSTOR_OK) {
    sqlite3_close(db);
    return mapped;
  }
  *out = db;
  return GDEFSTOR_OK;
}

static void bag_destroy(void * ctx) {
  SqlBag * bag = (SqlBag *)ctx;
  if (bag == NULL) {
    return;
  }
  gcu_allocator_free(bag->alloc, bag->dir);
  gcu_allocator_free(bag->alloc, bag);
}

static int exec_begin(sqlite3 * db) {
  return sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
}

static void exec_rollback(sqlite3 * db) {
  sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
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
  mapped = open_tenant(bag, tenant, tenant_len, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = exec_begin(db);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  rc = sqlite3_prepare_v2(db,
      "INSERT INTO defiant_record (key, bytes) VALUES (?, ?) "
      "ON CONFLICT(key) DO UPDATE SET bytes = excluded.bytes",
      -1, &st, NULL);
  if (rc != SQLITE_OK) {
    exec_rollback(db);
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
  sqlite3_bind_blob(st, 2, len == 0 ? "" : (const void *)bytes, (int)len,
      SQLITE_STATIC);
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    exec_rollback(db);
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    exec_rollback(db);
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  sqlite3_close(db);
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
  int n;
  uint8_t * copy;
  if (bag == NULL || tenant == NULL || key == NULL || out_bytes == NULL ||
      out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (key_len > INT_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  mapped = open_tenant(bag, tenant, tenant_len, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = sqlite3_prepare_v2(db,
      "SELECT bytes FROM defiant_record WHERE key = ?", -1, &st, NULL);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
  rc = sqlite3_step(st);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_close(db);
    return GDEFSTOR_ERR_INVALID;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  blob = sqlite3_column_blob(st, 0);
  n = sqlite3_column_bytes(st, 0);
  if (n < 0) {
    sqlite3_finalize(st);
    sqlite3_close(db);
    return GDEFSTOR_ERR_CORRUPT;
  }
  copy = (uint8_t *)gcu_allocator_malloc(bag->alloc, n == 0 ? 1 : (size_t)n);
  if (copy == NULL) {
    sqlite3_finalize(st);
    sqlite3_close(db);
    return GDEFSTOR_ERR_OOM;
  }
  if (n > 0 && blob != NULL) {
    memcpy(copy, blob, (size_t)n);
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
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
  mapped = open_tenant(bag, tenant, tenant_len, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = exec_begin(db);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  rc = sqlite3_prepare_v2(db, "DELETE FROM defiant_record WHERE key = ?", -1,
      &st, NULL);
  if (rc != SQLITE_OK) {
    exec_rollback(db);
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    exec_rollback(db);
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  if (sqlite3_changes(db) == 0) {
    exec_rollback(db);
    sqlite3_close(db);
    return GDEFSTOR_ERR_INVALID;
  }
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    exec_rollback(db);
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  sqlite3_close(db);
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
  mapped = open_tenant(bag, tenant, tenant_len, &db);
  if (mapped != GDEFSTOR_OK) {
    return mapped;
  }
  rc = sqlite3_prepare_v2(db, "SELECT key FROM defiant_record ORDER BY key",
      -1, &st, NULL);
  if (rc != SQLITE_OK) {
    mapped = from_sqlite(rc);
    sqlite3_close(db);
    return mapped;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    int n = sqlite3_column_bytes(st, 0);
    const unsigned char * text;
    if (n < 0) {
      sqlite3_finalize(st);
      sqlite3_close(db);
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
    sqlite3_close(db);
    return mapped;
  }
  sqlite3_reset(st);
  if (count == 0) {
    sqlite3_finalize(st);
    sqlite3_close(db);
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
    sqlite3_close(db);
    return GDEFSTOR_ERR_OOM;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    int n = sqlite3_column_bytes(st, 0);
    const unsigned char * text = sqlite3_column_text(st, 0);
    char * copy;
    if ((size_t)n < prefix_len ||
        (prefix_len != 0 && memcmp(text, prefix, prefix_len) != 0)) {
      continue;
    }
    copy = (char *)gcu_allocator_malloc(bag->alloc, (size_t)n + 1);
    if (copy == NULL) {
      size_t j;
      for (j = 0; j < i; j++) {
        gcu_allocator_free(bag->alloc, keys[j]);
      }
      gcu_allocator_free(bag->alloc, keys);
      gcu_allocator_free(bag->alloc, lens);
      sqlite3_finalize(st);
      sqlite3_close(db);
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
  sqlite3_close(db);
  if (rc != SQLITE_DONE) {
    size_t j;
    for (j = 0; j < i; j++) {
      gcu_allocator_free(bag->alloc, keys[j]);
    }
    gcu_allocator_free(bag->alloc, keys);
    gcu_allocator_free(bag->alloc, lens);
    return from_sqlite(rc);
  }
  *out_keys = keys;
  *out_lens = lens;
  *out_count = i;
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
  rc = gdefstor_store_create(&backend, use, limits, out_store);
  if (rc != GDEFSTOR_OK) {
    bag_destroy(bag);
  }
  return from_port(rc);
}
