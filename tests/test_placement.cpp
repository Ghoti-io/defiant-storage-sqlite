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
 * Tenant files, busy, and a write that the disk refuses.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include "tmpdir.h"
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TmpDir {
public:
  TmpDir() {
    path_ = MakeTempDir("gdefssqlite-place-");
  }
  ~TmpDir() {
    if (!path_.empty()) {
      chmod(path_.c_str(), 0700);
      std::filesystem::remove_all(path_);
    }
  }
  const std::string & path() const { return path_; }
  std::string file(const std::string & name) const { return path_ + "/" + name; }

private:
  std::string path_;
};

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

GDEFSTOR_Result put_blob(GDEFSTOR_Store * store, const char * tenant,
    const char * hash, const uint8_t * bytes, size_t len) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Result rc = gdefstor_write_begin(store, tenant, strlen(tenant), NULL,
      &write);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_blob_put(write, hash, strlen(hash), bytes, len);
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(write);
    return rc;
  }
  return gdefstor_write_commit(write);
}

} /* namespace */

TEST(Open, BadDirectoryWritesNoStore) {
  GDEFSTOR_Store * store = reinterpret_cast<GDEFSTOR_Store *>(1);
  EXPECT_EQ(GDEFSSQLITE_ERR_INVALID,
      gdefssqlite_store_create(NULL, NULL, NULL, &store));
  EXPECT_EQ(store, reinterpret_cast<GDEFSTOR_Store *>(1));
  store = reinterpret_cast<GDEFSTOR_Store *>(1);
  EXPECT_EQ(GDEFSSQLITE_ERR_INVALID,
      gdefssqlite_store_create("", NULL, NULL, &store));
  EXPECT_EQ(store, reinterpret_cast<GDEFSTOR_Store *>(1));
}

TEST(Tenants, EachReadSeesOnlyItsTenant) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  GDEFSTOR_Store * store = NULL;
  ASSERT_EQ(GDEFSSQLITE_OK,
      gdefssqlite_store_create(dir.path().c_str(), NULL, NULL, &store));
  const uint8_t a[] = {1, 2};
  const uint8_t b[] = {3};
  ASSERT_EQ(GDEFSTOR_OK, put_blob(store, "A", "k", a, sizeof a));
  ASSERT_EQ(GDEFSTOR_OK, put_blob(store, "A", "only-a", a, sizeof a));
  ASSERT_EQ(GDEFSTOR_OK, put_blob(store, "B", "k", b, sizeof b));
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blob_get(store, "A", 1, "k", 1, &bytes, &len));
  ASSERT_EQ(len, sizeof a);
  EXPECT_EQ(0, memcmp(bytes, a, sizeof a));
  gdefstor_bytes_free(NULL, bytes);
  bytes = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blob_get(store, "B", 1, "k", 1, &bytes, &len));
  ASSERT_EQ(len, sizeof b);
  EXPECT_EQ(0, memcmp(bytes, b, sizeof b));
  gdefstor_bytes_free(NULL, bytes);
  bytes = NULL;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_blob_get(store, "B", 1, "only-a", 6, &bytes, &len));
  EXPECT_EQ(bytes, nullptr);
  gdefstor_store_destroy(store);
  EXPECT_TRUE(std::filesystem::exists(dir.file(hex_name("A"))));
  EXPECT_TRUE(std::filesystem::exists(dir.file(hex_name("B"))));
}

TEST(FileName, SlashIsHexNotAPath) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  GDEFSTOR_Store * store = NULL;
  ASSERT_EQ(GDEFSSQLITE_OK,
      gdefssqlite_store_create(dir.path().c_str(), NULL, NULL, &store));
  const uint8_t body[] = {9};
  ASSERT_EQ(GDEFSTOR_OK, put_blob(store, "a/b", "k", body, sizeof body));
  gdefstor_store_destroy(store);
  EXPECT_TRUE(std::filesystem::exists(dir.file("612f62.db")));
  EXPECT_FALSE(std::filesystem::exists(dir.file("a")));
  EXPECT_FALSE(std::filesystem::exists(dir.file("a/b")));
  EXPECT_FALSE(std::filesystem::exists(dir.file("a/b.db")));
}

