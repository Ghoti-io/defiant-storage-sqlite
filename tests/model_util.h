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
 * What the entity tests of this backend share. Declaring types, building and
 * saving trees and asking queries (tu), copied from defiant-storage-sql's
 * tests/test_util.h; and the tee (tee), copied from its tests/sql_stub.h: a
 * backend whose every query is also answered by gdefstor_model_scan(), the
 * port's reference engine, over the same rows, with any difference counted.
 */

#ifndef GDEFSSQLITE_TESTS_MODEL_UTIL_H
#define GDEFSSQLITE_TESTS_MODEL_UTIL_H

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage/model_backend.h>

#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tu {

const char * const kTenant = "t1";

/* ---- declarations --------------------------------------------------------- */

struct F {
  std::string name;
  int type = GDEFSTOR_FIELD_TEXT;
  unsigned flags = 0;
  int scale = 0;
  std::vector<F> kids;
  std::string subject;
  bool has_subject = false;
};

inline F Field(const std::string & name, int type, unsigned flags = 0,
    int scale = 0) {
  F f;
  f.name = name;
  f.type = type;
  f.flags = flags;
  f.scale = scale;
  return f;
}

inline F Group(const std::string & name, unsigned flags, std::vector<F> kids) {
  F f;
  f.name = name;
  f.type = GDEFSTOR_FIELD_GROUP;
  f.flags = flags;
  f.kids = std::move(kids);
  return f;
}

struct SpecArena {
  std::vector<std::vector<GDEFSTOR_FieldSpec>> lists;
  const GDEFSTOR_FieldSpec * Make(const std::vector<F> & fs) {
    lists.emplace_back();
    size_t at = lists.size() - 1;
    lists[at].resize(fs.size());
    for (size_t i = 0; i < fs.size(); i++) {
      GDEFSTOR_FieldSpec s;
      std::memset(&s, 0, sizeof s);
      s.size = sizeof s;
      s.name = fs[i].name.c_str();
      s.name_len = fs[i].name.size();
      s.type = fs[i].type;
      s.scale = fs[i].scale;
      s.flags = fs[i].flags;
      if (fs[i].has_subject) {
        s.subject = fs[i].subject.c_str();
        s.subject_len = fs[i].subject.size();
      }
      if (!fs[i].kids.empty()) {
        s.children = Make(fs[i].kids);
        s.child_count = fs[i].kids.size();
      }
      lists[at][i] = s;
    }
    return lists[at].data();
  }
};

inline GDEFSTOR_Result InWrite(GDEFSTOR_Store * store,
    const std::function<GDEFSTOR_Result(GDEFSTOR_Write *)> & body,
    const char * tenant = kTenant) {
  GDEFSTOR_Write * w = nullptr;
  GDEFSTOR_Result rc = gdefstor_write_begin(store, tenant, std::strlen(tenant),
      nullptr, &w);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = body(w);
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(w);
    return rc;
  }
  return gdefstor_write_commit(w);
}

inline GDEFSTOR_Result Declare(GDEFSTOR_Store * store, const std::string & type,
    const std::vector<F> & fields, const char * tenant = kTenant) {
  SpecArena arena;
  GDEFSTOR_TypeSpec spec;
  std::memset(&spec, 0, sizeof spec);
  spec.size = sizeof spec;
  spec.name = type.c_str();
  spec.name_len = type.size();
  spec.fields = arena.Make(fields);
  spec.field_count = fields.size();
  return InWrite(store, [&](GDEFSTOR_Write * w) {
    return gdefstor_type_declare(w, &spec);
  }, tenant);
}

/* ---- values and trees ----------------------------------------------------- */

struct V {
  int type = 0;
  int64_t i = 0;
  double r = 0.0;
  std::string s;
  std::string subject;
};

inline V Txt(const std::string & s) {
  V v;
  v.type = GDEFSTOR_FIELD_TEXT;
  v.s = s;
  return v;
}
inline V Ref(const std::string & s) {
  V v = Txt(s);
  v.type = GDEFSTOR_FIELD_REFERENCE;
  return v;
}
inline V Num(int type, int64_t i) {
  V v;
  v.type = type;
  v.i = i;
  return v;
}
inline V Flt(double d) {
  V v;
  v.type = GDEFSTOR_FIELD_FLOAT;
  v.r = d;
  return v;
}
inline V Personal(int type, const std::string & cipher, const std::string & subject) {
  V v;
  v.type = type;
  v.s = cipher;
  v.subject = subject;
  return v;
}

