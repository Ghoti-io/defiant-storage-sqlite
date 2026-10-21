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
 * Random records written to and read back from a SQLite store's entity
 * port. The first byte says whether the write commits and whether the record
 * is read back; the rest is the record's value. Files land in one temporary
 * directory for the process.
 */

#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  static char dir[64];
  static int ready = 0;
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSSQLITE_ModelOptions opts;
  if (data == NULL || size < 2) {
    return 0;
  }
  if (!ready) {
    snprintf(dir, sizeof dir, "/tmp/gdefssqlite-fuzz-%d", (int)getpid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
      return 0;
    }
    ready = 1;
  }
  gdefssqlite_model_options_default(&opts);
  opts.synchronous = GDEFSSQLITE_SYNC_OFF;
  if (gdefssqlite_model_store_create(dir, &opts, NULL, NULL, &store) !=
      GDEFSSQLITE_OK) {
    return 0;
  }
  if (gdefstor_write_begin(store, "t", 1, NULL, &write) == GDEFSTOR_OK) {
    gdefstor_record_put(write, "fuzz.ops", 8, "k", 1, data + 1, size - 1);
    if (data[0] & 1u) {
      gdefstor_write_commit(write);
    } else {
      gdefstor_write_abort(write);
    }
  }
  if (data[0] & 2u) {
    uint8_t * bytes = NULL;
    size_t len = 0;
    int found = 0;
    if (gdefstor_record_get(store, "t", 1, "fuzz.ops", 8, "k", 1, &found,
            &bytes, &len) == GDEFSTOR_OK) {
      gdefstor_bytes_free(NULL, bytes);
    }
  }
  gdefstor_store_destroy(store);
  return 0;
}
