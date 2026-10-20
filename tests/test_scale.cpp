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
 * The queries at 10,000 entities of one type (CAP-S7), measured in rows and
 * steps, never in time. For each shape the SQL layer compiles on an indexed
 * field - equality, `in`, a range, a sort by an indexed field, the relation
 * semi-join, the entity lookup by public id - every statement of the call
 * takes no step of a full table scan and builds no automatic index
 * (sqlite3_stmt_status), the call's virtual-machine steps are bounded by its
 * answer rather than the table, and `EXPLAIN QUERY PLAN` of the compiled
 * statement names the index the field's declaration made. Creating one more
 * entity, reading one, listing a page and counting are bounded the same way.
 *
 * Every order has a tree in three languages (en, fr, de; story S6a2) and
 * every query binds one, so each of these shapes is also a measurement of a
 * query in one language over a table that holds three.
 *
 * GDEFSSQLITE_SCALE_ORDERS in the environment sets another size, for looking
 * at plans by hand; the bounds below do not depend on it.
 * GDEFSSQLITE_SCALE_VERBOSE prints each measured call.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>
#include <sqlite3.h>

#include "model_util.h"
#include "tmpdir.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace tu;

namespace {

const int T = GDEFSTOR_FIELD_TEXT;
const int I = GDEFSTOR_FIELD_INTEGER;
const int R = GDEFSTOR_FIELD_REFERENCE;
const unsigned IDX = GDEFSTOR_FIELD_INDEXED;
const unsigned MUL = GDEFSTOR_FIELD_MULTIPLE;
const unsigned PER = GDEFSTOR_FIELD_PERSONAL;

/* An order's personal contact: its subject is one of 1,000 people (ten
 * orders each), and its blind-index MAC one of 500 (twenty orders each). */
std::string Subject(int i) {
  char b[16];
  std::snprintf(b, sizeof b, "p%04d", i % 1000);
  return b;
}

std::string MacFor(int k) {
  std::string m(GDEFSTOR_BLIND_MAC_BYTES, 'm');
  std::snprintf(&m[0], m.size(), "mac-%04d", k % 500);
  m[8] = 'x';
  return m;
}

int Orders() {
  const char * e = std::getenv("GDEFSSQLITE_SCALE_ORDERS");
  return e != nullptr && std::atoi(e) > 0 ? std::atoi(e) : 10000;
}

const int kCusts = 1000;

std::string Id(char c, int i, int width) {
  char b[32];
  std::snprintf(b, sizeof b, "%c%0*d", c, width, i);
  return b;
}

struct Rec {
  std::string sql;
  int exec = 0;
  uint64_t rows = 0;
  uint64_t fullscan = 0;
  uint64_t vm = 0;
  uint64_t sorts = 0;
  uint64_t autoindex = 0;
};

void OnStatement(void * ctx, const GDEFSSQLITE_StatementStats * s) {
  auto * v = static_cast<std::vector<Rec> *>(ctx);
  Rec r;
  r.sql.assign(s->sql, s->sql_len);
  r.exec = s->exec;
  r.rows = s->rows;
  r.fullscan = s->fullscan_steps;
  r.vm = s->vm_steps;
  r.sorts = s->sorts;
  r.autoindex = s->autoindexes;
  v->push_back(r);
}

/* One store of N orders and 1,000 customers, built once for the suite. */
struct Big {
  std::string dir = MakeTempDir("gdefssqlite-scale-");
  std::vector<Rec> recs;
  GDEFSTOR_Store * store = nullptr;
  sqlite3 * raw = nullptr;
  int orders = Orders();

