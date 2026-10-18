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
 * What only the SQLite files and connections can show (model.h): old and
 * newer files refused and left byte for byte as they were; every table,
 * column and index in a file a recorded schema step; reads on connections of
 * their own while a write is open; a begin that waits for another process's
 * write; a write killed part way leaving the last commit; the statement
 * cache; and the key store's file, where a destroyed key leaves no bytes.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <sqlite3.h>

#include "model_util.h"
#include "tmpdir.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace tu;

namespace {

/* "t1" as the file name sqlite.h and model.h both give it. */
const char * const kFile = "7431.db";
const char * const kKeyFile = "7431.keys.db";

struct Dir {
  std::string path = MakeTempDir("gdefssqlite-files-");
  ~Dir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string At(const std::string & name) const {
    return path + "/" + name;
  }
};

std::string ReadAll(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
      std::istreambuf_iterator<char>());
}

std::set<std::string> Listing(const std::string & dir) {
  std::set<std::string> out;
  std::error_code ec;
  for (const auto & e : std::filesystem::directory_iterator(dir, ec)) {
    out.insert(e.path().filename().string());
  }
  return out;
}

/* Everything one store did, statement by statement. */
struct Seen {
  struct Rec {
    std::string sql;
    unsigned conn = 0;
    int write = 0;
    int exec = 0;
    int cached = 0;
    std::string phase;
  };
  std::vector<Rec> recs;
  std::string phase = "setup";
  std::vector<std::string> refusals;
  std::vector<int> refusal_codes;
};

void OnStatement(void * ctx, const GDEFSSQLITE_StatementStats * s) {
  Seen * seen = static_cast<Seen *>(ctx);
  Seen::Rec r;
  r.sql.assign(s->sql, s->sql_len);
  r.conn = s->connection;
  r.write = s->write;
  r.exec = s->exec;
  r.cached = s->cached;
  r.phase = seen->phase;
  seen->recs.push_back(r);
}

void OnRefusal(void * ctx, int result, const char * text, size_t len) {
  Seen * seen = static_cast<Seen *>(ctx);
  seen->refusals.emplace_back(text, len);
  seen->refusal_codes.push_back(result);
}

GDEFSSQLITE_ModelOptions Options(Seen * seen) {
  GDEFSSQLITE_ModelOptions o;
  gdefssqlite_model_options_default(&o);
  o.on_statement = OnStatement;
  o.on_statement_ctx = seen;
  o.on_refusal = OnRefusal;
  o.on_refusal_ctx = seen;
  return o;
}

GDEFSTOR_Store * Open(const Dir & d, Seen * seen,
    const GDEFSSQLITE_ModelOptions * opts = nullptr) {
  GDEFSSQLITE_ModelOptions o = Options(seen);
  GDEFSTOR_Store * store = nullptr;
  EXPECT_EQ(GDEFSSQLITE_OK, gdefssqlite_model_store_create(d.path.c_str(),
      opts != nullptr ? opts : &o, nullptr, nullptr, &store));
  return store;
}

/* ---- raw SQLite, for looking at a file from outside ---------------------- */

struct Raw {
  sqlite3 * db = nullptr;
  explicit Raw(const std::string & path, bool create = false) {
    EXPECT_EQ(SQLITE_OK, sqlite3_open_v2(path.c_str(), &db,
        SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0), nullptr));
  }
  ~Raw() {
    sqlite3_close_v2(db);
  }
  std::vector<std::vector<std::string>> Rows(const std::string & sql) {
    std::vector<std::vector<std::string>> out;
    sqlite3_stmt * st = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr))
        << sqlite3_errmsg(db) << " in " << sql;
    while (st != nullptr && sqlite3_step(st) == SQLITE_ROW) {
      std::vector<std::string> row;
      for (int i = 0; i < sqlite3_column_count(st); i++) {
        const unsigned char * t = sqlite3_column_text(st, i);
        row.push_back(t == nullptr ? std::string() : std::string((const char *)t));
      }
      out.push_back(row);
    }
    sqlite3_finalize(st);
    return out;
  }
  void Exec(const std::string & sql) {
    char * err = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err))
        << (err != nullptr ? err : "") << " in " << sql;
    sqlite3_free(err);
  }
};

