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
 * The agreement sweep on SQLite: seeded random data written alike to a
 * SQLite store and to a memory store, and seeded random queries asked of
 * both. Every SQLite answer is also answered by gdefstor_model_scan() over the
 * same SQLite rows (the tee), and must equal the memory backend's answer, page
 * order and total included. The sweep requires that it met the cases that
 * tell engines apart.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>

#include "query_gen.h"
#include "tmpdir.h"

#include <sqlite3.h>

#include <filesystem>

using namespace tu;

namespace {

struct Seen {
  int queries = 0;
  int nonempty = 0;
  int empty = 0;
  int paged = 0;
  int clamped = 0;
  int partial = 0;
  int sorted = 0;
  int descending = 0;
  int exists = 0;
  int nots = 0;
  int field_field = 0;
  int outer = 0;
};

struct Sqlite {
  std::string dir = MakeTempDir("gdefssqlite-agree-");
  tee::Tee tee;
  GDEFSTOR_Store * store = nullptr;
  ~Sqlite() {
    gdefstor_store_destroy(store);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

void Destroy(void *) {}

GDEFSTOR_Result NoRow(void *, const char *, size_t, const char *, size_t,
    uint8_t **, size_t *) {
  return GDEFSTOR_ERR_INVALID;
}

GDEFSTOR_Result Refuse(void *, const char *, size_t, const char *, size_t,
    const uint8_t *, size_t) {
  return GDEFSTOR_ERR_UNSUPPORTED;
}

GDEFSTOR_Result RefuseRemove(void *, const char *, size_t, const char *,
    size_t) {
  return GDEFSTOR_ERR_UNSUPPORTED;
}

GDEFSTOR_Result NoRows(void *, const char *, size_t, const char *, size_t,
    char *** keys, size_t ** lens, size_t * n) {
  *keys = nullptr;
  *lens = nullptr;
  *n = 0;
  return GDEFSTOR_OK;
}

void OpenSqlite(Sqlite & s, uint32_t seed) {
  ASSERT_FALSE(s.dir.empty());
  GDEFSTOR_ModelBackend model;
  ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_model_backend(s.dir.c_str(), nullptr,
      nullptr, &model));
  s.tee.on_disagree = [seed](const std::string & what) {
    ADD_FAILURE() << "seed " << seed << ": the compiled query and the "
                  << "reference scan disagree:\n" << what;
  };
  GDEFSTOR_ModelBackend teed = tee::TeeOf(&s.tee, model);
  GDEFSTOR_Backend kv;
  std::memset(&kv, 0, sizeof kv);
  kv.destroy = Destroy;
  kv.put = Refuse;
  kv.get = NoRow;
  kv.remove = RefuseRemove;
  kv.list = NoRows;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_store_create_model(&kv, &teed, nullptr,
      nullptr, &s.store));
}

void Sweep(uint32_t seed, Seen & seen) {
  Sqlite sq;
  OpenSqlite(sq, seed);
  ASSERT_NE(nullptr, sq.store);
  GDEFSTOR_Store * mem = nullptr;
  ASSERT_EQ(GDEFSMEMORY_OK, gdefsmemory_store_create(nullptr, nullptr, &mem));
  /* Two generators with one seed write the same data to each store. */
  qgen::Gen g(seed);
  qgen::Gen gm(seed);
  g.norders = 10 + g.Pick(20);
  gm.norders = 10 + gm.Pick(20);
  ASSERT_EQ(GDEFSTOR_OK, g.Populate(sq.store)) << "seed " << seed;
  ASSERT_EQ(GDEFSTOR_OK, gm.Populate(mem)) << "seed " << seed;
  for (int n = 0; n < 150; n++) {
    qgen::Gen::Built b;
    const char * type = g.Chance(5) ? "shop.cust" : "shop.order";
    g.Make(b, type);
    std::vector<std::string> ids;
    std::vector<std::string> mids;
    uint64_t total = 0;
    uint64_t mtotal = 0;
    GDEFSTOR_Result rc = Ask(sq.store, type, b.q, &ids, &total);
    ASSERT_EQ(GDEFSTOR_OK, rc) << "seed " << seed << " query " << n << ": "
                               << gdefstor_result_string(rc);
    ASSERT_EQ(GDEFSTOR_OK, Ask(mem, type, b.q, &mids, &mtotal));
    EXPECT_EQ(mids, ids) << "seed " << seed << " query " << n
                         << ": SQLite and memory pages differ";
    EXPECT_EQ(mtotal, total) << "seed " << seed << " query " << n;
    uint64_t count = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(sq.store, kTenant, 2, type,
        std::strlen(type), b.q.where, &count));
    EXPECT_EQ(total, count) << "seed " << seed << " query " << n;
    seen.queries++;
    (ids.empty() ? seen.empty : seen.nonempty)++;
    seen.paged += b.q.offset > 0 && total > b.q.offset;
    seen.clamped += b.q.limit > GDEFSTOR_QUERY_LIMIT_MAX && total > 0;
    seen.partial += ids.size() < total && !ids.empty();
    seen.sorted += b.q.sort_count > 0 && ids.size() > 2;
    for (size_t k = 0; k < b.q.sort_count; k++) {
      seen.descending += b.q.sorts[k].descending && ids.size() > 2;
    }
    seen.exists += b.exists;
    seen.nots += b.nots;
    seen.field_field += b.field_field;
    seen.outer += b.outer;
  }
  EXPECT_EQ(0, sq.tee.disagreements) << sq.tee.first_disagreement;
  EXPECT_EQ(0, sq.tee.refusals);
  EXPECT_EQ(300, sq.tee.queries);
  gdefstor_store_destroy(mem);
}