inline GDEFSTOR_Value ValueOf(const V & v) {
  GDEFSTOR_Value x;
  std::memset(&x, 0, sizeof x);
  x.type = v.type;
  x.integer = v.i;
  x.real = v.r;
  x.bytes = (const uint8_t *)v.s.data();
  x.len = v.s.size();
  if (!v.subject.empty()) {
    x.subject = (const uint8_t *)v.subject.data();
    x.subject_len = v.subject.size();
  }
  return x;
}

struct Node {
  std::map<std::string, V> vals;
  std::vector<std::pair<std::string, Node>> kids; /* in order */
  std::vector<std::pair<std::string, V>> multi;   /* items of multiple scalars */
  Node & Set(const std::string & f, const V & v) {
    vals[f] = v;
    return *this;
  }
  Node & Add(const std::string & f) {
    kids.emplace_back(f, Node());
    return kids.back().second;
  }
  Node & Item(const std::string & f, const V & v) {
    multi.emplace_back(f, v);
    return *this;
  }
};

inline GDEFSTOR_Result Fill(GDEFSTOR_Tree * t, size_t at, const Node & n) {
  for (const auto & kv : n.vals) {
    GDEFSTOR_Value v = ValueOf(kv.second);
    GDEFSTOR_Result rc = gdefstor_tree_set(t, at, kv.first.data(),
        kv.first.size(), &v);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (const auto & m : n.multi) {
    size_t item = 0;
    GDEFSTOR_Result rc = gdefstor_tree_add(t, at, m.first.data(),
        m.first.size(), 0, &item);
    GDEFSTOR_Value v = ValueOf(m.second);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_tree_set(t, item, "", 0, &v);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (const auto & k : n.kids) {
    size_t item = 0;
    GDEFSTOR_Result rc = gdefstor_tree_add(t, at, k.first.data(),
        k.first.size(), 0, &item);
    if (rc == GDEFSTOR_OK) {
      rc = Fill(t, item, k.second);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

inline GDEFSTOR_RevisionMeta Meta() {
  GDEFSTOR_RevisionMeta m;
  std::memset(&m, 0, sizeof m);
  m.size = sizeof m;
  m.who = "u1";
  m.who_len = 2;
  m.when_ms = 1000;
  m.why = "test";
  m.why_len = 4;
  return m;
}

/* Create @p pid (when @p create) and save @p tree on @p base. */
inline GDEFSTOR_Result Put(GDEFSTOR_Store * store, const std::string & type,
    const std::string & pid, const Node & tree, uint64_t base, bool create,
    uint64_t * out_rev = nullptr, const std::string & creator = "",
    int64_t created = 5, const char * tenant = kTenant,
    const char * lang = "en") {
  return InWrite(store, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = GDEFSTOR_OK;
    if (create) {
      rc = gdefstor_entity_create(w, type.data(), type.size(), pid.data(),
          pid.size(), creator.data(), creator.size(), created, nullptr);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    GDEFSTOR_Tree * t = nullptr;
    rc = gdefstor_tree_create(nullptr, &t);
    if (rc == GDEFSTOR_OK) {
      rc = Fill(t, 0, tree);
    }
    GDEFSTOR_RevisionMeta m = Meta();
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_save(w, pid.data(), pid.size(), base, &m, lang,
          std::strlen(lang), t, out_rev);
    }
    gdefstor_tree_free(t);
    return rc;
  }, tenant);
}

inline GDEFSTOR_Result State(GDEFSTOR_Store * store, const std::string & pid,
    int pub, int del, const char * tenant = kTenant) {
  return InWrite(store, [&](GDEFSTOR_Write * w) {
    return gdefstor_entity_set_state(w, pid.data(), pid.size(), pub, del, "a1",
        2, 7, nullptr);
  }, tenant);
}

/* Ask: the public ids of the page, in order; the total through *out_total. */
inline GDEFSTOR_Result Ask(GDEFSTOR_Store * store, const std::string & type,
    const GDEFSTOR_Query & q, std::vector<std::string> * out_ids,
    uint64_t * out_total, const char * tenant = kTenant) {
  GDEFSTOR_QueryResult * r = nullptr;
  GDEFSTOR_Result rc = gdefstor_entity_query(store, tenant, std::strlen(tenant),
      type.data(), type.size(), &q, &r);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  out_ids->clear();
  for (size_t i = 0; i < gdefstor_query_result_rows(r); i++) {
    const char * p = nullptr;
    size_t n = 0;
    gdefstor_query_result_entity(r, i, &p, &n);
    out_ids->push_back(std::string(p, n));
  }
  *out_total = gdefstor_query_result_total(r);
  gdefstor_query_result_free(r);
  return GDEFSTOR_OK;
}

}  // namespace tu

namespace tee {

/* ---- the tee: every query also answered by the reference scan ------------ */

struct Tee {
  GDEFSTOR_ModelBackend inner;
  long queries = 0;
  long disagreements = 0;
  long refusals = 0;
  std::string first_disagreement;
  std::function<void(const std::string &)> on_disagree;
};

struct ScanCtx {
  Tee * tee;
  const char * tenant;
  size_t tenant_len;
};

struct ScanFilter {
  const GDEFSTOR_ModelVisitor * to;
  std::string type;
  bool on = false;
};

inline GDEFSTOR_Result SF_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  ScanFilter * f = (ScanFilter *)ctx;
  f->on = std::string(e->type, e->type_len) == f->type;
  return f->on ? f->to->entity(f->to->ctx, e) : GDEFSTOR_OK;
}

inline GDEFSTOR_Result SF_item(void * ctx, uint64_t row, const GDEFSTOR_ModelItem * i) {
  ScanFilter * f = (ScanFilter *)ctx;
  return f->on ? f->to->item(f->to->ctx, row, i) : GDEFSTOR_OK;
}

/* gdefstor_model_scan()'s view of the store: the current tables of the last
 * commit, through the backend's own dump of them. Entities come grouped
 * with their items, as the scan wants, because the dump visits an entity's
 * type's items after its entities: so collect first. */
struct Collected {
  struct B {
    GDEFSTOR_ModelBlind b;
    std::string subject, mac, lang;
  };
  struct E {
    GDEFSTOR_ModelEntity e;
    std::string type, pid, creator;
    std::vector<GDEFSTOR_ModelItem> items;
    std::vector<std::string> langs;
    std::vector<std::vector<GDEFSTOR_Value>> vals;
    std::vector<std::vector<std::string>> bytes;
    std::vector<std::vector<std::string>> subjects;
    std::vector<B> blinds;
  };
  std::map<uint64_t, E> ents;
};

inline GDEFSTOR_Result C_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  Collected * c = (Collected *)ctx;
  Collected::E & x = c->ents[e->row];
  x.e = *e;
  x.type.assign(e->type, e->type_len);
  x.pid.assign(e->public_id, e->public_id_len);
  x.creator.assign(e->creator, e->creator_len);
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result C_item(void * ctx, uint64_t row, const GDEFSTOR_ModelItem * i) {
  Collected * c = (Collected *)ctx;
  Collected::E & x = c->ents[row];
  x.items.push_back(*i);
  x.langs.push_back(std::string(i->language, i->language_len));
  std::vector<GDEFSTOR_Value> v(i->values, i->values + i->value_count);
  /* Copies of what the dump lends: bytes and subjects both, since the
   * lent ones are gone once the dump returns. */
  std::vector<std::string> b;
  std::vector<std::string> subj;
  for (auto & one : v) {
    b.push_back(one.len == 0 ? std::string()
                             : std::string((const char *)one.bytes, one.len));
    subj.push_back(one.subject_len == 0
                       ? std::string()
                       : std::string((const char *)one.subject, one.subject_len));
  }
  x.vals.push_back(v);
  x.bytes.push_back(b);
  x.subjects.push_back(subj);
  return GDEFSTOR_OK;
}

/* Blind rows are truth, not current: they come from the truth dump. */
inline GDEFSTOR_Result C_blind(void * ctx, uint64_t row, const GDEFSTOR_ModelBlind * b) {
  Collected * c = (Collected *)ctx;
  auto it = c->ents.find(row);
  if (it == c->ents.end()) {
    return GDEFSTOR_ERR_CORRUPT; /* a blind row of no entity */
  }
  Collected::B x;
  x.b = *b;
  x.subject.assign((const char *)b->subject, b->subject_len);
  x.mac.assign((const char *)b->mac, GDEFSTOR_BLIND_MAC_BYTES);
  x.lang.assign(b->language, b->language_len);
  it->second.blinds.push_back(x);
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result TeeScan(void * ctx, const char * type, size_t type_len,
    const GDEFSTOR_ModelVisitor * visitor) {
  ScanCtx * s = (ScanCtx *)ctx;
  Collected col;
  GDEFSTOR_ModelVisitor v;
  std::memset(&v, 0, sizeof v);
  v.ctx = &col;
  v.entity = C_entity;
  v.item = C_item;
  GDEFSTOR_Result rc = s->tee->inner.dump(s->tee->inner.ctx, s->tenant,
      s->tenant_len, GDEFSTOR_MODEL_CURRENT, &v);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (visitor->blind != nullptr) {
    GDEFSTOR_ModelVisitor bv;
    std::memset(&bv, 0, sizeof bv);
    bv.ctx = &col;
    bv.blind = C_blind;
    rc = s->tee->inner.dump(s->tee->inner.ctx, s->tenant, s->tenant_len,
        GDEFSTOR_MODEL_TRUTH, &bv);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  std::string want(type, type_len);
  for (auto & kv : col.ents) {
    Collected::E & x = kv.second;
    if (x.type != want) {
      continue;
    }
    x.e.type = x.type.data();
    x.e.public_id = x.pid.data();
    x.e.creator = x.creator.data();
    rc = visitor->entity(visitor->ctx, &x.e);
    for (size_t i = 0; rc == GDEFSTOR_OK && i < x.items.size(); i++) {
      for (size_t k = 0; k < x.vals[i].size(); k++) {
        x.vals[i][k].bytes = (const uint8_t *)x.bytes[i][k].data();
        x.vals[i][k].subject = (const uint8_t *)x.subjects[i][k].data();
      }
      x.items[i].values = x.vals[i].data();
      x.items[i].language = x.langs[i].data();
      rc = visitor->item(visitor->ctx, x.e.row, &x.items[i]);
    }
    for (size_t i = 0; rc == GDEFSTOR_OK && visitor->blind != nullptr &&
                       i < x.blinds.size(); i++) {
      x.blinds[i].b.subject = (const uint8_t *)x.blinds[i].subject.data();
      x.blinds[i].b.mac = (const uint8_t *)x.blinds[i].mac.data();
      x.blinds[i].b.language = x.blinds[i].lang.data();
      rc = visitor->blind(visitor->ctx, x.e.row, &x.blinds[i].b);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

inline std::string HitsText(const GDEFSTOR_ModelHits & h) {
  std::string s = "total " + std::to_string(h.total) + ":";
  for (size_t i = 0; i < h.count; i++) {
    s += " " + std::string(h.public_ids[i], h.public_id_lens[i]) + "#" +
         std::to_string(h.rows[i]);
  }
  return s;
}

inline GDEFSTOR_Result Tee_query(void * ctx, const char * tenant, size_t tlen,
    const GDEFSTOR_ModelPlan * plan, GDEFSTOR_ModelHits * out) {
  Tee * t = (Tee *)ctx;
  GDEFSTOR_Result rc = t->inner.query(t->inner.ctx, tenant, tlen, plan, out);
  t->queries += 1;
  if (rc != GDEFSTOR_OK) {
    if (rc == GDEFSTOR_ERR_UNSUPPORTED || rc == GDEFSTOR_ERR_LIMIT) {
      t->refusals += 1;
    }
    return rc;
  }
  GDEFSTOR_ModelHits ref;
  std::memset(&ref, 0, sizeof ref);
  ScanCtx sc{t, tenant, tlen};
  GDEFSTOR_Result rr = gdefstor_model_scan(plan, TeeScan, &sc, &ref);
  if (rr != GDEFSTOR_OK) {
    /* The reference could not answer (an allocation failed): no verdict. */
    gdefstor_model_hits_release(plan->alloc, &ref);
    return rc;
  }
  std::string a = HitsText(*out);
  std::string b = HitsText(ref);
  if (a != b) {
    t->disagreements += 1;
    if (t->first_disagreement.empty()) {
      t->first_disagreement = "sql " + a + "\nref " + b;
    }
    if (t->on_disagree) {
      t->on_disagree("sql " + a + "\nref " + b);
    }
  }
  gdefstor_model_hits_release(plan->alloc, &ref);
  return rc;
}

#define TEE_FWD(name, ret, params, args)                                   \
  inline ret Tee_##name params {                                               \
    Tee * t = (Tee *)ctx;                                                      \
    return t->inner.name args;                                                 \
  }
TEE_FWD(begin, GDEFSTOR_Result, (void * ctx, const char * a, size_t b, void ** c),
    (t->inner.ctx, a, b, c))
TEE_FWD(commit, GDEFSTOR_Result, (void * ctx, void * x), (t->inner.ctx, x))
TEE_FWD(abort, GDEFSTOR_Result, (void * ctx, void * x), (t->inner.ctx, x))
TEE_FWD(type_load, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, const char * c, size_t d, uint8_t ** e, size_t * f),
    (t->inner.ctx, x, a, b, c, d, e, f))
TEE_FWD(type_store, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_Type * a, const uint8_t * b, size_t c), (t->inner.ctx, x, a, b, c))
TEE_FWD(entity_find, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, const char * c, size_t d, const GDEFSTOR_ModelVisitor * v),
    (t->inner.ctx, x, a, b, c, d, v))
TEE_FWD(entity_insert, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_ModelEntity * e, uint64_t * r), (t->inner.ctx, x, e, r))
TEE_FWD(state_append, GDEFSTOR_Result, (void * ctx, void * x, uint64_t r,
    const GDEFSTOR_ModelState * s), (t->inner.ctx, x, r, s))
TEE_FWD(apply, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_ModelApply * a), (t->inner.ctx, x, a))
TEE_FWD(history, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, uint64_t r, const GDEFSTOR_ModelVisitor * v), (t->inner.ctx, x, a, b, r, v))
TEE_FWD(items, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, uint64_t r, uint64_t o, const char * l, size_t ll,
    const GDEFSTOR_ModelVisitor * v), (t->inner.ctx, x, a, b, r, o, l, ll, v))
