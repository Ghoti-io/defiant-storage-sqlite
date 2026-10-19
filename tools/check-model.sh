#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the entity port's SQLite executor
# (src/model.c) turns a named test red: the defects story S3 names for this
# backend. Each plant edits a scratch copy of the real source
# (tools/plant-lib.sh), so nothing in src/ selects a defect.
#
# Usage: check-model.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-model.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-model.sh <make> <build-dir> [make args]}"
shift 2

. tools/plant-lib.sh
plant_setup "$MAKE" "$BUILD_DIR" "$@"
plant_control testScale 'Scale.*'
plant_control testFiles 'ModelFiles.*:ModelConcurrency.*:ModelKeyStore.*'

# A filter that scans despite an index: the field index's step reported done
# and never run. The table and its record are there; the index is not.
plant index-step-dropped src/model.c testScale \
  'Scale.EqualityUsesTheFieldIndex Scale.InUsesTheFieldIndex Scale.RangesUseTheFieldIndex' \
  '  int rc = run_plain(c->db, sql, sql_len);' \
  '  int rc = sql_len > 27 && memcmp(sql, "CREATE INDEX IF NOT EXISTS ", 27) == 0
               ? SQLITE_OK
               : run_plain(c->db, sql, sql_len);'

# A milestone-1 file opened by the v2 open: the catalog never read, so the SQL
# layer creates its tables in the old file.
plant old-file-opened src/model.c testFiles \
  'ModelFiles.MilestoneOneFileIsRefusedAndLeftAsItIs ModelFiles.TheMilestoneOneFixtureIsRefusedAndLeftAsItIs' \
  '  r = classify(ex, c, &state);' \
  '  r = GDEFSTOR_OK;
  state = FILE_EMPTY;'

# DDL issued outside its recorded step: a table of the executor's own, made
# when a write connection opens, that no dss_name row records.
plant ddl-outside-step src/model.c testFiles \
  'ModelFiles.EveryTableAndIndexIsARecordedStep' \
  '  if (r == GDEFSTOR_OK) {
    r = setup(ex, c);
  }' \
  '  if (r == GDEFSTOR_OK) {
    r = setup(ex, c);
  }
  if (r == GDEFSTOR_OK && write && !ex->keys) {
    const char * ddl = "CREATE TABLE IF NOT EXISTS \"dss_cache\" (\"k\" TEXT)";
    r = from_sqlite(run_plain(c->db, ddl, strlen(ddl)));
  }'

# secure_delete off in the key store: a destroyed key stays in a free block of
# its page, in the file.
plant secure-delete-off src/model.c testFiles \
  'ModelKeyStore.ADestroyedKeyLeavesNoBytesInTheFile' \
  '    rc = pragma_text(c->db, "PRAGMA secure_delete=ON", got, sizeof got);
    if (rc == SQLITE_OK && strcmp(got, "1") != 0) {' \
  '    rc = pragma_text(c->db, "PRAGMA secure_delete=OFF", got, sizeof got);
    if (rc == SQLITE_OK && strcmp(got, "0") != 0) {'

# The read connection sharing the write connection: a read takes the tenant's
# write connection whenever no write is using it.
plant reader-shares-writer src/model.c testFiles \
  'ModelConcurrency.ReadersUseTheirOwnConnections' \
  '  } else if (t->nidle > 0) {
    /* A read has a connection of its own: never the writer'"'"'s, which may be
     * in a write that is not committed. */' \
  '  } else if (t->writer != NULL && !t->writer->in_use) {
    c = t->writer;
    ex->idle_total -= 1;
  } else if (t->nidle > 0) {
    /* A read has a connection of its own: never the writer'"'"'s, which may be
     * in a write that is not committed. */'

# A subject lookup that scans despite the index (story S5a): the subject
# columns' index step reported done and never run, so "everything about this
# person" reads every version of the type.
plant subject-index-dropped src/model.c testScale \
  'Scale.TheSubjectLookupUsesTheSubjectIndex' \
  '  int rc = run_plain(c->db, sql, sql_len);' \
  '  const char * under = sql_len > 28 ? strchr(sql + 28, '"'"'_'"'"') : NULL;
  int rc = sql_len > 28 &&
                   memcmp(sql, "CREATE INDEX IF NOT EXISTS \"t", 28) == 0 &&
                   under != NULL && under[1] == '"'"'j'"'"'
               ? SQLITE_OK
               : run_plain(c->db, sql, sql_len);'

# The default weakened (2026-10-19): the model suite runs at OFF for speed, so
# a deployment's FULL default is held only by its own test.
plant sync-default-off src/model.c testFiles \
  'ModelFiles.CommitsWaitForTheDiskByDefault' \
  '  out->statement_cache = 64;
}' \
  '  out->statement_cache = 64;
  out->synchronous = GDEFSSQLITE_SYNC_OFF;
}'

plant_finish
