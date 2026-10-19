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
 * Records, counters and the outbox (story S6a) where only real files and
 * several connections can show it: two stores on one tenant file, each on a
 * thread of its own, never take one counter value, an aborted write's value
 * included, and never hold live leases on one stream at once; a value an
 * aborted write took stays taken after the store is closed and opened again.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <sqlite3.h>

#include "model_util.h"
#include "tmpdir.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace tu;

namespace {

/* "t1" as the file name model.h gives it. */
const char * const kFile = "7431.db";

struct Dir {
  std::string path = MakeTempDir("gdefssqlite-system-");
  ~Dir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

/* A store on @p d that waits for the other writer rather than failing, and
 * does not wait for the disk (the subject here is isolation, not
 * durability). */
GDEFSTOR_Store * Open(const Dir & d) {
  GDEFSSQLITE_ModelOptions o;
  gdefssqlite_model_options_default(&o);
  o.busy_timeout_ms = 60000;
  o.synchronous = GDEFSSQLITE_SYNC_OFF;
  GDEFSTOR_Store * store = nullptr;
  EXPECT_EQ(GDEFSSQLITE_OK, gdefssqlite_model_store_create(d.path.c_str(), &o,
      nullptr, nullptr, &store));
  return store;
}

uint64_t Peek(GDEFSTOR_Store * s, const char * name) {
  uint64_t v = UINT64_MAX;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_counter_peek(s, kTenant, 2, name,
      std::strlen(name), &v));
  return v;
}

/* Each of two stores, on threads of their own, takes counter values in
 * writes it commits or aborts. No value is handed out twice, whether its
 * write committed or not, and the counter ends at the highest committed. */
TEST(SystemConcurrency, TwoStoresNeverTakeOneCounterValue) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  GDEFSTOR_Store * stores[2] = {Open(d), Open(d)};
  ASSERT_NE(nullptr, stores[0]);
  ASSERT_NE(nullptr, stores[1]);
  /* The tenant's tables first, from one store, so that neither thread's
   * first write races the other's to make them. */
  ASSERT_EQ(GDEFSTOR_OK, InWrite(stores[0], [](GDEFSTOR_Write * w) {
    return gdefstor_record_put(w, "core.m", 6, "k", 1, nullptr, 0);
  }));
  std::mutex mu;
  std::vector<uint64_t> given;
  std::vector<uint64_t> committed;
  std::vector<std::string> errors;
  auto work = [&](int who) {
    for (int i = 0; i < 150; i++) {
      GDEFSTOR_Write * w = nullptr;
      GDEFSTOR_Result rc = gdefstor_write_begin(stores[who], kTenant, 2,
          nullptr, &w);
      uint64_t v = 0;
      if (rc == GDEFSTOR_OK) {
        rc = gdefstor_counter_next(w, "core.epoch", 10, &v);
        if (rc != GDEFSTOR_OK) {
          gdefstor_write_abort(w);
        }
      }
      if (rc != GDEFSTOR_OK) {
        std::lock_guard<std::mutex> g(mu);
        errors.push_back(std::to_string(who) + ": " +
                         gdefstor_result_string(rc));
        continue;
      }
      bool commit = (i + who) % 4 != 0;
      rc = commit ? gdefstor_write_commit(w) : gdefstor_write_abort(w);
      std::lock_guard<std::mutex> g(mu);
      given.push_back(v);
      if (rc != GDEFSTOR_OK) {
        errors.push_back(std::to_string(who) + " end: " +
                         gdefstor_result_string(rc));
      } else if (commit) {
        committed.push_back(v);
      }
    }
  };
  std::thread a(work, 0);
  std::thread b(work, 1);
  a.join();
  b.join();
  EXPECT_TRUE(errors.empty()) << errors.size() << " errors, first: "
                              << (errors.empty() ? "" : errors[0]);
  EXPECT_EQ(300u, given.size());
  std::set<uint64_t> unique(given.begin(), given.end());
  EXPECT_EQ(given.size(), unique.size()) << "a counter value was handed out twice";
  ASSERT_FALSE(committed.empty());
  EXPECT_EQ(*std::max_element(committed.begin(), committed.end()),
      Peek(stores[0], "core.epoch"));
  EXPECT_EQ(Peek(stores[0], "core.epoch"), Peek(stores[1], "core.epoch"));
  gdefstor_store_destroy(stores[0]);
  gdefstor_store_destroy(stores[1]);
}

/* Two stores claim one stream at once, under leases far longer than the
 * test: one row at a time, each row once, in sequence, because a stream has
 * one live claim across every connection to the file. */
