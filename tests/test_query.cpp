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
 * What the SQLite backend does with a query that the port's suite cannot
 * see: that it answers it itself (the suite would pass on the port's scan too),
 * what it hands back to the port when it cannot, and the shape of its
 * statement's range. The answers themselves are checked by the port's
 * conformance suite, which test_conformance.cpp includes.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <vector>
#include "tmpdir.h"

extern "C" {
extern int gdefssqlite_query_native;
extern int gdefssqlite_query_fallback;
}

namespace {

class QueryDir : public ::testing::Test {
 protected:
  std::string dir;
  GDEFSTOR_Store * store = NULL;
  void SetUp() override {
    dir = MakeTempDir("gdefssqlite-query-");
    ASSERT_FALSE(dir.empty());
    ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_store_create(dir.c_str(), NULL, NULL, &store));
    gdefssqlite_query_native = 0;
    gdefssqlite_query_fallback = 0;
  }
  void TearDown() override {
    gdefstor_store_destroy(store);
    std::filesystem::remove_all(dir);
  }
  void Fill(size_t n) {
    GDEFSTOR_Write * write = NULL;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, "t", 1, NULL, &write));
    for (size_t i = 0; i < n; i++) {
      std::string entity = std::to_string(i + 1);
      std::string v = std::to_string(i % 10);
      GDEFSTOR_Column col;
      memset(&col, 0, sizeof col);
      col.name = "k";
      col.name_len = 1;
      col.value = (const uint8_t *)v.data();
      col.value_len = v.size();
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_put(write, "rows", 4,
                                 entity.data(), entity.size(), &col, 1));
    }
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  }
  /* The entities of a query for k = v, as a comma list. */
  std::string Find(const std::vector<std::string> & members, uint64_t * total) {
    std::vector<GDEFSTOR_QueryText> texts;
    for (const auto & m : members) {
      GDEFSTOR_QueryText t;
      t.bytes = (const uint8_t *)m.data();
      t.len = m.size();
      texts.push_back(t);
    }
    GDEFSTOR_QueryNode node;
    memset(&node, 0, sizeof node);
    node.op = GDEFSTOR_QUERY_IN;
    node.left.kind = GDEFSTOR_QUERY_COLUMN;
    node.left.column = "k";
    node.left.column_len = 1;
    node.right.kind = GDEFSTOR_QUERY_CONST;
    node.right.texts = texts.data();
    node.right.text_count = texts.size();
    GDEFSTOR_Query q;
    memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.where = &node;
    q.entity_numeric = 1;
    GDEFSTOR_QueryResult * r = NULL;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, "t", 1, "rows", 4, &q, &r));
    std::string out;
    for (size_t i = 0; i < gdefstor_query_result_rows(r); i++) {
      const char * e = NULL;
      size_t n = 0;
      gdefstor_query_result_entity(r, i, &e, &n);
      out += std::string(e, n) + ",";
    }
    *total = gdefstor_query_result_total(r);
    gdefstor_query_result_free(r);
    return out;
  }
};

TEST_F(QueryDir, PushedDownIsUsed) {
  Fill(30);
  uint64_t total = 0;
  EXPECT_EQ("1,11,21,", Find({"0"}, &total));
  EXPECT_EQ(3u, total);
  /* One query is one statement for the page and one for the total, run by the
   * backend and not by the port's scan. */
  EXPECT_EQ(1, gdefssqlite_query_native);
  EXPECT_EQ(0, gdefssqlite_query_fallback);
}

TEST_F(QueryDir, ATreeTooWideForOneStatementIsScannedByThePort) {
  Fill(30);
  /* GDEFSTOR_QUERY_MAX_TEXTS caps one operand, so the constants are spread
   * over many leaves. */
  std::vector<GDEFSTOR_QueryText> texts;
  std::vector<std::string> keep;
  for (int i = 0; i < 200; i++) {
    keep.push_back("z" + std::to_string(i));
  }
  for (const auto & k : keep) {
    GDEFSTOR_QueryText t;
    t.bytes = (const uint8_t *)k.data();
    t.len = k.size();
    texts.push_back(t);
  }
  std::vector<GDEFSTOR_QueryNode> leaves(170);
  std::vector<const GDEFSTOR_QueryNode *> ptrs;
  for (auto & leaf : leaves) {
    memset(&leaf, 0, sizeof leaf);
    leaf.op = GDEFSTOR_QUERY_IN;
    leaf.left.kind = GDEFSTOR_QUERY_COLUMN;
    leaf.left.column = "k";
    leaf.left.column_len = 1;
    leaf.right.kind = GDEFSTOR_QUERY_CONST;
    leaf.right.texts = texts.data();
    leaf.right.text_count = texts.size();
    ptrs.push_back(&leaf);
  }
  /* The last leaf is the one that matches. */
  GDEFSTOR_QueryText zero;
  zero.bytes = (const uint8_t *)"0";
  zero.len = 1;
  std::vector<GDEFSTOR_QueryText> hit = {zero};
  leaves.back().right.texts = hit.data();
  leaves.back().right.text_count = 1;
  GDEFSTOR_QueryNode root;
  memset(&root, 0, sizeof root);
  root.op = GDEFSTOR_QUERY_OR;
  root.children = ptrs.data();
  root.child_count = ptrs.size();
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.where = &root;
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, "t", 1, "rows", 4, &q, &r));
  EXPECT_EQ(3u, gdefstor_query_result_total(r));
  EXPECT_EQ(3u, gdefstor_query_result_rows(r));
  gdefstor_query_result_free(r);
  /* 169 leaves of 200 constants and 7 bound values each is over the 32766 a
   * connection binds by default; the backend said so and the port scanned. */
  EXPECT_EQ(0, gdefssqlite_query_native);
  EXPECT_EQ(1, gdefssqlite_query_fallback);
}

TEST_F(QueryDir, TheRangeOfATypeIsAnIndexSearchOnTheKey) {
  Fill(5);
  /* The statement's range, run apart from the functions the rest of it needs:
   * the engine searches the unique index on the key (the primary key) for the
   * type's keys, and does not scan the table. */
  std::string path = dir + "/74.db";
  sqlite3 * db = NULL;
  ASSERT_EQ(SQLITE_OK, sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, NULL));
  sqlite3_stmt * st = NULL;
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
      "EXPLAIN QUERY PLAN SELECT r.key, r.bytes FROM defiant_record r "
      "WHERE r.key >= ? AND r.key < ?", -1, &st, NULL));
  ASSERT_EQ(SQLITE_OK, sqlite3_bind_text(st, 1, "p:72", -1, SQLITE_STATIC));
  ASSERT_EQ(SQLITE_OK, sqlite3_bind_text(st, 2, "p:73", -1, SQLITE_STATIC));
  std::string plan;
  while (sqlite3_step(st) == SQLITE_ROW) {
    plan += (const char *)sqlite3_column_text(st, 3);
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  EXPECT_NE(std::string::npos, plan.find("SEARCH")) << plan;
  EXPECT_NE(std::string::npos, plan.find("key>? AND key<?")) << plan;
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