  ~Big() {
    sqlite3_close_v2(raw);
    gdefstor_store_destroy(store);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

Big * g_big = nullptr;

/* An order: number N.. (unique), bucket b000..b999 (ten orders each),
 * amount = i, the buyer c0000..c0999 (ten orders each), a note nothing
 * indexes, one line whose sku is one of 2,000 (five orders each), and a
 * title that is "common" in en (and de, below). */
Node Order(int i) {
  Node n;
  n.Set("number", Txt(Id('N', i, 5)));
  n.Set("title", Txt("common"));
  n.Set("bucket", Txt(Id('b', i % 1000, 3)));
  n.Set("amount", Num(I, i));
  n.Set("buyer", Ref(Id('c', i % kCusts, 4)));
  n.Set("note", Txt("n" + std::to_string(i % 7)));
  n.Set("contact", Personal(T, "sealed" + std::to_string(i), Subject(i)));
  Node & l = n.Add("lines");
  l.Set("sku", Txt(Id('s', i % 2000, 4)));
  l.Set("qty", Num(I, 1 + i % 3));
  return n;
}

/* The order's tree in fr or de: the same, without the personal contact (its
 * blind rows and subject stay the en tree's alone), and a title that is the
 * order's own in fr, f00000..f09999, and "common" in de. */
Node Translated(int i, char lang) {
  Node n = Order(i);
  n.vals.erase("contact");
  if (lang == 'f') {
    n.Set("title", Txt(Id('f', i, 5)));
  }
  return n;
}

Node Cust(int i) {
  Node n;
  n.Set("region", Txt(Id('r', i % 100, 2)));
  n.Set("name", Txt("name" + std::to_string(i)));
  return n;
}

/* Create @p pid and save @p tree in en and, given, @p fr and @p de, in one
 * revision. */
GDEFSTOR_Result SaveNew(GDEFSTOR_Write * w, const char * type,
    const std::string & pid, const Node & tree, const Node * fr = nullptr,
    const Node * de = nullptr) {
  GDEFSTOR_Result rc = gdefstor_entity_create(w, type, std::strlen(type),
      pid.data(), pid.size(), "", 0, 1, nullptr);
  GDEFSTOR_Tree * t = nullptr;
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_tree_create(nullptr, &t);
  }
  if (rc == GDEFSTOR_OK) {
    rc = Fill(t, 0, tree);
  }
  GDEFSTOR_Value contact;
  if (rc == GDEFSTOR_OK && gdefstor_tree_get(t, 0, "contact", 7, &contact) ==
          GDEFSTOR_OK && contact.type != 0) {
    /* The order's number is its index: N00042 has MAC 42 % 500. */
    std::string mac = MacFor(std::atoi(pid.c_str() + 1));
    GDEFSTOR_BlindMac b;
    std::memset(&b, 0, sizeof b);
    b.size = sizeof b;
    b.mac = reinterpret_cast<const uint8_t *>(mac.data());
    b.mac_len = mac.size();
    rc = gdefstor_tree_set_blind(t, 0, "contact", 7, &b);
  }
  GDEFSTOR_Tree * more[2] = {nullptr, nullptr};
  GDEFSTOR_LanguageTree lt[3];
  size_t nl = 0;
  std::memset(lt, 0, sizeof lt);
  lt[nl].size = sizeof lt[nl];
  lt[nl].language = "en";
  lt[nl].language_len = 2;
  lt[nl++].tree = t;
  for (int k = 0; rc == GDEFSTOR_OK && k < 2; k++) {
    const Node * n = k == 0 ? fr : de;
    if (n == nullptr) {
      continue;
    }
    rc = gdefstor_tree_create(nullptr, &more[k]);
    if (rc == GDEFSTOR_OK) {
      rc = Fill(more[k], 0, *n);
    }
    lt[nl].size = sizeof lt[nl];
    lt[nl].language = k == 0 ? "fr" : "de";
    lt[nl].language_len = 2;
    lt[nl++].tree = more[k];
  }
  GDEFSTOR_RevisionMeta m = Meta();
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_entity_save_languages(w, pid.data(), pid.size(), 0, &m, lt,
        nl, nullptr);
  }
  gdefstor_tree_free(t);
  gdefstor_tree_free(more[0]);
  gdefstor_tree_free(more[1]);
  return rc;
}

class Scale : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    g_big = new Big();
    Big & b = *g_big;
    ASSERT_FALSE(b.dir.empty());
    GDEFSSQLITE_ModelOptions o;
    gdefssqlite_model_options_default(&o);
    o.on_statement = OnStatement;
    o.on_statement_ctx = &b.recs;
    ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_model_store_create(b.dir.c_str(), &o,
        nullptr, nullptr, &b.store));
    ASSERT_EQ(GDEFSTOR_OK, Declare(b.store, "shop.cust",
        {Field("region", T, IDX), Field("name", T)}));
    ASSERT_EQ(GDEFSTOR_OK, Declare(b.store, "shop.order",
        {Field("number", T, IDX), Field("title", T, IDX),
            Field("bucket", T, IDX), Field("amount", I, IDX), Field("buyer", R, IDX), Field("note", T),
            Field("contact", T, PER),
            Group("lines", MUL, {Field("sku", T, IDX), Field("qty", I)})}));
    const int batch = 250;
    for (int from = 0; from < kCusts; from += batch) {
      ASSERT_EQ(GDEFSTOR_OK, InWrite(b.store, [&](GDEFSTOR_Write * w) {
        GDEFSTOR_Result rc = GDEFSTOR_OK;
        for (int i = from; rc == GDEFSTOR_OK && i < from + batch && i < kCusts;
             i++) {
          rc = SaveNew(w, "shop.cust", Id('c', i, 4), Cust(i));
        }
        return rc;
      }));
    }
    for (int from = 0; from < b.orders; from += batch) {
      ASSERT_EQ(GDEFSTOR_OK, InWrite(b.store, [&](GDEFSTOR_Write * w) {
        GDEFSTOR_Result rc = GDEFSTOR_OK;
        for (int i = from; rc == GDEFSTOR_OK && i < from + batch && i < b.orders;
             i++) {
          Node fr = Translated(i, 'f');
          Node de = Translated(i, 'd');
          rc = SaveNew(w, "shop.order", Id('o', i, 5), Order(i), &fr, &de);
        }
        return rc;
      }));
    }
    b.recs.clear();
    ASSERT_EQ(SQLITE_OK, sqlite3_open_v2((b.dir + "/7431.db").c_str(), &b.raw,
        SQLITE_OPEN_READONLY, nullptr));
  }

  static void TearDownTestSuite() {
    delete g_big;
    g_big = nullptr;
  }

  void SetUp() override {
    ASSERT_NE(nullptr, g_big);
    ASSERT_NE(nullptr, g_big->store);
    ASSERT_NE(nullptr, g_big->raw);
    if (g_big->orders != 10000) {
      std::printf("scale: %d orders, not 10,000 (GDEFSSQLITE_SCALE_ORDERS)\n",
          g_big->orders);
    }
  }

  /* ---- reading the file from outside ----------------------------------- */

  std::vector<std::string> Column(const std::string & sql) {
    std::vector<std::string> out;
    sqlite3_stmt * st = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(g_big->raw, sql.c_str(), -1, &st,
        nullptr)) << sqlite3_errmsg(g_big->raw) << " in " << sql;
    while (st != nullptr && sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char * t = sqlite3_column_text(st, 0);
      out.push_back(t == nullptr ? "" : (const char *)t);
    }
    sqlite3_finalize(st);
    return out;
  }

  /* The plan SQLite chooses for a statement, one detail per line. */
  std::string Plan(const std::string & sql) {
    std::string out;
    sqlite3_stmt * st = nullptr;
    std::string q = "EXPLAIN QUERY PLAN " + sql;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(g_big->raw, q.c_str(), -1, &st,
        nullptr)) << sqlite3_errmsg(g_big->raw);
    while (st != nullptr && sqlite3_step(st) == SQLITE_ROW) {
      out += (const char *)sqlite3_column_text(st, 3);
      out += "\n";
    }
    sqlite3_finalize(st);
    return out;
  }

  /* The index the declaration made for field @p path of @p type. */
  std::string IndexOf(const char * type, const char * path) {
    GDEFSTOR_Type * t = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_get(g_big->store, kTenant, 2, type,
        std::strlen(type), &t));
    size_t at = 0;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_find(t, path, std::strlen(path), &at));
    GDEFSTOR_FieldInfo f;
    std::memset(&f, 0, sizeof f);
    f.size = sizeof f;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_field(t, at, &f));
    gdefstor_type_free(t);
    uint32_t group = (f.flags & GDEFSTOR_FIELD_MULTIPLE) ? f.group : f.in_group;
    std::vector<std::string> tno =
        Column(std::string("SELECT tno FROM dss_type WHERE name = '") + type + "'");
    EXPECT_EQ(1u, tno.size());
    std::vector<std::string> name = Column("SELECT name FROM dss_name WHERE "
        "role = 9 AND tno = " + (tno.empty() ? std::string("0") : tno[0]) +
        " AND grp = " + std::to_string(group) + " AND col = " +
        std::to_string(f.column));
    EXPECT_EQ(1u, name.size()) << type << "." << path;
    return name.empty() ? "<none>" : name[0];
  }

  std::string EntityIndexOf(const char * type) {
    std::vector<std::string> tno =
        Column(std::string("SELECT tno FROM dss_type WHERE name = '") + type + "'");
    return "t" + (tno.empty() ? std::string("0") : tno[0]) + "p";
  }

  /* ---- what one call cost ------------------------------------------------ */

  struct Cost {
    std::vector<Rec> stmts;
    uint64_t vm = 0;
    uint64_t fullscan = 0;
    uint64_t autoindex = 0;
  };

  Cost Measure(const std::function<void()> & call) {
    g_big->recs.clear();
    call();
    Cost c;
    c.stmts = g_big->recs;
    for (const Rec & r : c.stmts) {
      c.vm += r.vm;
      c.fullscan += r.fullscan;
      c.autoindex += r.autoindex;
    }
    return c;
  }

  static std::string Describe(const Cost & c) {
    std::string s;
    for (const Rec & r : c.stmts) {
      if (r.exec) {
        continue;
      }
      s += "  vm " + std::to_string(r.vm) + " fullscan " +
           std::to_string(r.fullscan) + " rows " + std::to_string(r.rows) +
           ": " + r.sql + "\n";
    }
    return s;
  }

  /* No full-scan step, no automatic index, and steps that grow with the
   * answer, not the table: a scan of one of the tables, 10,000 rows, costs
   * well over 10,000 steps on its own, where an indexed answer of ten rows
   * measured about 400 (the catalog lookups included). */
  static void ExpectIndexed(const Cost & c, uint64_t answer, const char * what) {
    if (std::getenv("GDEFSSQLITE_SCALE_VERBOSE") != nullptr) {
      std::printf("%s: answer %llu, vm %llu\n%s", what,
          (unsigned long long)answer, (unsigned long long)c.vm,
          Describe(c).c_str());
    }
    EXPECT_EQ(0u, c.fullscan) << what << " scanned a table:\n" << Describe(c);
    EXPECT_EQ(0u, c.autoindex) << what << " built an index:\n" << Describe(c);
    EXPECT_LE(c.vm, 60 * (answer + 1) + 2000)
        << what << " took steps out of proportion to its answer of " << answer
        << ":\n" << Describe(c);
  }

  /* The compiled statements of a query: those that read the type's entity
   * table. */
  std::vector<std::string> Compiled(const Cost & c, const char * type) {
    std::string e = "\"t" +
        Column(std::string("SELECT tno FROM dss_type WHERE name = '") + type +
            "'")[0] + "_e\"";
    std::vector<std::string> out;
    for (const Rec & r : c.stmts) {
      if (!r.exec && r.sql.rfind("SELECT", 0) == 0 &&
          r.sql.find(e) != std::string::npos) {
        out.push_back(r.sql);
      }
    }
    return out;
  }

  void ExpectPlanNames(const Cost & c, const char * type,
      const std::vector<std::string> & indexes) {
    std::vector<std::string> stmts = Compiled(c, type);
    ASSERT_FALSE(stmts.empty()) << Describe(c);
    for (const std::string & sql : stmts) {
      std::string plan = Plan(sql);
      for (const std::string & ix : indexes) {
        EXPECT_NE(std::string::npos, plan.find("INDEX " + ix + " "))
            << "the plan does not use " << ix << ":\n" << plan << "for " << sql;
      }
    }
  }
};

