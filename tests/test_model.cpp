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
 * The port's entity conformance suite (model.inc), the purchase-order fixture
 * included, and its key store suite (keystore.inc), on SQLite files. Each
 * store is a fresh directory, and its backend is tee'd (model_util.h): every
 * query the suite asks is also answered by gdefstor_model_scan() over the
 * same rows, and a difference fails the test that asked it.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>

#include "model_util.h"
#include "tmpdir.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace {

struct Opened {
  std::string dir;
  tee::Tee * tee = nullptr;
};

/* The allocation-failure sweeps open and close stores from several threads at
 * once (model.inc), so the table of open stores is locked. */
std::map<GDEFSTOR_Store *, Opened> & Stores() {
  static std::map<GDEFSTOR_Store *, Opened> m;
  return m;
}

std::mutex & StoresLock() {
  static std::mutex m;
  return m;
}

/* Key stores are closed by the suite itself, so their directories go at exit. */
std::vector<std::string> & KeyDirs() {
  static std::vector<std::string> v;
  return v;
}

GDEFSTOR_Result ToPort(GDEFSSQLITE_Result rc) {
  switch (rc) {
  case GDEFSSQLITE_OK:
    return GDEFSTOR_OK;
  case GDEFSSQLITE_ERR_OOM:
    return GDEFSTOR_ERR_OOM;
  case GDEFSSQLITE_ERR_INVALID:
    return GDEFSTOR_ERR_INVALID;
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
}

/* Quiet refusals: the suite opens nothing that is refused. */
void Refused(void * ctx, int result, const char * text, size_t len) {
  (void)ctx;
  ADD_FAILURE() << "a file was refused (" << result << "): "
                << std::string(text, len);
}

}  // namespace

extern "C" GDEFSTOR_Result ModelOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * alloc) {
  Opened o;
  o.dir = MakeTempDir("gdefssqlite-model-");
  if (o.dir.empty()) {
    return GDEFSTOR_ERR_IO;
  }
  GDEFSSQLITE_ModelOptions opts;
  gdefssqlite_model_options_default(&opts);
  opts.on_refusal = Refused;
  /* The conformance suite's subject is the port, not durability; the files
   * and crash tests (test_files.cpp) open at the default. Its allocation-
   * failure tests open a fresh store for every allocation: at FULL the first
   * took 1,149 s on disk and 35 s on tmpfs (2026-10-19). */
  opts.synchronous = GDEFSSQLITE_SYNC_OFF;
  GDEFSTOR_ModelBackend model;
  GDEFSSQLITE_Result rc = gdefssqlite_model_backend(o.dir.c_str(), &opts,
      alloc, &model);
  if (rc != GDEFSSQLITE_OK) {
    std::error_code ec;
    std::filesystem::remove_all(o.dir, ec);
    return ToPort(rc);
  }
  o.tee = new tee::Tee();
  o.tee->on_disagree = [](const std::string & what) {
    ADD_FAILURE() << "the compiled query and the reference scan disagree:\n"
                  << what;
  };
  GDEFSTOR_ModelBackend teed = tee::TeeOf(o.tee, model);
  GDEFSTOR_Result port = gdefstor_store_open(&teed, alloc, nullptr, out);
  if (port != GDEFSTOR_OK) {
    model.destroy(model.ctx);
    delete o.tee;
    std::error_code ec;
    std::filesystem::remove_all(o.dir, ec);
    return port;
  }
  {
    std::lock_guard<std::mutex> hold(StoresLock());
    Stores()[*out] = o;
  }
  return GDEFSTOR_OK;
}

extern "C" void ModelClose(GDEFSTOR_Store * store) {
  Opened o;
  bool known = false;
  {
    std::lock_guard<std::mutex> hold(StoresLock());
    auto it = Stores().find(store);
    if (it != Stores().end()) {
      o = it->second;
      known = true;
      Stores().erase(it);
    }
  }
  gdefstor_store_destroy(store);
  if (!known) {
    return;
  }
  EXPECT_EQ(0, o.tee->disagreements) << o.tee->first_disagreement;
  delete o.tee;
  std::error_code ec;
  std::filesystem::remove_all(o.dir, ec);
}

GDEFSTOR_Result KeyStoreOpen(const GDEFSTOR_KeyCipher * cipher,
    GDEFSTOR_KeyStore ** out) {
  std::string dir = MakeTempDir("gdefssqlite-keys-");
  if (dir.empty()) {
    return GDEFSTOR_ERR_IO;
  }
  KeyDirs().push_back(dir);
  return ToPort(gdefssqlite_keystore_create(dir.c_str(), cipher, nullptr,
      nullptr, out));
}

#include "model.inc"
#include "keystore.inc"

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  int rc = RUN_ALL_TESTS();
  for (const std::string & d : KeyDirs()) {
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
  }
  return rc;
}