std::vector<F> Article() {
  return {Field("title", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED),
      Field("rank", GDEFSTOR_FIELD_INTEGER),
      Field("tags", GDEFSTOR_FIELD_TEXT,
          GDEFSTOR_FIELD_MULTIPLE | GDEFSTOR_FIELD_INDEXED),
      Group("parts", GDEFSTOR_FIELD_MULTIPLE,
          {Field("heading", GDEFSTOR_FIELD_TEXT),
              Group("notes", GDEFSTOR_FIELD_MULTIPLE,
                  {Field("body", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED),
                      Field("score", GDEFSTOR_FIELD_DECIMAL, 0, 2)})})};
}

Node Titled(const std::string & title) {
  Node n;
  n.Set("title", Txt(title));
  n.Set("rank", Num(GDEFSTOR_FIELD_INTEGER, 3));
  n.Item("tags", Txt("a"));
  Node & p = n.Add("parts");
  p.Set("heading", Txt("h"));
  p.Add("notes").Set("body", Txt("b"));
  return n;
}

std::string TitleOf(GDEFSTOR_Store * store, const char * pid,
    GDEFSTOR_Result * rc_out = nullptr) {
  GDEFSTOR_Tree * t = nullptr;
  GDEFSTOR_Result rc = gdefstor_entity_tree(store, kTenant, 2, pid,
      std::strlen(pid), 0, &t);
  if (rc_out != nullptr) {
    *rc_out = rc;
  }
  if (rc != GDEFSTOR_OK) {
    return "<" + std::string(gdefstor_result_string(rc)) + ">";
  }
  GDEFSTOR_Value v;
  gdefstor_tree_get(t, 0, "title", 5, &v);
  std::string s(reinterpret_cast<const char *>(v.bytes), v.len);
  gdefstor_tree_free(t);
  return s;
}

GDEFSTOR_Result CountTitled(GDEFSTOR_Store * store, const std::string & title,
    uint64_t * out) {
  GDEFSTOR_Value v;
  std::memset(&v, 0, sizeof v);
  v.type = GDEFSTOR_FIELD_TEXT;
  v.bytes = reinterpret_cast<const uint8_t *>(title.data());
  v.len = title.size();
  GDEFSTOR_QueryNode n;
  std::memset(&n, 0, sizeof n);
  n.op = GDEFSTOR_QUERY_EQ;
  n.left.kind = GDEFSTOR_QUERY_FIELD;
  n.left.field = "title";
  n.left.field_len = 5;
  n.right.kind = GDEFSTOR_QUERY_VALUES;
  n.right.values = &v;
  n.right.value_count = 1;
  return gdefstor_entity_count(store, kTenant, 2, "site.article", 12, &n, out);
}

/* ---- old and newer files --------------------------------------------------- */

/* Every way the v2 store can reach a tenant's file: a read of an entity, a
 * count, a declaration in a write. */
void ExpectEveryPathRefused(GDEFSTOR_Store * store, GDEFSTOR_Result want) {
  GDEFSTOR_Entity * e = nullptr;
  EXPECT_EQ(want, gdefstor_entity_get(store, kTenant, 2, "e1", 2, &e));
  EXPECT_EQ(nullptr, e);
  GDEFSTOR_Type * type = nullptr;
  EXPECT_EQ(want, gdefstor_type_get(store, kTenant, 2, "site.article", 12,
      &type));
  gdefstor_type_free(type);
  EXPECT_EQ(want, Declare(store, "site.article", Article()));
}

/* A file the milestone-1 API made (defiant_record, defiant_migration, no
 * dss_schema) is refused by the v2 open on every path, with a sentence that
 * names the cause, and is left exactly as it was: no table added, no WAL
 * switched on, no file beside it. The old API still reads it. */