/* ---- building queries -------------------------------------------------------- */

struct Q {
  std::vector<std::unique_ptr<GDEFSTOR_QueryNode>> nodes;
  std::vector<std::unique_ptr<std::vector<const GDEFSTOR_QueryNode *>>> lists;
  std::vector<std::unique_ptr<std::vector<GDEFSTOR_Value>>> vals;
  std::vector<std::unique_ptr<std::string>> strs;

  GDEFSTOR_QueryOperand Field(const char * path, int flags = 0) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_FIELD;
    o.field = path;
    o.field_len = std::strlen(path);
    o.flags = flags;
    return o;
  }
  GDEFSTOR_QueryOperand Entity(int flags = 0) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_ENTITY;
    o.flags = flags;
    return o;
  }
  GDEFSTOR_QueryOperand Values(std::vector<V> vs) {
    auto v = std::make_unique<std::vector<GDEFSTOR_Value>>();
    for (const V & x : vs) {
      strs.push_back(std::make_unique<std::string>(x.s));
      GDEFSTOR_Value g = ValueOf(x);
      g.bytes = reinterpret_cast<const uint8_t *>(strs.back()->data());
      v->push_back(g);
    }
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_VALUES;
    o.values = v->data();
    o.value_count = v->size();
    vals.push_back(std::move(v));
    return o;
  }
  const GDEFSTOR_QueryNode * Cmp(int op, GDEFSTOR_QueryOperand l,
      GDEFSTOR_QueryOperand r) {
    auto n = std::make_unique<GDEFSTOR_QueryNode>();
    std::memset(n.get(), 0, sizeof *n);
    n->op = op;
    n->left = l;
    n->right = r;
    nodes.push_back(std::move(n));
    return nodes.back().get();
  }
  const GDEFSTOR_QueryNode * Op(int op, std::vector<const GDEFSTOR_QueryNode *> kids,
      const char * exists_type = nullptr) {
    auto l = std::make_unique<std::vector<const GDEFSTOR_QueryNode *>>(kids);
    auto n = std::make_unique<GDEFSTOR_QueryNode>();
    std::memset(n.get(), 0, sizeof *n);
    n->op = op;
    n->children = l->data();
    n->child_count = l->size();
    if (exists_type != nullptr) {
      n->exists_type = exists_type;
      n->exists_type_len = std::strlen(exists_type);
    }
    lists.push_back(std::move(l));
    nodes.push_back(std::move(n));
    return nodes.back().get();
  }
};