TEST(Busy, SqliteBusyDoesNotWait) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  GDEFSTOR_Store * store = NULL;
  ASSERT_EQ(GDEFSSQLITE_OK,
      gdefssqlite_store_create(dir.path().c_str(), NULL, NULL, &store));
  const uint8_t first[] = {1};
  ASSERT_EQ(GDEFSTOR_OK, put_blob(store, "t", "k", first, sizeof first));
  sqlite3 * lock = NULL;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(dir.file(hex_name("t")).c_str(), &lock));
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(lock, "BEGIN EXCLUSIVE", NULL, NULL, NULL));
  const uint8_t second[] = {2};
  auto start = std::chrono::steady_clock::now();
  GDEFSTOR_Result rc = put_blob(store, "t", "k2", second, sizeof second);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(GDEFSTOR_ERR_BUSY, rc);
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
  sqlite3_exec(lock, "ROLLBACK", NULL, NULL, NULL);
  sqlite3_close(lock);
  uint8_t * bytes = NULL;
  size_t len = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_blob_get(store, "t", 1, "k2", 2, &bytes, &len));
  gdefstor_store_destroy(store);
}

TEST(Limits, StoreCapAppliesWhenWriteLimitsAreNull) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  GDEFSTOR_Limits limits;
  gdefstor_limits_default(&limits);
  limits.max_blob_bytes = 1;
  GDEFSTOR_Store * store = NULL;
  ASSERT_EQ(GDEFSSQLITE_OK,
      gdefssqlite_store_create(dir.path().c_str(), NULL, &limits, &store));
  GDEFSTOR_Write * write = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, "capped", 6, NULL, &write));
  const uint8_t two[] = {1, 2};
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_blob_put(write, "k", 1, two, sizeof two));
  gdefstor_write_abort(write);
  uint8_t * bytes = NULL;
  size_t len = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_blob_get(store, "absent", 6, "k", 1, &bytes, &len));
  EXPECT_EQ(bytes, nullptr);
  EXPECT_FALSE(std::filesystem::exists(dir.file(hex_name("absent"))));
  gdefstor_store_destroy(store);
}

TEST(Io, DiskFailureOnPutIsNotCommitted) {
#ifdef _WIN32
  GTEST_SKIP() << "a read-only directory does not stop a Windows process creating files in it";
#endif
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  GDEFSTOR_Store * store = NULL;
  ASSERT_EQ(GDEFSSQLITE_OK,
      gdefssqlite_store_create(dir.path().c_str(), NULL, NULL, &store));
  const uint8_t oldb[] = {4, 5};
  ASSERT_EQ(GDEFSTOR_OK, put_blob(store, "t", "old", oldb, sizeof oldb));
  ASSERT_EQ(0, chmod(dir.path().c_str(), 0555));
  const uint8_t fresh[] = {6};
  GDEFSTOR_Result rc = put_blob(store, "t", "new", fresh, sizeof fresh);
  ASSERT_EQ(0, chmod(dir.path().c_str(), 0700));
  EXPECT_EQ(GDEFSTOR_ERR_IO, rc);
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_get(store, "t", 1, "old", 3, &bytes, &len));
  ASSERT_EQ(len, sizeof oldb);
  EXPECT_EQ(0, memcmp(bytes, oldb, sizeof oldb));
  gdefstor_bytes_free(NULL, bytes);
  bytes = NULL;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_blob_get(store, "t", 1, "new", 3, &bytes, &len));
  gdefstor_store_destroy(store);
}

extern "C" int gdefssqlite_plant_fail_put;

TEST(Commit, FailedUpdateKeepsPreviousBytes) {
  TmpDir dir;
  ASSERT_FALSE(dir.path().empty());
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Column column;
  GDEFSTOR_Projection * row = NULL;
  GDEFSTOR_Column got;
  const uint8_t blob[] = {9};
  ASSERT_EQ(GDEFSSQLITE_OK,
      gdefssqlite_store_create(dir.path().c_str(), NULL, NULL, &store));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, "t", 1, NULL, &write));
  memset(&column, 0, sizeof column);
  column.name = "title";
  column.name_len = 5;
  column.value = (const uint8_t *)"Hello";
  column.value_len = 5;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_put(write, "page", 4, "e1", 2, &column, 1));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  gdefssqlite_plant_fail_put = 2;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, "t", 1, NULL, &write));
  column.value = (const uint8_t *)"There";
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_put(write, "page", 4, "e1", 2, &column, 1));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_put(write, "ab", 2, blob, sizeof blob));
  EXPECT_EQ(GDEFSTOR_ERR_IO, gdefstor_write_commit(write));
  gdefssqlite_plant_fail_put = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_get(store, "t", 1, "page", 4, "e1", 2, &row));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_column(row, 0, &got));
  ASSERT_EQ(got.value_len, 5u);
  EXPECT_EQ(0, memcmp(got.value, "Hello", 5));
  gdefstor_projection_free(row);
  {
    uint8_t * bytes = NULL;
    size_t len = 0;
    EXPECT_EQ(GDEFSTOR_ERR_INVALID,
        gdefstor_blob_get(store, "t", 1, "ab", 2, &bytes, &len));
  }
  gdefstor_store_destroy(store);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