TEST(ModelFiles, MilestoneOneFileIsRefusedAndLeftAsItIs) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  GDEFSTOR_Store * old = nullptr;
  ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_store_create(d.path.c_str(), nullptr,
      nullptr, &old));
  const uint8_t blob[] = {1, 2, 3};
  ASSERT_EQ(GDEFSTOR_OK, InWrite(old, [&](GDEFSTOR_Write * w) {
    return gdefstor_blob_put(w, "ab", 2, blob, sizeof blob);
  }));
  gdefstor_store_destroy(old);
  const std::string before = ReadAll(d.At(kFile));
  const std::set<std::string> listed = Listing(d.path);
  ASSERT_FALSE(before.empty());

  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  ASSERT_NE(nullptr, store);
  ExpectEveryPathRefused(store, GDEFSTOR_ERR_FORMAT);
  gdefstor_store_destroy(store);

  EXPECT_TRUE(before == ReadAll(d.At(kFile))) << "the refused file was changed";
  EXPECT_EQ(listed, Listing(d.path)) << "a file was made beside it";
  ASSERT_FALSE(seen.refusals.empty());
  for (size_t i = 0; i < seen.refusals.size(); i++) {
    EXPECT_NE(std::string::npos, seen.refusals[i].find("milestone-1"))
        << seen.refusals[i];
    EXPECT_NE(std::string::npos, seen.refusals[i].find("defiant_record"))
        << seen.refusals[i];
    EXPECT_NE(std::string::npos, seen.refusals[i].find(d.At(kFile)))
        << seen.refusals[i];
    EXPECT_EQ(GDEFSTOR_ERR_FORMAT, seen.refusal_codes[i]);
  }
  for (const auto & r : seen.recs) {
    EXPECT_NE(0, r.exec) << "a statement ran on a refused file: " << r.sql;
  }

  ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_store_create(d.path.c_str(), nullptr,
      nullptr, &old));
  uint8_t * bytes = nullptr;
  size_t len = 0;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_blob_get(old, kTenant, 2, "ab", 2, &bytes,
      &len));
  EXPECT_EQ(sizeof blob, len);
  gdefstor_bytes_free(nullptr, bytes);
  gdefstor_store_destroy(old);
}

/* The same for the committed milestone-1 fixture. */
TEST(ModelFiles, TheMilestoneOneFixtureIsRefusedAndLeftAsItIs) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  std::filesystem::copy_file(
      std::string(GDEFSSQLITE_TEST_CORPUS) + "/version_1.db", d.At(kFile));
  const std::string before = ReadAll(d.At(kFile));
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  ASSERT_NE(nullptr, store);
  ExpectEveryPathRefused(store, GDEFSTOR_ERR_FORMAT);
  gdefstor_store_destroy(store);
  EXPECT_TRUE(before == ReadAll(d.At(kFile)));
  EXPECT_EQ(std::set<std::string>({kFile}), Listing(d.path));
  EXPECT_FALSE(seen.refusals.empty());
}

/* A store whose dss_schema says a later layout is refused as unsupported, on
 * reads as well as writes, and left as it was. */
TEST(ModelFiles, ANewerLayoutIsRefusedAndLeftAsItIs) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  {
    Seen seen;
    GDEFSTOR_Store * store = Open(d, &seen);
    ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article()));
    ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", Titled("one"), 0,
        true));
    gdefstor_store_destroy(store);
  }
  {
    Raw raw(d.At(kFile));
    raw.Exec("UPDATE dss_schema SET version = 2");
  }
  ASSERT_FALSE(std::filesystem::exists(d.At(std::string(kFile) + "-wal")));
  const std::string before = ReadAll(d.At(kFile));
  const std::set<std::string> listed = Listing(d.path);
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  ExpectEveryPathRefused(store, GDEFSTOR_ERR_UNSUPPORTED);
  uint64_t n = 9;
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, CountTitled(store, "one", &n));
  gdefstor_store_destroy(store);
  EXPECT_TRUE(before == ReadAll(d.At(kFile)));
  EXPECT_EQ(listed, Listing(d.path));
  ASSERT_FALSE(seen.refusals.empty());
  EXPECT_NE(std::string::npos, seen.refusals[0].find("version 2"))
      << seen.refusals[0];
}

/* A file with tables of someone else's is not taken over either. */
TEST(ModelFiles, AFileOfOtherTablesIsRefused) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  {
    Raw raw(d.At(kFile), true);
    raw.Exec("CREATE TABLE notes (x TEXT)");
  }
  const std::string before = ReadAll(d.At(kFile));
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  ExpectEveryPathRefused(store, GDEFSTOR_ERR_FORMAT);
  gdefstor_store_destroy(store);
  EXPECT_TRUE(before == ReadAll(d.At(kFile)));
}

/* ---- the schema in the file is the recorded steps -------------------------- */

/* Every table, column and index in a tenant's file is either one of the six
 * tenant tables (and their two indexes) or a step recorded in dss_name, and
 * every recorded step is in the file. DDL runs only in the writes that
 * declare a type. */