GDEFSTOR_Query Page(const GDEFSTOR_QueryNode * where, uint64_t limit = 20,
    const char * lang = "en") {
  GDEFSTOR_Query q;
  std::memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.language = lang;
  q.language_len = std::strlen(lang);
  q.where = where;
  q.limit = limit;
  return q;
}

/* ---- the shapes ------------------------------------------------------------- */

TEST_F(Scale, EqualityUsesTheFieldIndex) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("bucket"),
      q.Values({Txt("b007")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(uint64_t(g_big->orders / 1000), total);
  ExpectIndexed(c, total, "bucket = 'b007'");
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

TEST_F(Scale, EqualityOnAMultipleGroupUsesItsIndex) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("lines.sku"),
      q.Values({Txt("s0042")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(uint64_t(g_big->orders / 2000), total);
  ExpectIndexed(c, total, "lines.sku = 's0042'");
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "lines.sku")});
}

TEST_F(Scale, InUsesTheFieldIndex) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_IN, q.Field("bucket"),
      q.Values({Txt("b001"), Txt("b002"), Txt("b003")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(uint64_t(3 * (g_big->orders / 1000)), total);
  ExpectIndexed(c, total, "bucket in (...)");
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

TEST_F(Scale, RangesUseTheFieldIndex) {
  for (int op : {GDEFSTOR_QUERY_GE, GDEFSTOR_QUERY_LT}) {
    Q q;
    int64_t bound = op == GDEFSTOR_QUERY_GE ? g_big->orders - 10 : 10;
    GDEFSTOR_Query query = Page(q.Cmp(op, q.Field("amount"),
        q.Values({Num(I, bound)})));
    std::vector<std::string> ids;
    uint64_t total = 0;
    Cost c = Measure([&]() {
      ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids,
          &total));
    });
    EXPECT_EQ(10u, total);
    ExpectIndexed(c, total, op == GDEFSTOR_QUERY_GE ? "amount >= N-10"
                                                    : "amount < 10");
    ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "amount")});
  }
}