TEST(SystemConcurrency, TwoStoresNeverHoldLiveLeasesOnOneStream) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  GDEFSTOR_Store * stores[2] = {Open(d), Open(d)};
  ASSERT_NE(nullptr, stores[0]);
  ASSERT_NE(nullptr, stores[1]);
  const int kRows = 40;
  ASSERT_EQ(GDEFSTOR_OK, InWrite(stores[0], [&](GDEFSTOR_Write * w) {
    for (int i = 1; i <= kRows; i++) {
      std::string id = "r" + std::to_string(i);
      GDEFSTOR_Result rc = gdefstor_model_outbox_append(w, "core.bus", 8,
          id.data(), id.size(), "e/1", 3,
          reinterpret_cast<const uint8_t *>("{}"), 2, nullptr);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return GDEFSTOR_OK;
  }));
  std::mutex mu;
  std::vector<uint64_t> order;
  std::vector<std::string> errors;
  std::atomic<int> left{kRows};
  auto work = [&](int who) {
    int spins = 0;
    while (left.load() > 0 && spins < 200000) {
      spins++;
      GDEFSTOR_OutboxClaim * c = nullptr;
      GDEFSTOR_Result rc = gdefstor_model_outbox_claim(stores[who], kTenant, 2,
          "core.bus", 8, 3600000, 1000, 1, &c);
      if (rc != GDEFSTOR_OK) {
        std::lock_guard<std::mutex> g(mu);
        errors.push_back(std::string("claim: ") + gdefstor_result_string(rc));
        return;
      }
      if (gdefstor_model_outbox_claim_count(c) == 1) {
        GDEFSTOR_OutboxEntry e;
        std::memset(&e, 0, sizeof e);
        e.size = sizeof e;
        gdefstor_model_outbox_claim_entry(c, 0, &e);
        std::string id(e.public_id, e.public_id_len);
        {
          std::lock_guard<std::mutex> g(mu);
          order.push_back(e.seq);
        }
        const char * ids[] = {id.c_str()};
        size_t lens[] = {id.size()};
        size_t gone = 0;
        rc = gdefstor_model_outbox_delivered(stores[who], kTenant, 2, ids, lens,
            1, &gone);
        if (rc != GDEFSTOR_OK || gone != 1) {
          std::lock_guard<std::mutex> g(mu);
          errors.push_back("delivered " + id);
        }
        left.fetch_sub(1);
      }
      gdefstor_model_outbox_claim_free(c);
    }
  };
  std::thread a(work, 0);
  std::thread b(work, 1);
  a.join();
  b.join();
  EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
  ASSERT_EQ(static_cast<size_t>(kRows), order.size())
      << "a row was claimed twice, or not at all";
  for (size_t i = 0; i < order.size(); i++) {
    EXPECT_EQ(i + 1, order[i]) << "claimed out of order at " << i;
  }
  gdefstor_store_destroy(stores[0]);
  gdefstor_store_destroy(stores[1]);
}

/* The floor is in the file: a value an aborted write took is still skipped
 * by a store opened afterwards, and the committed value is unchanged. */
TEST(SystemFiles, ASkippedCounterValueStaysSkippedAfterReopen) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  GDEFSTOR_Store * s = Open(d);
  ASSERT_EQ(GDEFSTOR_OK, InWrite(s, [](GDEFSTOR_Write * w) {
    uint64_t v = 0;
    return gdefstor_counter_next(w, "core.epoch", 10, &v);
  }));
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(s, kTenant, 2, nullptr, &w));
  uint64_t v = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_counter_next(w, "core.epoch", 10, &v));
  EXPECT_EQ(2u, v);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  gdefstor_store_destroy(s);

  sqlite3 * db = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open_v2((d.path + "/" + kFile).c_str(), &db,
      SQLITE_OPEN_READONLY, nullptr));
  sqlite3_stmt * st = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
      "SELECT last, kept FROM dss_seq WHERE kind = 1 AND name = 'core.epoch'",
      -1, &st, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
  EXPECT_EQ(1, sqlite3_column_int64(st, 0)) << "the committed value";
  EXPECT_EQ(2, sqlite3_column_int64(st, 1)) << "the floor";
  sqlite3_finalize(st);
  sqlite3_close_v2(db);

  s = Open(d);
  EXPECT_EQ(1u, Peek(s, "core.epoch"));
  ASSERT_EQ(GDEFSTOR_OK, InWrite(s, [](GDEFSTOR_Write * x) {
    uint64_t n = 0;
    GDEFSTOR_Result rc = gdefstor_counter_next(x, "core.epoch", 10, &n);
    return rc == GDEFSTOR_OK && n != 3 ? GDEFSTOR_ERR_INTERNAL : rc;
  }));
  EXPECT_EQ(3u, Peek(s, "core.epoch"));
  gdefstor_store_destroy(s);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