TEST(Agreement, SqliteMatchesTheReferenceScanAndTheMemoryBackend) {
  Seen s;
  for (uint32_t seed = 1; seed <= 6; seed++) {
    Sweep(seed, s);
  }
  EXPECT_GT(s.nonempty, 300);
  EXPECT_GT(s.empty, 50);
  EXPECT_GT(s.paged, 50);
  EXPECT_GT(s.clamped, 20);
  EXPECT_GT(s.partial, 100);
  EXPECT_GT(s.sorted, 200);
  EXPECT_GT(s.descending, 100);
  EXPECT_GT(s.exists, 50);
  EXPECT_GT(s.nots, 100);
  EXPECT_GT(s.field_field, 20);
  EXPECT_GT(s.outer, 30);
}

/* The derived tables, wiped by hand and rebuilt by the port, hold what they
 * held, and the rebuilt store answers as the memory backend does. */
TEST(Agreement, RebuiltCurrentTablesAnswerAlike) {
  Sqlite sq;
  OpenSqlite(sq, 77);
  ASSERT_NE(nullptr, sq.store);
  qgen::Gen g(77);
  ASSERT_EQ(GDEFSTOR_OK, g.Populate(sq.store));
  uint8_t * before = nullptr;
  size_t blen = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(sq.store, kTenant, 2,
      GDEFSTOR_MODEL_CURRENT, &before, &blen));
  /* Wipe the derived tables from outside, so the rebuild has to make them. */
  {
    sqlite3 * db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open_v2((sq.dir + "/7431.db").c_str(), &db,
        SQLITE_OPEN_READWRITE, nullptr));
    std::vector<std::string> derived;
    sqlite3_stmt * st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_schema WHERE type = 'table' AND (name = "
        "'dss_entity' OR name GLOB 't[0-9]*_n' OR name GLOB 't[0-9]*_c[0-9]*')",
        -1, &st, nullptr));
    while (sqlite3_step(st) == SQLITE_ROW) {
      derived.emplace_back((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    EXPECT_GE(derived.size(), 8u);
    for (const std::string & t : derived) {
      ASSERT_EQ(SQLITE_OK, sqlite3_exec(db, ("DELETE FROM \"" + t + "\"").c_str(),
          nullptr, nullptr, nullptr));
    }
    sqlite3_close_v2(db);
  }
  ASSERT_EQ(GDEFSTOR_OK, InWrite(sq.store, [](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  uint8_t * after = nullptr;
  size_t alen = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(sq.store, kTenant, 2,
      GDEFSTOR_MODEL_CURRENT, &after, &alen));
  EXPECT_EQ(std::string((char *)before, blen), std::string((char *)after, alen));
  gcu_allocator_free(nullptr, before);
  gcu_allocator_free(nullptr, after);
  for (int n = 0; n < 50; n++) {
    qgen::Gen::Built b;
    g.Make(b, "shop.order");
    std::vector<std::string> ids;
    uint64_t total = 0;
    ASSERT_EQ(GDEFSTOR_OK, Ask(sq.store, "shop.order", b.q, &ids, &total));
  }
  EXPECT_EQ(0, sq.tee.disagreements) << sq.tee.first_disagreement;
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