/* A sort by an indexed field over what an indexed filter selects: the sort
 * is of the answer, not of the table. */
TEST_F(Scale, ASortedFilteredPageSortsTheAnswer) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_IN, q.Field("bucket"),
      q.Values({Txt("b001"), Txt("b002"), Txt("b003"), Txt("b004"),
          Txt("b005")})));
  GDEFSTOR_QuerySort s;
  std::memset(&s, 0, sizeof s);
  s.key = q.Field("amount");
  s.descending = 1;
  query.sorts = &s;
  query.sort_count = 1;
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  uint64_t want = 5 * (uint64_t)(g_big->orders / 1000);
  EXPECT_EQ(want, total);
  ASSERT_EQ(std::min<size_t>(20, want), ids.size());
  /* Descending by amount: the bucket b005 order with the largest amount. */
  int last = g_big->orders - 1;
  while (last % 1000 < 1 || last % 1000 > 5) {
    last--;
  }
  EXPECT_EQ(Id('o', last, 5), ids[0]);
  ExpectIndexed(c, total, "bucket in (...) sorted by amount");
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

/* The relation semi-join: customers of one region with an order that names
 * them as its buyer. The region index drives the outer rows and the buyer
 * index each EXISTS. */
TEST_F(Scale, TheRelationSemiJoinUsesBothIndexes) {
  Q q;
  const GDEFSTOR_QueryNode * exists = q.Op(GDEFSTOR_QUERY_EXISTS,
      {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("buyer"),
          q.Entity(GDEFSTOR_QUERY_OUTER))},
      "shop.order");
  GDEFSTOR_Query query = Page(q.Op(GDEFSTOR_QUERY_AND,
      {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("region"), q.Values({Txt("r07")})),
          exists}));
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.cust", query, &ids, &total));
  });
  uint64_t want = g_big->orders >= kCusts ? kCusts / 100 : 0;
  EXPECT_EQ(want, total);
  ExpectIndexed(c, total * (uint64_t)(g_big->orders / kCusts + 1),
      "region = 'r07' and exists an order bought by it");
  ExpectPlanNames(c, "shop.cust", {IndexOf("shop.cust", "region"),
      IndexOf("shop.order", "buyer")});
}

