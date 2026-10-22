#!/bin/sh
#
# The migration corpus gate (tests/test_migration.cpp): show that a schema
# step that loses a row turns
# Migration.EveryStepOnTheVersionOneCorpusKeepsEveryRow red.
#
# Copies the tree, proves the unpatched copy passes, then plants one defect
# by an exact textual edit and requires that test to fail by name. A patch
# that does not match exactly once is an error: a stale anchor would
# otherwise leave a pristine copy that passes.
#
# The copies start from the finished build (the helpers' plant_warm), so a
# plant has to show that its edit was compiled in; the last plant shows that
# the gate refuses one whose rebuild did not happen. The control runs the
# named test only, as every plant does, not the whole suite of the program.

set -u

MAKE_CMD="${1:?usage: check-defects.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-defects.sh <make> <build-dir> [make args]}"
shift 2

# The helpers are defiant-storage's, installed with its conformance suites;
# the Makefile names the file.
PLANT_LIB="${GHOTI_PLANT_LIB:-}"
if [ ! -r "$PLANT_LIB" ]; then
  printf 'check-defects: defiant-storage installs the plant helpers (plant-lib.sh) with its conformance suites and none was found at "%s": install defiant-storage first, then run this through make (GHOTI_PLANT_LIB names the file)\n' \
    "$PLANT_LIB" >&2
  exit 1
fi
. "$PLANT_LIB"
if ! command -v plant_hook_stale > /dev/null 2>&1; then
  printf 'check-defects: the plant helpers at "%s" are older than this gate needs (no plant_hook_stale): install defiant-storage again from this checkout\n' \
    "$PLANT_LIB" >&2
  exit 1
fi
PLANT_TAG=check-defects

plant_hook_control() { # exe filter
  plant_say "check-defects: control passes"
}

plant_hook_caught() { # name expect log
  printf 'check-defects: %s: caught by %s\n' "$1" "$2"
}

plant_hook_stale() { # name files objects
  printf 'check-defects: %s: caught by the stale-object check: %s edited, and not rebuilt\n' "$1" "$2"
}

plant_hook_done() {
  printf 'check-defects: every planted defect turned its gate red\n'
}

plant_setup "$MAKE_CMD" "$BUILD_DIR" "$@"

# A step that loses a row: each ADD COLUMN the executor runs also deletes the
# first row of the table it altered, as a step that rebuilt the table and
# dropped one would.
STEP_OLD='  int rc = run_plain(c->db, sql, sql_len);
  report(ex, c, sql, sql_len, NULL, 1, 0, 0);'
STEP_NEW='  int rc = run_plain(c->db, sql, sql_len);
  if (rc == SQLITE_OK && sql_len > 12 && memcmp(sql, "ALTER TABLE ", 12) == 0) {
    const char * name = sql + 12;
    const char * end = (const char *)memchr(name, '"'"' '"'"', sql_len - 12);
    char del[256];
    if (end != NULL && end - name < 64) {
      snprintf(del, sizeof del,
          "DELETE FROM %.*s WHERE rowid = (SELECT min(rowid) FROM %.*s)",
          (int)(end - name), name, (int)(end - name), name);
      rc = run_plain(c->db, del, strlen(del));
    }
  }
  report(ex, c, sql, sql_len, NULL, 1, 0, 0);'
STEP_TEST='Migration.EveryStepOnTheVersionOneCorpusKeepsEveryRow'

plant_control testMigration "$STEP_TEST"

plant step-loses-a-row src/model.c testMigration "$STEP_TEST" \
  "$STEP_OLD" "$STEP_NEW"

# The stale-object plant: the same edit, with the rebuild that should follow
# it made not to happen, which has to be refused as a STALE OBJECT. If it ever
# passes instead, a warm copy could pass with the defect never built in.
plant_stale stale-object src/model.c testMigration "$STEP_TEST" \
  "$STEP_OLD" "$STEP_NEW"

plant_finish
