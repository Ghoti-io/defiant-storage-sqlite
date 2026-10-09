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
 * @file core.h
 * @brief Result codes for the SQLite storage backend.
 * @stability free
 *
 * The vocabulary matches the port's, name for name. Numeric values are not
 * a contract: a caller maps with a switch, it does not cast.
 */

#ifndef GHOTI_IO_GDEFSSQLITE_CORE_H
#define GHOTI_IO_GDEFSSQLITE_CORE_H

#include <ghoti.io/defiant-storage-sqlite/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a SQLite-backend call.
 *
 * Same names as the port, including ::GDEFSSQLITE_ERR_BUSY, so a backend and
 * the port can be talked about with one vocabulary.
 */
typedef enum GDEFSSQLITE_Result {
  GDEFSSQLITE_OK = 0,          ///< Succeeded.
  GDEFSSQLITE_ERR_IO,          ///< A stream or sink reported an error.
  GDEFSSQLITE_ERR_FORMAT,      ///< Well formed input that is not this format.
  GDEFSSQLITE_ERR_UNSUPPORTED, ///< Unknown format name, or a newer version.
  GDEFSSQLITE_ERR_LIMIT,       ///< A limit was exceeded. Nothing is truncated.
  GDEFSSQLITE_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GDEFSSQLITE_ERR_OOM,         ///< The allocator returned NULL.
  GDEFSSQLITE_ERR_INVALID,     ///< A caller argument is wrong, or a missing row.
  GDEFSSQLITE_ERR_INTERNAL,    ///< A library invariant failed.
  GDEFSSQLITE_ERR_BUSY,        ///< SQLite returned busy. The call did not wait.
  GDEFSSQLITE_RESULT_COUNT     ///< Not a result. Closes the string table.
} GDEFSSQLITE_Result;

/**
 * @brief A static description of a result. Never NULL.
 *
 * @param result The code, including out-of-range values.
 * @return A string the caller does not free.
 */
GDEFSSQLITE_API const char * gdefssqlite_result_string(GDEFSSQLITE_Result result);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQLITE_CORE_H */