TEE_FWD(draft_put, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_ModelDraft * d), (t->inner.ctx, x, d))
TEE_FWD(draft_get, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, const char * c, size_t d, const GDEFSTOR_ModelVisitor * v),
    (t->inner.ctx, x, a, b, c, d, v))
TEE_FWD(draft_delete, GDEFSTOR_Result, (void * ctx, void * x,
    const char * a, size_t b), (t->inner.ctx, x, a, b))
TEE_FWD(rebuild, GDEFSTOR_Result, (void * ctx, void * x), (t->inner.ctx, x))
TEE_FWD(dump, GDEFSTOR_Result, (void * ctx, const char * a, size_t b, int w,
    const GDEFSTOR_ModelVisitor * v), (t->inner.ctx, a, b, w, v))
#undef TEE_FWD

inline void Tee_destroy(void * ctx) {
  Tee * t = (Tee *)ctx;
  t->inner.destroy(t->inner.ctx);
}

/* A backend that is @p inner with every query cross-checked. The Tee is the
 * caller's and must outlive the store. */
inline GDEFSTOR_ModelBackend TeeOf(Tee * t, const GDEFSTOR_ModelBackend & inner) {
  t->inner = inner;
  GDEFSTOR_ModelBackend b = inner;
  b.ctx = t;
  b.destroy = Tee_destroy;
  b.begin = Tee_begin;
  b.commit = Tee_commit;
  b.abort = Tee_abort;
  b.type_load = Tee_type_load;
  b.type_store = Tee_type_store;
  b.entity_find = Tee_entity_find;
  b.entity_insert = Tee_entity_insert;
  b.state_append = Tee_state_append;
  b.apply = Tee_apply;
  b.history = Tee_history;
  b.items = Tee_items;
  b.draft_put = Tee_draft_put;
  b.draft_get = Tee_draft_get;
  b.draft_delete = Tee_draft_delete;
  b.query = Tee_query;
  b.rebuild = Tee_rebuild;
  b.dump = Tee_dump;
  return b;
}

}  // namespace tee

#endif /* GDEFSSQLITE_TESTS_MODEL_UTIL_H */