TEST(ModelFiles, EveryTableAndIndexIsARecordedStep) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  seen.phase = "declare";
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article()));
  F who = Field("email", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_PERSONAL);
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.person",
      {Field("name", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED), who}));
  seen.phase = "work";
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", Titled("one"), 0,
      true));
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e2", Titled("two"), 0,
      true));
  ASSERT_EQ(GDEFSTOR_OK, State(store, "e1", 1, 0));
  seen.phase = "declare";
  std::vector<F> more = Article();
  more.push_back(Field("summary", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED));
  more[1].flags = GDEFSTOR_FIELD_INDEXED; /* rank becomes indexed */
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", more));
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", more)); /* no step */
  seen.phase = "work";
  Node t = Titled("three");
  t.Set("summary", Txt("s"));
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", t, 1, false));
  uint64_t n = 0;
  ASSERT_EQ(GDEFSTOR_OK, CountTitled(store, "three", &n));
  EXPECT_EQ(1u, n);
  ASSERT_EQ(GDEFSTOR_OK, InWrite(store, [](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  gdefstor_store_destroy(store);

  for (const auto & r : seen.recs) {
    bool ddl = r.sql.rfind("CREATE ", 0) == 0 || r.sql.rfind("ALTER ", 0) == 0;
    if (ddl) {
      EXPECT_EQ("declare", r.phase) << "DDL outside a declaration: " << r.sql;
    }
  }

  Raw raw(d.At(kFile));
  std::set<std::string> in_file;
  for (const auto & row : raw.Rows("SELECT name FROM sqlite_schema WHERE "
                                   "type IN ('table', 'index') AND name NOT "
                                   "LIKE 'sqlite\\_%' ESCAPE '\\'")) {
    in_file.insert(row[0]);
  }
  std::set<std::string> recorded = {"dss_schema", "dss_counter", "dss_type",
      "dss_name", "dss_entity", "dss_draft", "dss_type_tno", "dss_entity_rid"};
  std::set<std::string> columns_recorded;
  for (const auto & row : raw.Rows("SELECT tno, grp, role, name FROM dss_name")) {
    int role = std::stoi(row[2]);
    if (role == 7 || role == 8) {
      columns_recorded.insert(row[0] + "/" + row[1] + "/" + row[3]);
      continue;
    }
    recorded.insert(row[3]);
    if (role == 1) {
      recorded.insert("t" + row[0] + "p"); /* made with the entity table */
    }
  }
  EXPECT_EQ(recorded, in_file);
  /* Each group table's and current table's columns past the fixed ones. */
  std::set<std::string> fixed = {"ent", "item", "vfrom", "vto", "parent", "ord"};
  int checked = 0;
  for (const std::string & table : in_file) {
    size_t u = table.find('_');
    if (table[0] != 't' || u == std::string::npos ||
        (table[u + 1] != 'g' && table[u + 1] != 'c')) {
      continue;
    }
    std::string tno = table.substr(1, u - 1);
    std::string grp = table.substr(u + 2);
    for (const auto & col : raw.Rows("PRAGMA table_info(\"" + table + "\")")) {
      if (fixed.count(col[1]) != 0) {
        continue;
      }
      EXPECT_EQ(1u, columns_recorded.count(tno + "/" + grp + "/" + col[1]))
          << table << "." << col[1] << " is no recorded step";
      checked++;
    }
  }
  EXPECT_GT(checked, 10);
}

/* ---- connections ------------------------------------------------------------ */

/* While a write is open and holds a change, every read runs on a read
 * connection of its own and sees the last commit; the write's connection
 * reads only for the write. */
TEST(ModelConcurrency, ReadersUseTheirOwnConnections) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article()));
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", Titled("one"), 0,
      true));
  ASSERT_EQ("one", TitleOf(store, "e1"));
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  GDEFSTOR_Tree * t = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_create(nullptr, &t));
  ASSERT_EQ(GDEFSTOR_OK, Fill(t, 0, Titled("two")));
  GDEFSTOR_RevisionMeta m = Meta();
  seen.phase = "write";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_save(w, "e1", 2, 1, &m, t, nullptr));
  gdefstor_tree_free(t);

  seen.phase = "read";
  EXPECT_EQ("one", TitleOf(store, "e1"));
  uint64_t n = 9;
  EXPECT_EQ(GDEFSTOR_OK, CountTitled(store, "two", &n));
  EXPECT_EQ(0u, n);
  EXPECT_EQ(GDEFSTOR_OK, CountTitled(store, "one", &n));
  EXPECT_EQ(1u, n);
  seen.phase = "commit";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
  seen.phase = "after";
  EXPECT_EQ("two", TitleOf(store, "e1"));
  gdefstor_store_destroy(store);

  std::set<unsigned> writers;
  std::set<unsigned> readers;
  int reads = 0;
  for (const auto & r : seen.recs) {
    if (r.phase == "write") {
      EXPECT_EQ(1, r.write) << r.sql;
      writers.insert(r.conn);
    }
    if (r.phase == "read" || r.phase == "after") {
      EXPECT_EQ(0, r.write) << "a read ran on the write connection: " << r.sql;
      readers.insert(r.conn);
      reads++;
    }
  }
  EXPECT_GT(reads, 3);
  ASSERT_FALSE(writers.empty());
  for (unsigned c : readers) {
    EXPECT_EQ(0u, writers.count(c)) << "connection " << c << " did both";
  }
}

