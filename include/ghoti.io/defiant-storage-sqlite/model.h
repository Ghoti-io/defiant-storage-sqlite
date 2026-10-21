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
 * @file model.h
 * @brief The entity port (defiant-storage's model.h) and the key store port
 * on SQLite, through defiant-storage-sql.
 * @stability free
 *
 * defiant-storage-sql writes every statement; this library gives it the
 * SQLite dialect and an executor, and owns the files, the connections, the
 * statement cache and the pragmas (documentation/sqlite.md).
 *
 * **Files.** A data store is a directory with one SQLite file per tenant,
 * named by the lowercase hex of the tenant bytes plus `.db`; the tenant
 * string is never a path. A key store is a directory with one file per tenant named by
 * the same hex plus `.keys.db`, so it never shares a file with a data store
 * even in the same directory. Every file is in WAL mode, `synchronous=FULL`;
 * a key-store file also has `secure_delete=ON`, and after each write that
 * commits, its WAL is checkpointed and truncated.
 *
 * **What is refused.** Before a connection to an existing file is used for
 * anything, its catalog is read (a pragma). A file that holds tables but not
 * this layout's - tables and no `dss_schema`, a key store opened as a data
 * store, a data store opened as a key store - is ::GDEFSTOR_ERR_FORMAT; a
 * layout version above this build's is ::GDEFSTOR_ERR_UNSUPPORTED, and any
 * other version but this build's is ::GDEFSTOR_ERR_FORMAT. Either way the
 * file is left byte for byte as it was: no table is added, the journal mode
 * is not changed. The refusal is reported through
 * GDEFSSQLITE_ModelOptions::on_refusal with a sentence naming the file and
 * the cause.
 *
 * **Connections.** Each tenant has one write connection, used by one write at
 * a time, and read connections of its own (`query_only`): a read never runs
 * on the write connection, so it sees the last commit while a write is open.
 * A begin that finds another process writing waits up to
 * GDEFSSQLITE_ModelOptions::busy_timeout_ms (SQLite's busy handler retries),
 * then is ::GDEFSTOR_ERR_BUSY. A connection the SQL layer returns after an
 * error is closed, never reused (AD-14). Prepared statements are cached per
 * connection by their text, which is per tenant and query shape.
 *
 * A store or key store made here is used from one thread at a time.
 */

#ifndef GHOTI_IO_GDEFSSQLITE_MODEL_H
#define GHOTI_IO_GDEFSSQLITE_MODEL_H

#include <ghoti.io/defiant-storage-sqlite/allocator.h>
#include <ghoti.io/defiant-storage-sqlite/core.h>
#include <ghoti.io/defiant-storage-sqlite/macros.h>
#include <ghoti.io/defiant-storage-sql/dialect.h>
#include <ghoti.io/defiant-storage/keystore.h>
#include <ghoti.io/defiant-storage/model_backend.h>
#include <ghoti.io/defiant-storage/store.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What one statement did, as SQLite counted it: for rows-scanned
 * checks (never timings) and diagnostics.
 */
typedef struct GDEFSSQLITE_StatementStats {
  size_t size;                ///< `sizeof(GDEFSSQLITE_StatementStats)`.
  const char * sql;           ///< The statement's text; borrowed for the call.
  size_t sql_len;             ///< Its length.
  unsigned connection;        ///< The connection's number, unique in its store.
  int write;                  ///< Non-zero on a tenant's write connection.
  int keys;                   ///< Non-zero in a key store.
  /** Non-zero for a statement run by the executor's exec: DDL and the
   * transaction statements. Its counters are zero. */
  int exec;
  int cached;                 ///< Its prepared form came from the cache.
  uint64_t rows;              ///< Rows it returned.
  /** `SQLITE_STMTSTATUS_FULLSCAN_STEP`: steps forward in a full table scan.
   * Zero when every table it read was reached by an index or a key. */
  uint64_t fullscan_steps;
  uint64_t vm_steps;          ///< `SQLITE_STMTSTATUS_VM_STEP`.
  uint64_t sorts;             ///< `SQLITE_STMTSTATUS_SORT`.
  uint64_t autoindexes;       ///< `SQLITE_STMTSTATUS_AUTOINDEX`: rows put in a transient index.
} GDEFSSQLITE_StatementStats;

/** Called once for each statement, when the SQL layer finalizes it. */
typedef void (*GDEFSSQLITE_StatementObserver)(void * ctx,
    const GDEFSSQLITE_StatementStats * stats);

/**
 * Called when a file is refused. @p result is a ::GDEFSTOR_Result; @p text is
 * one sentence naming the file and the cause, borrowed for the call.
 */
typedef void (*GDEFSSQLITE_Refusal)(void * ctx, int result, const char * text,
    size_t len);

/**
 * How hard each commit waits for the disk (`PRAGMA synchronous`). FULL is
 * the default and what a deployment uses: a commit that returned survives a
 * power loss. OFF hands writes to the operating system and returns, which
 * survives a crashed process but not a crashed machine; it is for tests whose
 * subject is not durability, where thousands of fresh stores otherwise spend
 * their time in fsync.
 */
typedef enum GDEFSSQLITE_Sync {
  GDEFSSQLITE_SYNC_FULL = 0, ///< `synchronous=FULL`, the default.
  GDEFSSQLITE_SYNC_OFF = 1,  ///< `synchronous=OFF`, for tests.
} GDEFSSQLITE_Sync;

/** How a store or key store is opened. Size-prefixed. */
typedef struct GDEFSSQLITE_ModelOptions {
  size_t size;                ///< `sizeof(GDEFSSQLITE_ModelOptions)`.
  /** How long a begin waits for another process's write, in milliseconds.
   * 0 does not wait. Default 5000. */
  unsigned busy_timeout_ms;
  /** Idle read connections kept per tenant. Default 4. 0 keeps none. */
  size_t read_connections;
  /** Idle connections kept open in the whole store, write connections
   * included; beyond it the least recently used is closed, so a store that
   * meets many tenants does not hold a file open for each. Default 32. */
  size_t idle_connections;
  /** Prepared statements kept per connection. Default 64. 0 keeps none. */
  size_t statement_cache;
  GDEFSSQLITE_StatementObserver on_statement; ///< May be NULL.
  void * on_statement_ctx;    ///< Passed to @ref on_statement.
  /** May be NULL: each refusal is then written as one line to stderr. */
  GDEFSSQLITE_Refusal on_refusal;
  void * on_refusal_ctx;      ///< Passed to @ref on_refusal.
  /** A ::GDEFSSQLITE_Sync. Default ::GDEFSSQLITE_SYNC_FULL; any other value
   * is ::GDEFSSQLITE_ERR_INVALID. */
  int synchronous;
} GDEFSSQLITE_ModelOptions;

/** @brief Fill @p out with the defaults. NULL is ignored. */
GDEFSSQLITE_API void gdefssqlite_model_options_default(
    GDEFSSQLITE_ModelOptions * out);

/**
 * @brief The SQLite dialect this library hands defiant-storage-sql.
 *
 * Its strings are static. For tests and diagnostics; a store needs nothing
 * from it.
 */
GDEFSSQLITE_API void gdefssqlite_dialect(GDEFSSQL_Dialect * out);

/**
 * @brief The entity backend over a directory of tenant files, for
 * gdefstor_store_create_model() or for a caller that wraps it.
 *
 * @param directory Holds the tenant files. NULL or empty is
 *   ::GDEFSSQLITE_ERR_INVALID. Copied.
 * @param options NULL uses the defaults. Copied; the callbacks' contexts are
 *   borrowed for the backend's life.
 * @param allocator NULL uses gdefssqlite_allocator_default(). It must be the
 *   allocator the store is created with.
 * @param out Receives the backend; GDEFSTOR_ModelBackend::destroy releases
 *   it and closes every connection.
 * @return ::GDEFSSQLITE_OK, ::GDEFSSQLITE_ERR_INVALID (a bad argument, or an
 *   options size that is not this header's), or ::GDEFSSQLITE_ERR_OOM.
 */
GDEFSSQLITE_API GDEFSSQLITE_Result gdefssqlite_model_backend(
    const char * directory, const GDEFSSQLITE_ModelOptions * options,
    const GDEFSSQLITE_Allocator * allocator, GDEFSTOR_ModelBackend * out);

/**
 * @brief Open a store with the entity port (model.h) over a directory of
 * tenant files.
 *
 * The store is opened with gdefstor_store_open() over
 * gdefssqlite_model_backend().
 *
 * @param limits NULL uses the port's defaults. Passed through to the port.
 * @param out_store Receives the store; close it with gdefstor_store_destroy().
 * @return As gdefssqlite_model_backend(), or the port's failure.
 */
GDEFSSQLITE_API GDEFSSQLITE_Result gdefssqlite_model_store_create(
    const char * directory, const GDEFSSQLITE_ModelOptions * options,
    const GDEFSSQLITE_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store);

/**
 * @brief Open a key store (defiant-storage's keystore.h) over a directory of
 * key files, apart from every data store.
 *
 * Keys are made with @p cipher's randomness inside the key store and stored
 * in the tenant's key file; wrap and unwrap seal and open with @p cipher.
 * Destroying a key deletes its row with `secure_delete` on and truncates the
 * WAL after the commit, so its bytes are in neither file afterwards.
 *
 * @param cipher Copied; its `ctx` is borrowed for the key store's life.
 * @param out_keystore Receives the key store; close it with
 *   gdefstor_keystore_destroy().
 * @return ::GDEFSSQLITE_OK, ::GDEFSSQLITE_ERR_INVALID, ::GDEFSSQLITE_ERR_OOM,
 *   or the port's failure.
 */
GDEFSSQLITE_API GDEFSSQLITE_Result gdefssqlite_keystore_create(
    const char * directory, const GDEFSTOR_KeyCipher * cipher,
    const GDEFSSQLITE_ModelOptions * options,
    const GDEFSSQLITE_Allocator * allocator, GDEFSTOR_KeyStore ** out_keystore);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQLITE_MODEL_H */
