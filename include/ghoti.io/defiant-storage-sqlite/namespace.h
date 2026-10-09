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
 * @file namespace.h
 * @brief Maps every public name into this build's version namespace.
 */

#ifndef GHOTI_IO_GDEFSSQLITE_NAMESPACE_H
#define GHOTI_IO_GDEFSSQLITE_NAMESPACE_H

#include <ghoti.io/defiant-storage-sqlite/libver.h>

/// @cond HIDDEN_SYMBOLS

#define GDEFSSQLITE_Allocator GHOTIIO_DEFSSQLITE(GDEFSSQLITE_Allocator)
#define GDEFSSQLITE_Result GHOTIIO_DEFSSQLITE(GDEFSSQLITE_Result)

#define gdefssqlite_allocator_default                                          \
  GHOTIIO_DEFSSQLITE(gdefssqlite_allocator_default)
#define gdefssqlite_result_string GHOTIIO_DEFSSQLITE(gdefssqlite_result_string)
#define gdefssqlite_store_create GHOTIIO_DEFSSQLITE(gdefssqlite_store_create)

/// @endcond

#endif /* GHOTI_IO_GDEFSSQLITE_NAMESPACE_H */