#ifndef _WIN32
/* A begin that finds another store (another process, for SQLite) writing
 * the tenant waits in SQLite's busy handler and goes ahead once the other
 * commits; with no wait allowed it is ERR_BUSY at once. */
TEST(ModelConcurrency, ABusyBeginWaitsForTheOtherWriter) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  Seen sa;
  Seen sb;
  Seen sc;
  GDEFSTOR_Store * a = Open(d, &sa);
  ASSERT_EQ(GDEFSTOR_OK, Declare(a, "site.article", Article()));
  GDEFSSQLITE_ModelOptions waits = Options(&sb);
  waits.busy_timeout_ms = 60000;
  GDEFSSQLITE_ModelOptions never = Options(&sc);
  never.busy_timeout_ms = 0;
  GDEFSTOR_Store * b = Open(d, &sb, &waits);
  GDEFSTOR_Store * c = Open(d, &sc, &never);

  GDEFSTOR_Write * wa = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(a, kTenant, 2, nullptr, &wa));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_create(wa, "site.article", 12, "e1",
      2, "", 0, 1, nullptr));
  /* a holds the tenant's write lock now. */
  EXPECT_EQ(GDEFSTOR_ERR_BUSY, InWrite(c, [](GDEFSTOR_Write * w) {
    return gdefstor_entity_create(w, "site.article", 12, "e3", 2, "", 0, 1,
        nullptr);
  }));
  std::atomic<int> committed{0};
  std::thread other([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    committed = 1;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_write_commit(wa));
  });
  GDEFSTOR_Result rb = InWrite(b, [](GDEFSTOR_Write * w) {
    return gdefstor_entity_create(w, "site.article", 12, "e2", 2, "", 0, 1,
        nullptr);
  });
  int seen_commit = committed.load();
  other.join();
  EXPECT_EQ(GDEFSTOR_OK, rb);
  EXPECT_EQ(1, seen_commit) << "b's write went ahead while a held the lock";
  GDEFSTOR_Entity * e = nullptr;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_get(c, kTenant, 2, "e1", 2, &e));
  gdefstor_entity_free(e);
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_get(c, kTenant, 2, "e2", 2, &e));
  gdefstor_entity_free(e);
  gdefstor_store_destroy(a);
  gdefstor_store_destroy(b);
  gdefstor_store_destroy(c);
}

/* A process killed in the middle of a write leaves the last commit: WAL
 * frames with no commit are not part of the database. */
TEST(ModelCrash, AWriteKilledPartWayLeavesThePreviousRevision) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  {
    Seen seen;
    GDEFSTOR_Store * store = Open(d, &seen);
    ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article()));
    ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", Titled("one"), 0,
        true));
    gdefstor_store_destroy(store);
  }
  pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    GDEFSSQLITE_ModelOptions o;
    gdefssqlite_model_options_default(&o);
    GDEFSTOR_Store * store = nullptr;
    if (gdefssqlite_model_store_create(d.path.c_str(), &o, nullptr, nullptr,
            &store) != GDEFSSQLITE_OK) {
      _exit(3);
    }
    GDEFSTOR_Write * w = nullptr;
    if (gdefstor_write_begin(store, kTenant, 2, nullptr, &w) != GDEFSTOR_OK) {
      _exit(4);
    }
    GDEFSTOR_Tree * t = nullptr;
    gdefstor_tree_create(nullptr, &t);
    Fill(t, 0, Titled("two"));
    GDEFSTOR_RevisionMeta m = Meta();
    if (gdefstor_entity_save(w, "e1", 2, 1, &m, t, nullptr) != GDEFSTOR_OK) {
      _exit(5);
    }
    /* Enough more that pages spill into the WAL before any commit. */
    for (int i = 0; i < 400; i++) {
      std::string pid = "x" + std::to_string(i);
      gdefstor_entity_create(w, "site.article", 12, pid.data(), pid.size(), "",
          0, 1, nullptr);
    }
    raise(SIGKILL);
    _exit(6);
  }
  int status = 0;
  ASSERT_EQ(child, waitpid(child, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status)) << "the child exited " << WEXITSTATUS(status);
  ASSERT_EQ(SIGKILL, WTERMSIG(status));
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  EXPECT_EQ("one", TitleOf(store, "e1"));
  GDEFSTOR_History * h = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_history(store, kTenant, 2, "e1", 2,
      &h));
  EXPECT_EQ(1u, gdefstor_history_revision_count(h));
  gdefstor_history_free(h);
  GDEFSTOR_Entity * e = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_get(store, kTenant, 2, "x0",
      2, &e));
  uint64_t rev = 0;
  EXPECT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", Titled("again"), 1,
      false, &rev));
  EXPECT_EQ(2u, rev);
  gdefstor_store_destroy(store);
}
#endif

