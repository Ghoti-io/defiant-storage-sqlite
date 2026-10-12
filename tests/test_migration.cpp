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
 * Schema version 1 and the record_v0 corpus. The known blob hash is the two
 * bytes "ab", which the port stores under the key b:6162.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include "tmpdir.h"
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef GDEFSSQLITE_TEST_CORPUS
#error GDEFSSQLITE_TEST_CORPUS is set by the Makefile
#endif

namespace {

const char kTenant[] = "t";
const char kHash[] = "ab";
const uint8_t kV0[] = {0x10, 0x20, 0x30};
const uint8_t kV1[] = {0x11, 0x22};

std::string hex_name(const std::string & tenant) {
  static const char * hex = "0123456789abcdef";
  std::string out;
  for (unsigned char c : tenant) {
    out.push_back(hex[c >> 4]);
    out.push_back(hex[c & 0x0f]);
  }
  out += ".db";
  return out;
}

class TmpDir {
public:
  TmpDir() {
    path_ = MakeTempDir("gdefssqlite-mig-");
  }
  ~TmpDir() {
    if (!path_.empty()) {
      std::filesystem::remove_all(path_);
    }
  }
  const std::string & path() const { return path_; }
  std::string file(const std::string & name) const { return path_ + "/" + name; }

private:
  std::string path_;
};

GDEFSTOR_Store * open_store(const std::string & dir) {
  GDEFSTOR_Store * store = NULL;
  EXPECT_EQ(GDEFSSQLITE_OK,
      gdefssqlite_store_create(dir.c_str(), NULL, NULL, &store));
  return store;
}

int query_int(sqlite3 * db, const char * sql) {
  sqlite3_stmt * st = NULL;
  int value = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    return -1;
  }
  if (sqlite3_step(st) == SQLITE_ROW) {
    value = sqlite3_column_int(st, 0);
  }
  sqlite3_finalize(st);
  return value;
}

} /* namespace */

TEST(Migration, OldRowSurvives) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  std::filesystem::copy_file(std::string(GDEFSSQLITE_TEST_CORPUS) + "/record_v0.db",
      dir.file(hex_name(kTenant)));
  GDEFSTOR_Store * store = open_store(dir.path());
  ASSERT_NE(store, nullptr);
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_get(store, kTenant, 1, kHash, 2, &bytes, &len));
  ASSERT_EQ(len, sizeof kV0);
  EXPECT_EQ(0, memcmp(bytes, kV0, sizeof kV0));
  gdefstor_bytes_free(NULL, bytes);
  gdefstor_store_destroy(store);

  sqlite3 * db = NULL;
  ASSERT_EQ(SQLITE_OK, sqlite3_open_v2(dir.file(hex_name(kTenant)).c_str(), &db,
                            SQLITE_OPEN_READONLY, NULL));
  EXPECT_EQ(1, query_int(db, "SELECT version FROM defiant_migration"));
  EXPECT_EQ(0, query_int(db,
                  "SELECT COUNT(*) FROM sqlite_master WHERE name = 'record_v0'"));
  sqlite3_close(db);
}

TEST(Migration, VersionOneIsNotRewritten) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  std::string db_path = dir.file(hex_name(kTenant));
  std::filesystem::copy_file(std::string(GDEFSSQLITE_TEST_CORPUS) + "/version_1.db",
      db_path);
  sqlite3 * db = NULL;
  ASSERT_EQ(SQLITE_OK,
      sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, NULL));
  int rowid = query_int(db, "SELECT rowid FROM defiant_record WHERE key = 'b:6162'");
  sqlite3_close(db);
  ASSERT_GT(rowid, 0);

  GDEFSTOR_Store * store = open_store(dir.path());
  ASSERT_NE(store, nullptr);
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_get(store, kTenant, 1, kHash, 2, &bytes, &len));
  ASSERT_EQ(len, sizeof kV1);
  EXPECT_EQ(0, memcmp(bytes, kV1, sizeof kV1));
  gdefstor_bytes_free(NULL, bytes);
  gdefstor_store_destroy(store);

  ASSERT_EQ(SQLITE_OK,
      sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, NULL));
  EXPECT_EQ(rowid,
      query_int(db, "SELECT rowid FROM defiant_record WHERE key = 'b:6162'"));
  EXPECT_EQ(1, query_int(db, "SELECT COUNT(*) FROM defiant_record"));
  EXPECT_EQ(1, query_int(db, "SELECT version FROM defiant_migration"));
  sqlite3_close(db);
}

TEST(Migration, FailedStepLeavesVersionUnrecorded) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  std::string keep = dir.file("keep.txt");
  {
    std::ofstream out(keep);
    out << "leave-this";
  }
  std::string db_path = dir.file(hex_name(kTenant));
  sqlite3 * db = NULL;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(db_path.c_str(), &db));
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(db,
      "CREATE TABLE record_v0 (key TEXT PRIMARY KEY, bytes BLOB NOT NULL);"
      "INSERT INTO record_v0 (key, bytes) VALUES ('b:6162', X'102030');"
      "CREATE TABLE defiant_migration (version INTEGER PRIMARY KEY);"
      "CREATE TRIGGER abort_version BEFORE INSERT ON defiant_migration "
      "BEGIN SELECT RAISE(ABORT, 'fail version'); END;",
      NULL, NULL, NULL));
  sqlite3_close(db);

  GDEFSTOR_Store * store = open_store(dir.path());
  ASSERT_NE(store, nullptr);
  uint8_t * bytes = NULL;
  size_t len = 0;
  EXPECT_NE(GDEFSTOR_OK,
      gdefstor_blob_get(store, kTenant, 1, kHash, 2, &bytes, &len));
  EXPECT_EQ(bytes, nullptr);
  gdefstor_store_destroy(store);

  EXPECT_TRUE(std::filesystem::exists(keep));
  EXPECT_TRUE(std::filesystem::exists(db_path));
  ASSERT_EQ(SQLITE_OK,
      sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, NULL));
  EXPECT_EQ(1, query_int(db,
                  "SELECT COUNT(*) FROM record_v0 WHERE key = 'b:6162' "
                  "AND bytes = X'102030'"));
  EXPECT_EQ(0, query_int(db,
                  "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                  "AND name = 'defiant_record'"));
  EXPECT_EQ(0, query_int(db, "SELECT COUNT(*) FROM defiant_migration"));
  sqlite3_close(db);
}

TEST(Open, EmptyFileIsSchemaVersionOne) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  std::string db_path = dir.file(hex_name(kTenant));
  {
    std::ofstream empty(db_path, std::ios::binary);
  }
  GDEFSTOR_Store * store = open_store(dir.path());
  ASSERT_NE(store, nullptr);
  uint8_t * bytes = NULL;
  size_t len = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_blob_get(store, kTenant, 1, kHash, 2, &bytes, &len));
  gdefstor_store_destroy(store);

  sqlite3 * db = NULL;
  ASSERT_EQ(SQLITE_OK,
      sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, NULL));
  EXPECT_EQ(1, query_int(db, "SELECT version FROM defiant_migration"));
  EXPECT_EQ(0, query_int(db, "SELECT COUNT(*) FROM defiant_record"));
  sqlite3_close(db);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
