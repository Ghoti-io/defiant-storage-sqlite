/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage SQL.
 *
 * Ghoti.io Defiant Storage SQL is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * version 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage SQL is distributed in the hope that it will be
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
 * Seeded random data and queries over two related types, for the agreement
 * sweep; copied from defiant-storage-sql's tests/query_gen.h. Values come from
 * small domains so that comparisons match, sort keys tie and some values are
 * absent: the cases where a compiled query can differ from the reference scan.
 */

#ifndef GDEFSSQLITE_TESTS_QUERY_GEN_H
#define GDEFSSQLITE_TESTS_QUERY_GEN_H

#include "model_util.h"

#include <functional>
#include <memory>
#include <random>

namespace qgen {

using namespace tu;

const int T = GDEFSTOR_FIELD_TEXT;
const int I = GDEFSTOR_FIELD_INTEGER;
const int D = GDEFSTOR_FIELD_DECIMAL;
const int FL = GDEFSTOR_FIELD_FLOAT;
const int DT = GDEFSTOR_FIELD_DATETIME;
const int DA = GDEFSTOR_FIELD_DATE;
const int R = GDEFSTOR_FIELD_REFERENCE;
const int B = GDEFSTOR_FIELD_BOOLEAN;
const unsigned MUL = GDEFSTOR_FIELD_MULTIPLE;
const unsigned IDX = GDEFSTOR_FIELD_INDEXED;
const unsigned PER = GDEFSTOR_FIELD_PERSONAL;

inline std::vector<F> Order() {
  return {Field("number", T, IDX), Field("total", D, 0, 2), Field("placed", DT),
      Field("tags", T, MUL), Field("buyer", R), Field("email", T, PER),
      Field("old", I), Field("weight", FL), Field("due", DA),
      Group("lines", MUL, {Field("sku", T, IDX), Field("qty", I),
          Group("taxes", MUL, {Field("rate", D, 0, 4), Field("kind", T)})})};
}

inline std::vector<F> Customer() {
  return {Field("name", T), Field("order", R, IDX), Field("vip", B)};
}

/* A scalar field of either type: its path, type and domain. */
struct Leaf {
  const char * type_name;
  const char * path;
  int type;
  bool root_single; /* sortable */
};

inline const std::vector<Leaf> & Leaves() {
  static const std::vector<Leaf> v = {
      {"shop.order", "number", T, true}, {"shop.order", "total", D, true},
      {"shop.order", "placed", DT, true}, {"shop.order", "tags", T, false},
      {"shop.order", "buyer", R, true}, {"shop.order", "old", I, true},
      {"shop.order", "weight", FL, true}, {"shop.order", "due", DA, true},
      {"shop.order", "lines.sku", T, false}, {"shop.order", "lines.qty", I, false},
      {"shop.order", "lines.taxes.rate", D, false},
      {"shop.order", "lines.taxes.kind", T, false},
      {"shop.cust", "name", T, true}, {"shop.cust", "order", R, true},
      {"shop.cust", "vip", B, true}};
  return v;
}

struct Gen {
  std::mt19937 rng;
  /* Every choice of language (story S6a2) is drawn from this one, so that
   * adding languages left every other draw of rng where it was. */
  std::mt19937 lang_rng;
  bool distinct = false; /* constants unlike any stored value, for binding */
  int norders = 12;
  int ncusts = 6;
  explicit Gen(uint32_t seed) : rng(seed), lang_rng(seed + 0x5a2u) {}
  int Pick(int n) {
    return (int)(rng() % (uint32_t)n);
  }
  bool Chance(int in) {
    return Pick(in) == 0;
  }
  std::string OrderId(int i) {
    return "o" + std::to_string(i);
  }
  std::string CustId(int i) {
    return "c" + std::to_string(i);
  }

  /* One value of field type @p type from its small domain. */
  V Value(int type) {
    if (distinct) {
      std::string s = "ZQ" + std::to_string(rng() % 100000) + "x";
      switch (type) {
      case T:
        return Txt(s);
      case R:
        return Ref(s);
      case FL:
        return Flt(1234567.25 + Pick(10));
      case B:
        return Num(B, Pick(2));
      default:
        return Num(type, 987654300 + Pick(90));
      }
    }
    static const char * const texts[] = {"A", "B", "s1", "s2", "x", "y", "vat"};
    switch (type) {
    case T:
      return Txt(texts[Pick(7)]);
    case R:
      return Ref(Chance(2) ? OrderId(1 + Pick(norders)) : CustId(1 + Pick(ncusts)));
    case D:
      return Num(D, 100 * Pick(6));
    case DT:
      return Num(DT, Pick(5));
    case DA:
      return Num(DA, Pick(4) - 1);
    case I:
      return Num(I, Pick(5) - 1);
    case FL:
      return Flt((double)(Pick(5) - 2) / 2.0);
    case B:
      return Num(B, Pick(2));
    default:
      return Num(I, 0);
    }
  }