/* ---- the statement cache ----------------------------------------------------- */

/* The same query asked twice runs its statements from the cache the second
 * time, and answers the same; with no cache nothing is reused and the
 * answers are the same again. */
TEST(ModelCache, RepeatedStatementsComeFromTheCache) {
  for (int cache : {64, 0}) {
    Dir d;
    ASSERT_FALSE(d.path.empty());
    Seen seen;
    GDEFSSQLITE_ModelOptions o = Options(&seen);
    o.statement_cache = (size_t)cache;
    GDEFSTOR_Store * store = Open(d, &seen, &o);
    ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article()));
    ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", Titled("one"), 0,
        true));
    uint64_t n1 = 0;
    uint64_t n2 = 0;
    /* A new connection checks its file first; that is not the query. */
    ASSERT_EQ(GDEFSTOR_OK, CountTitled(store, "one", &n1));
    seen.phase = "first";
    ASSERT_EQ(GDEFSTOR_OK, CountTitled(store, "one", &n1));
    seen.phase = "second";
    ASSERT_EQ(GDEFSTOR_OK, CountTitled(store, "one", &n2));
    gdefstor_store_destroy(store);
    EXPECT_EQ(1u, n1);
    EXPECT_EQ(n1, n2);
    int first = 0;
    int second = 0;
    for (const auto & r : seen.recs) {
      if (r.exec) {
        continue;
      }
      first += r.phase == "first";
      if (r.phase == "second") {
        second++;
        EXPECT_EQ(cache > 0 ? 1 : 0, r.cached) << r.sql;
      }
    }
    EXPECT_GT(first, 1);
    EXPECT_EQ(first, second);
  }
}

/* ---- the store's own surface ------------------------------------------------- */

/* gdefssqlite_model_store_create() serves the entity port and holds no
 * key/bytes rows: a read finds none and a write of one is refused. One file
 * per tenant, named by the tenant's hex. */
TEST(ModelFiles, TheStoreServesEntitiesInOneFilePerTenant) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article()));
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article(), "t2"));
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.article", "e1", Titled("one"), 0,
      true));
  EXPECT_EQ("one", TitleOf(store, "e1"));
  GDEFSTOR_Entity * e = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_get(store, "t2", 2, "e1", 2,
      &e));
  uint8_t * bytes = nullptr;
  size_t len = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_blob_get(store, kTenant, 2, "ab", 2,
      &bytes, &len));
  const uint8_t blob[] = {1};
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, InWrite(store, [&](GDEFSTOR_Write * w) {
    return gdefstor_blob_put(w, "ab", 2, blob, 1);
  }));
  gdefstor_store_destroy(store);
  std::set<std::string> dbs;
  for (const std::string & f : Listing(d.path)) {
    if (f.size() > 3 && f.compare(f.size() - 3, 3, ".db") == 0) {
      dbs.insert(f);
    }
  }
  EXPECT_EQ(std::set<std::string>({"7431.db", "7432.db"}), dbs);
  Raw raw(d.At(kFile));
  auto mode = raw.Rows("PRAGMA journal_mode");
  ASSERT_EQ(1u, mode.size());
  EXPECT_EQ("wal", mode[0][0]);
}

TEST(ModelFiles, ArgumentsAreChecked) {
  GDEFSTOR_Store * store = nullptr;
  EXPECT_EQ(GDEFSSQLITE_ERR_INVALID, gdefssqlite_model_store_create(nullptr,
      nullptr, nullptr, nullptr, &store));
  EXPECT_EQ(GDEFSSQLITE_ERR_INVALID, gdefssqlite_model_store_create("",
      nullptr, nullptr, nullptr, &store));
  GDEFSSQLITE_ModelOptions o;
  gdefssqlite_model_options_default(&o);
  o.size = 1;
  EXPECT_EQ(GDEFSSQLITE_ERR_INVALID, gdefssqlite_model_store_create("/tmp",
      &o, nullptr, nullptr, &store));
  EXPECT_EQ(nullptr, store);
  GDEFSSQL_Dialect dia;
  gdefssqlite_dialect(&dia);
  EXPECT_EQ(GDEFSSQL_OK, gdefssql_dialect_check(&dia));
}

