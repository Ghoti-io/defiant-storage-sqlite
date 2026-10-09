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
 * Result strings and the default allocator.
 */

#include <ghoti.io/defiant-storage-sqlite/allocator.h>
#include <ghoti.io/defiant-storage-sqlite/core.h>
#include <ghoti.io/cutil/allocator.h>

const GDEFSSQLITE_Allocator * gdefssqlite_allocator_default(void) {
  return gcu_allocator_default();
}

const char * gdefssqlite_result_string(GDEFSSQLITE_Result result) {
  switch (result) {
  case GDEFSSQLITE_OK:
    return "No error";
  case GDEFSSQLITE_ERR_IO:
    return "I/O error";
  case GDEFSSQLITE_ERR_FORMAT:
    return "Invalid format";
  case GDEFSSQLITE_ERR_UNSUPPORTED:
    return "Unsupported format";
  case GDEFSSQLITE_ERR_LIMIT:
    return "Limit exceeded";
  case GDEFSSQLITE_ERR_CORRUPT:
    return "Corrupt data";
  case GDEFSSQLITE_ERR_OOM:
    return "Out of memory";
  case GDEFSSQLITE_ERR_INVALID:
    return "Invalid argument";
  case GDEFSSQLITE_ERR_INTERNAL:
    return "Internal error";
  case GDEFSSQLITE_ERR_BUSY:
    return "Database busy";
  case GDEFSSQLITE_RESULT_COUNT:
  default:
    return "Unknown error";
  }
}