/* The public id as a query operand: the entity table's own index. */
TEST_F(Scale, TheEntityLookupByPublicIdUsesItsIndex) {
  Q q;
  std::string pid = Id('o', g_big->orders / 2, 5);
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Entity(),
      q.Values({Ref(pid)})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(1u, total);
  ASSERT_EQ(1u, ids.size());
  EXPECT_EQ(pid, ids[0]);
  ExpectIndexed(c, 1, "the entity by public id");
  ExpectPlanNames(c, "shop.order", {EntityIndexOf("shop.order")});
}

/* Reading one entity and its tree: the public-id lookup and the row reads
 * take no scan. */
TEST_F(Scale, ReadingOneEntityTakesNoScan) {
  std::string pid = Id('o', g_big->orders / 3, 5);
  Cost c = Measure([&]() {
    GDEFSTOR_Entity * e = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_get(g_big->store, kTenant, 2,
        pid.data(), pid.size(), &e));
    gdefstor_entity_free(e);
    GDEFSTOR_Tree * t = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(g_big->store, kTenant, 2,
        pid.data(), pid.size(), "en", 2, 0, &t));
    gdefstor_tree_free(t);
  });
  ExpectIndexed(c, 1, "reading one entity");
  bool looked_up = false;
  for (const Rec & r : c.stmts) {
    if (!r.exec && r.sql.find("\"dss_entity\"") != std::string::npos &&
        r.sql.find("\"pid\" = ?") != std::string::npos) {
      looked_up = true;
      EXPECT_NE(std::string::npos,
          Plan(r.sql).find("INDEX sqlite_autoindex_dss_entity_1 "))
          << Plan(r.sql);
    }
  }
  EXPECT_TRUE(looked_up) << Describe(c);
}

/* Creating and saving one more entity at this size: no scan, and a cost
 * that is the same as for the first one. */
TEST_F(Scale, CreatingOneMoreEntityTakesNoScan) {
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, InWrite(g_big->store, [](GDEFSTOR_Write * w) {
      return SaveNew(w, "shop.order", "z00001", Order(424242));
    }));
  });
  ExpectIndexed(c, 1, "creating one entity");
  Cost again = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, InWrite(g_big->store, [](GDEFSTOR_Write * w) {
      return SaveNew(w, "shop.order", "z00002", Order(424243));
    }));
  });
  ExpectIndexed(again, 1, "creating another");
}

/* A page of every entity, in creation order, reads the page and not the
 * table; its total counts every entity, which is its answer. */