  Node OrderTree() {
    Node n;
    n.Set("number", Value(T));
    if (!Chance(3)) {
      n.Set("total", Value(D));
    }
    if (!Chance(3)) {
      n.Set("placed", Value(DT));
    }
    if (!Chance(3)) {
      n.Set("old", Value(I));
    }
    if (!Chance(3)) {
      n.Set("weight", Value(FL));
    }
    if (!Chance(3)) {
      n.Set("due", Value(DA));
    }
    if (!Chance(3)) {
      n.Set("buyer", Ref(CustId(1 + Pick(ncusts))));
    }
    n.Set("email", Personal(T, "\x01\x02 cipher", "subj"));
    for (int k = Pick(3); k > 0; k--) {
      n.Item("tags", Value(T));
    }
    for (int k = Pick(4); k > 0; k--) {
      Node & l = n.Add("lines");
      l.Set("sku", Value(T));
      if (!Chance(3)) {
        l.Set("qty", Value(I));
      }
      for (int j = Pick(3); j > 0; j--) {
        Node & t = l.Add("taxes");
        if (!Chance(3)) {
          t.Set("rate", Value(D));
        }
        t.Set("kind", Value(T));
      }
    }
    return n;
  }

  Node CustTree() {
    Node n;
    n.Set("name", Value(T));
    if (!Chance(4)) {
      n.Set("order", Ref(OrderId(1 + Pick(norders))));
    }
    n.Set("vip", Value(B));
    return n;
  }

  /* Declare both types and write the data: entities with and without a
   * revision, several revisions, states, creators. */
  GDEFSTOR_Result Populate(GDEFSTOR_Store * store) {
    GDEFSTOR_Result rc = Declare(store, "shop.order", Order());
    if (rc == GDEFSTOR_OK) {
      rc = Declare(store, "shop.cust", Customer());
    }
    for (int i = 1; rc == GDEFSTOR_OK && i <= norders; i++) {
      static const char * const creators[] = {"", "u1", "u2"};
      if (Chance(8)) {
        /* An entity with no revision: no items, no field values. */
        std::string creator = creators[Pick(3)];
        int64_t created = Pick(4);
        rc = InWrite(store, [&](GDEFSTOR_Write * w) {
          std::string pid = OrderId(i);
          return gdefstor_entity_create(w, "shop.order", 10, pid.data(),
              pid.size(), creator.data(), creator.size(), created, nullptr);
        });
        continue;
      }
      std::string creator = creators[Pick(3)];
      rc = Put(store, "shop.order", OrderId(i), OrderTree(), 0, true, nullptr,
          creator, Pick(4));
      for (int k = Pick(3); rc == GDEFSTOR_OK && k > 0; k--) {
        uint64_t head = 0;
        GDEFSTOR_Entity * e = nullptr;
        std::string pid = OrderId(i);
        rc = gdefstor_entity_get(store, kTenant, 2, pid.data(), pid.size(), &e);
        if (rc == GDEFSTOR_OK) {
          GDEFSTOR_EntityInfo info;
          std::memset(&info, 0, sizeof info);
          info.size = sizeof info;
          gdefstor_entity_info(e, &info);
          head = info.revision;
          gdefstor_entity_free(e);
          rc = Put(store, "shop.order", pid, OrderTree(), head, false);
        }
      }
      if (rc == GDEFSTOR_OK && !Chance(3)) {
        rc = State(store, OrderId(i), Pick(2), Chance(4));
      }
      if (rc == GDEFSTOR_OK) {
        rc = Translate(store, "shop.order", OrderId(i), true);
      }
    }
    for (int i = 1; rc == GDEFSTOR_OK && i <= ncusts; i++) {
      rc = Put(store, "shop.cust", CustId(i), CustTree(), 0, true);
      if (rc == GDEFSTOR_OK && Chance(2)) {
        rc = State(store, CustId(i), 1, 0);
      }
      if (rc == GDEFSTOR_OK) {
        rc = Translate(store, "shop.cust", CustId(i), false);
      }
    }
    return rc;
  }