/* ---- the key store's file ----------------------------------------------------- */

/* A cipher whose keys the test knows: each is 32 bytes no file holds by
 * chance. Seal is a keyed XOR with a nonce; it is NOT a cipher. */
struct KnownKeys {
  std::vector<std::string> made;
  uint64_t nonce = 0;
};

extern "C" GDEFSTOR_Result kk_random(void * ctx, uint8_t * out, size_t n) {
  KnownKeys * k = static_cast<KnownKeys *>(ctx);
  if (n == 8) {
    k->nonce++;
    for (size_t i = 0; i < 8; i++) {
      out[i] = (uint8_t)(k->nonce >> (8 * i));
    }
    return GDEFSTOR_OK;
  }
  std::string key = "KEY#" + std::to_string(k->made.size()) + "#";
  while (key.size() < n) {
    key += (char)('a' + (key.size() * 7 + k->made.size()) % 26);
  }
  key.resize(n);
  std::memcpy(out, key.data(), n);
  k->made.push_back(key);
  return GDEFSTOR_OK;
}

extern "C" GDEFSTOR_Result kk_seal(void * ctx, const GDEFSTOR_Allocator * a,
    const uint8_t * key, const uint8_t * in, size_t n, uint8_t ** out,
    size_t * out_len) {
  uint8_t * o = static_cast<uint8_t *>(gcu_allocator_malloc(a, n + 8));
  if (o == nullptr) {
    return GDEFSTOR_ERR_OOM;
  }
  kk_random(ctx, o, 8);
  for (size_t i = 0; i < n; i++) {
    o[8 + i] = (uint8_t)(in[i] ^ key[i % 32] ^ o[i % 8]);
  }
  *out = o;
  *out_len = n + 8;
  return GDEFSTOR_OK;
}

extern "C" GDEFSTOR_Result kk_open(void * ctx, const GDEFSTOR_Allocator * a,
    const uint8_t * key, const uint8_t * in, size_t n, uint8_t ** out,
    size_t * out_len) {
  (void)ctx;
  if (n < 8) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  uint8_t * o = static_cast<uint8_t *>(gcu_allocator_malloc(a, n - 8 + 1));
  if (o == nullptr) {
    return GDEFSTOR_ERR_OOM;
  }
  for (size_t i = 0; i < n - 8; i++) {
    o[i] = (uint8_t)(in[8 + i] ^ key[i % 32] ^ in[i % 8]);
  }
  *out = o;
  *out_len = n - 8;
  return GDEFSTOR_OK;
}

GDEFSTOR_KeyCipher Known(KnownKeys * k) {
  GDEFSTOR_KeyCipher c;
  std::memset(&c, 0, sizeof c);
  c.size = sizeof c;
  c.ctx = k;
  c.key_bytes = 32;
  c.random = kk_random;
  c.seal = kk_seal;
  c.open = kk_open;
  return c;
}

/* Everything of the key store's on disk: the file and its WAL. */
std::string KeyBytes(const Dir & d) {
  return ReadAll(d.At(kKeyFile)) + ReadAll(d.At(std::string(kKeyFile) + "-wal"));
}

/* Destroying a key leaves no copy of it in the key store's file or its WAL,
 * while the key of a subject not erased is still there (so the search can
 * see a key at all), and the data store's directory holds no key. */