TEST_F(Scale, AListPageReadsThePageAndTheCountReadsTheAnswer) {
  GDEFSTOR_Query query = Page(nullptr);
  query.offset = 40;
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.cust", query, &ids, &total));
  });
  EXPECT_EQ((uint64_t)kCusts, total);
  ASSERT_EQ(20u, ids.size());
  EXPECT_EQ(Id('c', 40, 4), ids[0]);
  EXPECT_EQ(0u, c.autoindex) << Describe(c);
  std::vector<std::string> stmts = Compiled(c, "shop.cust");
  ASSERT_EQ(2u, stmts.size()) << Describe(c);
  for (const Rec & r : c.stmts) {
    if (r.exec || r.sql.find("COUNT(*)") != std::string::npos) {
      continue;
    }
    /* The page: offset plus limit rows of the entity table, at most. */
    EXPECT_LE(r.fullscan, 2u * (40 + 20)) << r.sql;
  }
  uint64_t n = 0;
  Cost count = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(g_big->store, kTenant, 2,
        "shop.order", 10, "en", 2, nullptr, &n));
  });
  EXPECT_GE(n, (uint64_t)g_big->orders);
  EXPECT_LE(count.vm, 60 * (n + 1) + 6000) << Describe(count);
  EXPECT_EQ(0u, count.autoindex) << Describe(count);
}

/* A count with an indexed filter reads the answer. */
TEST_F(Scale, AFilteredCountReadsTheAnswer) {
  Q q;
  uint64_t n = 0;
  const GDEFSTOR_QueryNode * where = q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("bucket"),
      q.Values({Txt("b123")}));
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(g_big->store, kTenant, 2,
        "shop.order", 10, "en", 2, where, &n));
  });
  EXPECT_EQ(uint64_t(g_big->orders / 1000), n);
  ExpectIndexed(c, n, "count of bucket = 'b123'");
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

/* ---- one language of three (story S6a2) --------------------------------- */

/* A query binds one language, and a field's index is (value, language,
 * entity), so an equality reads its language's entries and never the
 * others'. "title" is "common" in en and de for every order and the order's
 * own in fr: in fr, "common" matches nothing, and an index without the
 * language would read the 20,000 entries of the other two to learn that; in
 * fr, one order's title is one entry. The plan's search names the language
 * beside the value. */
TEST_F(Scale, AFieldQueryInOneLanguageReadsOnlyItsLanguage) {
  const std::string ix = IndexOf("shop.order", "title");
  {
    Q q;
    GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("title"),
        q.Values({Txt("common")})), 20, "fr");
    std::vector<std::string> ids;
    uint64_t total = 9;
    Cost c = Measure([&]() {
      ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids,
          &total));
    });
    EXPECT_EQ(0u, total);
    ExpectIndexed(c, 0, "title = 'common' in fr");
    ExpectPlanNames(c, "shop.order", {ix});
    for (const std::string & sql : Compiled(c, "shop.order")) {
      std::string plan = Plan(sql);
      EXPECT_NE(std::string::npos, plan.find(ix + " (v"))
          << "the plan does not search the title index:\n" << plan << sql;
      EXPECT_NE(std::string::npos, plan.find("=? AND lang=?)"))
          << "the search does not name the language:\n" << plan << sql;
    }
  }
  {
    Q q;
    GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("title"),
        q.Values({Txt(Id('f', 42, 5))})), 20, "fr");
    std::vector<std::string> ids;
    uint64_t total = 0;
    Cost c = Measure([&]() {
      ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids,
          &total));
    });
    EXPECT_EQ(1u, total);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(Id('o', 42, 5), ids[0]);
    ExpectIndexed(c, 1, "title = 'f00042' in fr");
  }
  {
    /* The same bucket in de as in en: the same ten orders, by the index. */
    Q q;
    GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("bucket"),
        q.Values({Txt("b007")})), 20, "de");
    std::vector<std::string> ids;
    uint64_t total = 0;
    Cost c = Measure([&]() {
      ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids,
          &total));
    });
    EXPECT_EQ(uint64_t(g_big->orders / 1000), total);
    ExpectIndexed(c, total, "bucket = 'b007' in de");
  }
}

/* ---- the blind index and the subject lookup (story S5a) ------------------ */

