#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the statement the SQLite backend builds for
# a query turns a named test red. The plants edit src/sqlite.c in a scratch
# copy; the tests are the port's conformance suite (testConformance, which
# checks the answers against a model written apart from the library) and
# testQuery (what only this backend can show).
#
# Usage: check-query.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-query.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-query.sh <make> <build-dir> [make args]}"
shift 2

. tools/plant-lib.sh
plant_setup "$MAKE" "$BUILD_DIR" "$@"
plant_control testConformance 'Query.*'
plant_control testQuery 'QueryDir.*'

# A clause of an AND (or OR) left out of the statement.
plant sql-drops-a-clause src/sqlite.c testConformance \
  'Query.AndOrNotCombineEveryClause Query.Generated' \
  '    emit_chain(q, n, mid, hi);' \
  '    sb_str(&q->sql, n->op == GDEFSTOR_QUERY_AND ? "1" : "0");'

# The range of the type with no upper end: a type whose hex begins with
# another'"'"'s, and every type after it, is read as the type.
plant range-without-upper-bound src/sqlite.c testConformance \
  'Query.TypeNamesAreNotPatterns' \
  '  sb_str(&q->sql, " AND r.key < ");
  qb_bind(q, kBindText, hi, hi_len, 0);' \
  '  sb_str(&q->sql, " AND r.key < ");
  qb_bind(q, kBindText, hi_len > 0 ? "\x7f" : hi, 1, 0);'

# The joined row looked up at the wrong place in the key.
plant join-at-wrong-offset src/sqlite.c testConformance \
  'Query.JoinedRowIsTheSameEntityOfAnotherType' \
  '  qb_bind(q, kBindInt, NULL, 0, (sqlite3_int64)q->prefix_len + 1);' \
  '  qb_bind(q, kBindInt, NULL, 0, (sqlite3_int64)q->prefix_len);'

# The integer ordering of the entity tiebreak left off.
plant tiebreak-as-bytes src/sqlite.c testConformance \
  'Query.TiebreakIsTheEntityArgument' \
  '    if (query->entity_numeric) {
      tie[0].numeric = 1;' \
  '    if (0) {
      tie[0].numeric = 1;'

# DESC never written.
plant descending-ignored src/sqlite.c testConformance \
  'Query.Generated' \
  '      if (s->descending) {' \
  '      if (0) {'

# The total counted without the predicate: every row of the type.
plant total-before-filter src/sqlite.c testConformance \
  'Query.CountIsOnlyWhatMatches Query.Generated' \
  '  sb_str(&q.sql, "SELECT count(*) FROM defiant_record r");
  emit_range(&q, plan->prefix, plan->prefix_len, hi, plan->prefix_len);
  if (query->where != NULL) {' \
  '  sb_str(&q.sql, "SELECT count(*) FROM defiant_record r");
  emit_range(&q, plan->prefix, plan->prefix_len, hi, plan->prefix_len);
  if (0) {'

# The offset not applied.
plant offset-ignored src/sqlite.c testConformance \
  'Query.PagesAreSlicesOfOneOrder Query.Generated' \
  '        query->offset > (uint64_t)INT64_MAX ? INT64_MAX
                                            : (sqlite3_int64)query->offset);' \
  '        0);'

# A constant written into the statement in quotes instead of bound.
plant constant-concatenated src/sqlite.c testConformance \
  'Query.ConstantsAreMatchedLiterally' \
  '    qb_bind(q, kBindBlob, o->texts[i].bytes, o->texts[i].len, 0);' \
  "    sb_str(&q->sql, \"'\");
    sb_add(&q->sql, (const char *)o->texts[i].bytes, o->texts[i].len);
    sb_str(&q->sql, \"'\");"

# The backend not offering its engine: the port scans, and the answers stay
# right, which is why the suite alone could not tell.
plant engine-not-offered src/sqlite.c testQuery \
  'QueryDir.PushedDownIsUsed' \
  '  backend.query = bag_query;' \
  '  backend.query = NULL;'

# The limit on bound values ignored: the statement is refused by SQLite and the
# query fails instead of being handed back to the port.
plant bind-limit-ignored src/sqlite.c testQuery \
  'QueryDir.ATreeTooWideForOneStatementIsScannedByThePort' \
  '  if (q->nbinds > (size_t)q->max_vars) {
    q->unsupported = 1;
  }' \
  '  (void)q->max_vars;'

plant_finish