  /* Perhaps a tree in "fr" too, from lang_rng's draws: in a revision of its
   * own, so the entity's "en" and "fr" values differ and a query in one
   * language that read the other would answer differently. "de" is never
   * written: a query in it finds every entity with no tree. */
  GDEFSTOR_Result Translate(GDEFSTOR_Store * store, const char * type,
      const std::string & pid, bool order) {
    GDEFSTOR_Entity * e = nullptr;
    GDEFSTOR_EntityInfo info;
    if (lang_rng() % 2 != 0) {
      return GDEFSTOR_OK;
    }
    GDEFSTOR_Result rc = gdefstor_entity_get(store, kTenant, 2, pid.data(),
        pid.size(), &e);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    gdefstor_entity_info(e, &info);
    gdefstor_entity_free(e);
    if (info.revision == 0) {
      return GDEFSTOR_OK;
    }
    std::swap(rng, lang_rng);
    Node t = order ? OrderTree() : CustTree();
    std::swap(rng, lang_rng);
    return Put(store, type, pid, t, info.revision, false, nullptr, "", 5,
        kTenant, "fr");
  }

  /* ---- queries ----------------------------------------------------------- */

  struct Built {
    std::vector<std::unique_ptr<GDEFSTOR_QueryNode>> nodes;
    std::vector<std::unique_ptr<std::vector<const GDEFSTOR_QueryNode *>>> lists;
    std::vector<std::unique_ptr<std::vector<GDEFSTOR_Value>>> values;
    std::vector<std::unique_ptr<V>> keep;
    std::vector<GDEFSTOR_QuerySort> sorts;
    GDEFSTOR_Query q;
    /* What was used, for the sweep's coverage counts. */
    int exists = 0;
    int nots = 0;
    int field_field = 0;
    int outer = 0;
    std::vector<V> constants;
  };

  GDEFSTOR_QueryOperand Op(int kind) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = kind;
    return o;
  }

  GDEFSTOR_QueryOperand Values(Built & b, int type, int n) {
    b.values.push_back(std::make_unique<std::vector<GDEFSTOR_Value>>());
    for (int i = 0; i < n; i++) {
      b.keep.push_back(std::make_unique<V>(Value(type)));
      b.constants.push_back(*b.keep.back());
      b.values.back()->push_back(ValueOf(*b.keep.back()));
    }
    GDEFSTOR_QueryOperand o = Op(GDEFSTOR_QUERY_VALUES);
    o.values = b.values.back()->data();
    o.value_count = b.values.back()->size();
    return o;
  }

  const Leaf & LeafOf(const char * type_name) {
    while (true) {
      const Leaf & l = Leaves()[(size_t)Pick((int)Leaves().size())];
      if (std::strcmp(l.type_name, type_name) == 0) {
        return l;
      }
    }
  }

  static bool Ordered(int t) {
    return t == I || t == D || t == FL || t == DT || t == DA;
  }

  GDEFSTOR_QueryOperand FieldOp(const Leaf & l, int flags = 0) {
    GDEFSTOR_QueryOperand o = Op(GDEFSTOR_QUERY_FIELD);
    o.field = l.path;
    o.field_len = std::strlen(l.path);
    o.flags = flags;
    return o;
  }

  /* A META or ENTITY operand of type @p t, if there is one. */
  bool ScalarOf(int t, int flags, GDEFSTOR_QueryOperand * out) {
    GDEFSTOR_QueryOperand o = Op(GDEFSTOR_QUERY_META);
    o.flags = flags;
    switch (t) {
    case R:
      if (Chance(2)) {
        o = Op(GDEFSTOR_QUERY_ENTITY);
        o.flags = flags;
      } else {
        o.meta = GDEFSTOR_META_CREATOR;
      }
      break;
    case DT:
      o.meta = GDEFSTOR_META_CREATED;
      break;
    case I:
      o.meta = GDEFSTOR_META_REVISION;
      break;
    case B:
      o.meta = Chance(2) ? GDEFSTOR_META_PUBLISHED : GDEFSTOR_META_DELETED;
      break;
    default:
      return false;
    }
    *out = o;
    return true;
  }

