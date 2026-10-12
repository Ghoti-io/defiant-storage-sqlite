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
 * Runs the port's conformance suite against the SQLite backend. Each store
 * gets its own temporary directory.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>

#include <filesystem>
#include "tmpdir.h"
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {

static GDEFSTOR_Result to_port(GDEFSSQLITE_Result rc) {
  switch (rc) {
  case GDEFSSQLITE_OK:
    return GDEFSTOR_OK;
  case GDEFSSQLITE_ERR_IO:
    return GDEFSTOR_ERR_IO;
  case GDEFSSQLITE_ERR_FORMAT:
    return GDEFSTOR_ERR_FORMAT;
  case GDEFSSQLITE_ERR_UNSUPPORTED:
    return GDEFSTOR_ERR_UNSUPPORTED;
  case GDEFSSQLITE_ERR_LIMIT:
    return GDEFSTOR_ERR_LIMIT;
  case GDEFSSQLITE_ERR_CORRUPT:
    return GDEFSTOR_ERR_CORRUPT;
  case GDEFSSQLITE_ERR_OOM:
    return GDEFSTOR_ERR_OOM;
  case GDEFSSQLITE_ERR_INVALID:
    return GDEFSTOR_ERR_INVALID;
  case GDEFSSQLITE_ERR_INTERNAL:
    return GDEFSTOR_ERR_INTERNAL;
  case GDEFSSQLITE_ERR_BUSY:
    return GDEFSTOR_ERR_BUSY;
  case GDEFSSQLITE_RESULT_COUNT:
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
}

static std::vector<std::pair<GDEFSTOR_Store *, std::string>> g_dirs;

GDEFSTOR_Result ConformanceOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * allocator) {
  std::string dir;
  GDEFSTOR_Store * store = NULL;
  GDEFSSQLITE_Result rc;
  if (out == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  dir = MakeTempDir("gdefssqlite-conf-");
  if (dir.empty()) {
    return GDEFSTOR_ERR_IO;
  }
  rc = gdefssqlite_store_create(dir.c_str(), allocator, NULL, &store);
  if (rc != GDEFSSQLITE_OK) {
    std::filesystem::remove_all(dir);
    return to_port(rc);
  }
  g_dirs.push_back(std::make_pair(store, std::string(dir)));
  *out = store;
  return GDEFSTOR_OK;
}

void ConformanceClose(GDEFSTOR_Store * store) {
  std::string path;
  for (auto it = g_dirs.begin(); it != g_dirs.end(); ++it) {
    if (it->first == store) {
      path = it->second;
      g_dirs.erase(it);
      break;
    }
  }
  gdefstor_store_destroy(store);
  if (!path.empty()) {
    std::filesystem::remove_all(path);
  }
}

} /* extern "C" */

#include "suite.inc"

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
