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
 * @file sqlite.h
 * @brief Open a port store whose bytes live in one SQLite file per tenant.
 * @stability free
 *
 * The returned store is the port's. Close it with gdefstor_store_destroy().
 * Isolation, the writer queue and the formats stay in the port. This library
 * sees a key only after the port commits. The tenant selects the file. The
 * file name is the tenant bytes as lowercase hex plus `.db`, never the
 * tenant string as a path.
 */

#ifndef GHOTI_IO_GDEFSSQLITE_SQLITE_H
#define GHOTI_IO_GDEFSSQLITE_SQLITE_H

#include <ghoti.io/defiant-storage-sqlite/allocator.h>
#include <ghoti.io/defiant-storage-sqlite/core.h>
#include <ghoti.io/defiant-storage-sqlite/macros.h>
#include <ghoti.io/defiant-storage/store.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a SQLite store rooted at a directory.
 *
 * Each tenant is one database file in @p directory. The first open of a
 * file applies forward-only schema steps. Schema version 1 is
 * `defiant_migration` and `defiant_record`. A file that still has
 * `record_v0` copies those rows in the same transaction. Loadable
 * extensions stay off. `SQLITE_BUSY` is ::GDEFSSQLITE_ERR_BUSY and the
 * call does not wait.
 *
 * @param directory Directory that will hold the files. NULL or empty is
 *   ::GDEFSSQLITE_ERR_INVALID. Not used as a prefix of the tenant.
 * @param allocator NULL uses gdefssqlite_allocator_default().
 * @param limits NULL uses the port's defaults. Passed through to the port.
 * @param out_store Receives the port store on success. Not written on
 *   failure.
 * @return ::GDEFSSQLITE_OK, ::GDEFSSQLITE_ERR_INVALID, ::GDEFSSQLITE_ERR_OOM,
 *   or the port's open failure mapped onto this vocabulary.
 */
GDEFSSQLITE_API GDEFSSQLITE_Result gdefssqlite_store_create(
    const char * directory, const GDEFSSQLITE_Allocator * allocator,
    const GDEFSTOR_Limits * limits, GDEFSTOR_Store ** out_store);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQLITE_SQLITE_H */