  const GDEFSTOR_QueryNode * Node_(Built & b, int op,
      std::vector<const GDEFSTOR_QueryNode *> kids) {
    b.nodes.push_back(std::make_unique<GDEFSTOR_QueryNode>());
    GDEFSTOR_QueryNode * n = b.nodes.back().get();
    std::memset(n, 0, sizeof *n);
    n->op = op;
    b.lists.push_back(std::make_unique<std::vector<const GDEFSTOR_QueryNode *>>(kids));
    n->children = b.lists.back()->data();
    n->child_count = kids.size();
    return n;
  }

  const GDEFSTOR_QueryNode * Leaf_(Built & b, const char * type_name,
      bool in_exists) {
    b.nodes.push_back(std::make_unique<GDEFSTOR_QueryNode>());
    GDEFSTOR_QueryNode * n = b.nodes.back().get();
    std::memset(n, 0, sizeof *n);
    const Leaf & l = LeafOf(type_name);
    static const int eq_ops[] = {GDEFSTOR_QUERY_EQ, GDEFSTOR_QUERY_NE,
        GDEFSTOR_QUERY_IN};
    static const int all_ops[] = {GDEFSTOR_QUERY_EQ, GDEFSTOR_QUERY_NE,
        GDEFSTOR_QUERY_IN, GDEFSTOR_QUERY_LT, GDEFSTOR_QUERY_LE,
        GDEFSTOR_QUERY_GT, GDEFSTOR_QUERY_GE};
    n->op = Ordered(l.type) ? all_ops[Pick(7)] : eq_ops[Pick(3)];
    int shape = Pick(10);
    GDEFSTOR_QueryOperand s;
    if (n->op != GDEFSTOR_QUERY_IN && shape < 2 &&
        ScalarOf(l.type, in_exists && Chance(2) ? GDEFSTOR_QUERY_OUTER : 0, &s)) {
      /* A field against a fact of the entity (or, inside EXISTS, of the
       * outer one). */
      if (s.flags != 0) {
        b.outer++;
      }
      n->left = FieldOp(l);
      n->right = s;
      if (Chance(2)) {
        std::swap(n->left, n->right);
      }
      return n;
    }
    if (n->op != GDEFSTOR_QUERY_IN && shape == 2) {
      /* A field against another field of one type: of this entity, or of the
       * outer one. */
      const Leaf * other = nullptr;
      const char * other_type = type_name;
      if (in_exists && Chance(2)) {
        other_type = "shop.order";
      }
      for (int tries = 0; tries < 20 && other == nullptr; tries++) {
        const Leaf & o = LeafOf(other_type);
        /* Decimals compare at one scale: the same field. */
        if (o.type == l.type &&
            (o.type != D || std::strcmp(o.path, l.path) == 0)) {
          other = &o;
        }
      }
      if (other != nullptr) {
        b.field_field++;
        int fl = other_type != type_name ? GDEFSTOR_QUERY_OUTER : 0;
        if (fl != 0) {
          b.outer++;
        }
        n->left = FieldOp(l);
        n->right = FieldOp(*other, fl);
        return n;
      }
    }
    if (n->op != GDEFSTOR_QUERY_IN && shape == 3 &&
        ScalarOf(l.type, 0, &s)) {
      /* A fact against constants. */
      n->left = s;
      n->right = Values(b, l.type, 1 + Pick(2));
      return n;
    }
    n->left = FieldOp(l);
    n->right = Values(b, l.type, n->op == GDEFSTOR_QUERY_IN ? Pick(4) : 1 + (Chance(3) ? Pick(3) : 0));
    if (n->op != GDEFSTOR_QUERY_IN && Chance(5)) {
      std::swap(n->left, n->right);
    }
    if (n->op != GDEFSTOR_QUERY_IN && !distinct && Chance(15)) {
      /* Two constant lists. */
      n->left = Values(b, l.type, Pick(3));
    }
    return n;
  }