TEST(ModelKeyStore, ADestroyedKeyLeavesNoBytesInTheFile) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  KnownKeys known;
  GDEFSTOR_KeyCipher cipher = Known(&known);
  GDEFSTOR_KeyStore * ks = nullptr;
  ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_keystore_create(d.path.c_str(),
      &cipher, nullptr, nullptr, &ks));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, kTenant, 2, "s1",
      2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, kTenant, 2, "s2",
      2));
  ASSERT_EQ(2u, known.made.size());
  const std::string k1 = known.made[0];
  const std::string k2 = known.made[1];
  std::string disk = KeyBytes(d);
  ASSERT_TRUE(std::string::npos != disk.find(k1)) << "the check cannot see a key";
  ASSERT_TRUE(std::string::npos != disk.find(k2));

  uint8_t * sealed = nullptr;
  size_t sealed_len = 0;
  int erased = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_wrap(ks, kTenant, 2, "s1", 2,
      reinterpret_cast<const uint8_t *>("hello"), 5, &erased, &sealed,
      &sealed_len));
  gdefstor_bytes_free(nullptr, sealed);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, kTenant, 2, "s1", 2, 9));
  disk = KeyBytes(d);
  EXPECT_TRUE(std::string::npos == disk.find(k1))
      << "a destroyed key's bytes are still in the key store's files";
  EXPECT_TRUE(std::string::npos != disk.find(k2));
  sealed = nullptr;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_keystore_wrap(ks, kTenant, 2, "s1", 2,
      reinterpret_cast<const uint8_t *>("hello"), 5, &erased, &sealed,
      &sealed_len));
  EXPECT_EQ(1, erased);
  EXPECT_EQ(nullptr, sealed);
  gdefstor_keystore_destroy(ks);
  disk = KeyBytes(d);
  EXPECT_TRUE(std::string::npos == disk.find(k1));
  EXPECT_EQ(std::set<std::string>({kKeyFile}), Listing(d.path));

  Raw raw(d.At(kKeyFile));
  auto sd = raw.Rows("SELECT name FROM sqlite_schema WHERE type = 'table' "
                     "AND name LIKE 'dss%'");
  EXPECT_TRUE(sd.empty()) << "a data-store table in the key store";
}

/* A key store file and a data store file are never mistaken for each other:
 * each open refuses the other's file, and leaves it as it was. */
TEST(ModelKeyStore, KeyFilesAndDataFilesAreNotConfused) {
  Dir d;
  ASSERT_FALSE(d.path.empty());
  Seen seen;
  GDEFSTOR_Store * store = Open(d, &seen);
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.article", Article()));
  gdefstor_store_destroy(store);
  KnownKeys known;
  GDEFSTOR_KeyCipher cipher = Known(&known);
  {
    GDEFSTOR_KeyStore * ks = nullptr;
    ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_keystore_create(d.path.c_str(),
        &cipher, nullptr, nullptr, &ks));
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, kTenant, 2,
        "s1", 2));
    gdefstor_keystore_destroy(ks);
  }
  /* Side by side in one directory, each kept to its own file. */
  EXPECT_EQ(1u, Listing(d.path).count(kFile));
  EXPECT_EQ(1u, Listing(d.path).count(kKeyFile));

  Dir swapped;
  ASSERT_FALSE(swapped.path.empty());
  std::filesystem::copy_file(d.At(kFile), swapped.At(kKeyFile));
  std::filesystem::copy_file(d.At(kKeyFile), swapped.At(kFile));
  const std::string data_before = ReadAll(swapped.At(kKeyFile));
  const std::string keys_before = ReadAll(swapped.At(kFile));
  Seen ks_seen;
  GDEFSSQLITE_ModelOptions o = Options(&ks_seen);
  GDEFSTOR_KeyStore * ks = nullptr;
  ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_keystore_create(swapped.path.c_str(),
      &cipher, &o, nullptr, &ks));
  EXPECT_EQ(GDEFSTOR_ERR_FORMAT, gdefstor_keystore_subject_create(ks, kTenant,
      2, "s9", 2));
  uint8_t * out = nullptr;
  size_t len = 0;
  int erased = 0;
  EXPECT_EQ(GDEFSTOR_ERR_FORMAT, gdefstor_keystore_wrap(ks, kTenant, 2, "s1",
      2, reinterpret_cast<const uint8_t *>("x"), 1, &erased, &out, &len));
  gdefstor_keystore_destroy(ks);
  ASSERT_FALSE(ks_seen.refusals.empty());
  EXPECT_NE(std::string::npos, ks_seen.refusals[0].find("is a data store"));
  Seen data_seen;
  GDEFSTOR_Store * s2 = Open(swapped, &data_seen);
  ExpectEveryPathRefused(s2, GDEFSTOR_ERR_FORMAT);
  gdefstor_store_destroy(s2);
  ASSERT_FALSE(data_seen.refusals.empty());
  EXPECT_NE(std::string::npos, data_seen.refusals[0].find("is a key store"));
  EXPECT_TRUE(data_before == ReadAll(swapped.At(kKeyFile)));
  EXPECT_TRUE(keys_before == ReadAll(swapped.At(kFile)));
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