/* "Everything about this person": the subject columns' index finds the ten
 * orders naming one subject. Only the catalog read (dss_name by role, a
 * table the size of the schema, not of the data) may scan; every statement
 * on a group table goes through the subject index. */
TEST_F(Scale, TheSubjectLookupUsesTheSubjectIndex) {
  GDEFSTOR_SubjectHits * h = nullptr;
  std::string who = Subject(7);
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_subject_lookup(g_big->store, kTenant, 2,
        who.data(), who.size(), &h));
  });
  uint64_t n = gdefstor_subject_hits_count(h);
  gdefstor_subject_hits_free(h);
  EXPECT_EQ(uint64_t(g_big->orders / 1000), n);
  EXPECT_EQ(0u, c.autoindex) << Describe(c);
  EXPECT_LE(c.vm, 60 * (n + 1) + 2000) << Describe(c);
  std::string tno = Column("SELECT tno FROM dss_type WHERE name = 'shop.order'")[0];
  std::string group = "\"t" + tno + "_g0\"";
  int read = 0;
  for (const Rec & r : c.stmts) {
    if (r.exec || r.sql.find("\"dss_name\"") != std::string::npos) {
      continue;
    }
    EXPECT_EQ(0u, r.fullscan) << "scanned: " << r.sql;
    if (r.sql.find(group) != std::string::npos) {
      read++;
      std::string plan = Plan(r.sql);
      EXPECT_NE(std::string::npos, plan.find("INDEX t" + tno + "_j0_"))
          << "the plan does not use the subject index:\n" << plan << r.sql;
    }
  }
  EXPECT_EQ(1, read) << Describe(c);
}

/* Blind equality: twenty orders share a MAC, found through the blind table's
 * (fld, mac) index and joined to their current versions by key. */
TEST_F(Scale, BlindEqualityUsesTheBlindIndex) {
  Q q;
  std::string mac = MacFor(42);
  GDEFSTOR_QueryText text;
  text.bytes = reinterpret_cast<const uint8_t *>(mac.data());
  text.len = mac.size();
  GDEFSTOR_QueryOperand right;
  std::memset(&right, 0, sizeof right);
  right.kind = GDEFSTOR_QUERY_CONST;
  right.texts = &text;
  right.text_count = 1;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("contact"),
      right), 50);
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(uint64_t(g_big->orders / 500), total);
  ExpectIndexed(c, total, "contact blind = MAC 42");
  std::string tno = Column("SELECT tno FROM dss_type WHERE name = 'shop.order'")[0];
  ExpectPlanNames(c, "shop.order", {"t" + tno + "_bm"});
}

/* The history lookup reads the same index, whatever the table's size. */
TEST_F(Scale, TheHistoryLookupUsesTheBlindIndex) {
  std::string mac = MacFor(7);
  GDEFSTOR_BlindHits * h = nullptr;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_blind_history(g_big->store, kTenant, 2,
        "shop.order", 10, "contact", 7,
        reinterpret_cast<const uint8_t *>(mac.data()), mac.size(), &h));
  });
  uint64_t n = gdefstor_blind_hits_count(h);
  gdefstor_blind_hits_free(h);
  EXPECT_EQ(uint64_t(g_big->orders / 500), n);
  ExpectIndexed(c, n, "the history of MAC 7");
  std::string tno = Column("SELECT tno FROM dss_type WHERE name = 'shop.order'")[0];
  int read = 0;
  for (const Rec & r : c.stmts) {
    if (!r.exec && r.sql.find("\"t" + tno + "_b\"") != std::string::npos) {
      read++;
      EXPECT_NE(std::string::npos, Plan(r.sql).find("INDEX t" + tno + "_bm"))
          << Plan(r.sql) << r.sql;
    }
  }
  EXPECT_EQ(1, read) << Describe(c);
}

/* The control: a filter on a field with no index does scan, and the
 * measurement sees it, so a zero above is a measurement and not a blind
 * spot. */
TEST_F(Scale, AFilterOnAFieldWithNoIndexIsSeenToScan) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("note"),
      q.Values({Txt("n3")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  Cost c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_GT(total, 0u);
  EXPECT_GE(c.fullscan + c.autoindex, (uint64_t)g_big->orders / 2)
      << Describe(c);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