  const GDEFSTOR_QueryNode * Pred(Built & b, const char * type_name, int depth,
      bool in_exists) {
    int pick = Pick(depth >= 3 ? 2 : 12);
    if (pick <= 3) {
      return Leaf_(b, type_name, in_exists);
    }
    if (pick <= 5) {
      std::vector<const GDEFSTOR_QueryNode *> kids;
      for (int k = Pick(4); k > 0; k--) {
        kids.push_back(Pred(b, type_name, depth + 1, in_exists));
      }
      return Node_(b, Chance(2) ? GDEFSTOR_QUERY_AND : GDEFSTOR_QUERY_OR, kids);
    }
    if (pick <= 7) {
      b.nots++;
      return Node_(b, GDEFSTOR_QUERY_NOT, {Pred(b, type_name, depth + 1, in_exists)});
    }
    if (pick == 8 && !in_exists && std::strcmp(type_name, "shop.order") == 0) {
      b.exists++;
      if (Chance(4)) {
        /* Any predicate of the other type, related or not. */
        const GDEFSTOR_QueryNode * e = Node_(b, GDEFSTOR_QUERY_EXISTS,
            {Pred(b, "shop.cust", depth + 1, true)});
        ((GDEFSTOR_QueryNode *)e)->exists_type = "shop.cust";
        ((GDEFSTOR_QueryNode *)e)->exists_type_len = 9;
        return e;
      }
      std::vector<const GDEFSTOR_QueryNode *> kids;
      /* A relation: the customer's order is this one; often with more. */
      b.nodes.push_back(std::make_unique<GDEFSTOR_QueryNode>());
      GDEFSTOR_QueryNode * rel = b.nodes.back().get();
      std::memset(rel, 0, sizeof *rel);
      rel->op = GDEFSTOR_QUERY_EQ;
      rel->left = FieldOp(Leaves()[13]);
      rel->right = Op(GDEFSTOR_QUERY_ENTITY);
      rel->right.flags = GDEFSTOR_QUERY_OUTER;
      b.outer++;
      kids.push_back(rel);
      if (Chance(2)) {
        kids.push_back(Pred(b, "shop.cust", depth + 1, true));
      }
      const GDEFSTOR_QueryNode * child = Node_(b, GDEFSTOR_QUERY_AND, kids);
      const GDEFSTOR_QueryNode * e = Node_(b, GDEFSTOR_QUERY_EXISTS, {child});
      ((GDEFSTOR_QueryNode *)e)->exists_type = "shop.cust";
      ((GDEFSTOR_QueryNode *)e)->exists_type_len = 9;
      return e;
    }
    if (pick == 9) {
      return Node_(b, Chance(2) ? GDEFSTOR_QUERY_TRUE : GDEFSTOR_QUERY_FALSE, {});
    }
    return Leaf_(b, type_name, in_exists);
  }

  /* A whole query over @p type_name. */
  void Make(Built & b, const char * type_name) {
    std::memset(&b.q, 0, sizeof b.q);
    b.q.size = sizeof b.q;
    /* One language per query, "en" most often (every entity has it). */
    static const char * const langs[] = {"en", "en", "fr", "de"};
    b.q.language = langs[lang_rng() % 4];
    b.q.language_len = 2;
    b.q.where = Chance(8) ? nullptr : Pred(b, type_name, 0, false);
    int nsorts = Pick(4);
    for (int k = 0; k < nsorts; k++) {
      GDEFSTOR_QuerySort s;
      std::memset(&s, 0, sizeof s);
      int kind = Pick(3);
      if (kind == 0) {
        const Leaf * l = nullptr;
        while (l == nullptr) {
          const Leaf & c = LeafOf(type_name);
          if (c.root_single) {
            l = &c;
          }
        }
        s.key = FieldOp(*l);
      } else if (kind == 1) {
        static const int metas[] = {GDEFSTOR_META_CREATED, GDEFSTOR_META_CREATOR,
            GDEFSTOR_META_REVISION, GDEFSTOR_META_PUBLISHED, GDEFSTOR_META_DELETED};
        s.key = Op(GDEFSTOR_QUERY_META);
        s.key.meta = metas[Pick(5)];
      } else {
        s.key = Op(GDEFSTOR_QUERY_ENTITY);
      }
      s.descending = Pick(2);
      b.sorts.push_back(s);
    }
    b.q.sorts = b.sorts.empty() ? nullptr : b.sorts.data();
    b.q.sort_count = b.sorts.size();
    static const uint64_t limits[] = {0, 1, 2, 3, 5, 2000};
    static const uint64_t offsets[] = {0, 0, 0, 1, 2, 7, 100};
    b.q.limit = limits[Pick(6)];
    b.q.offset = offsets[Pick(7)];
  }
};

}  // namespace qgen

#endif /* GDEFSSQLITE_TESTS_QUERY_GEN_H */
