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
# the gate refuses one whose rebuild did not happen.

set -u

MAKE_CMD="${1:?usage: check-defects.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-defects.sh <make> <build-dir> [make args]}"
shift 2
MARGS="-j${GHOTI_CHECK_JOBS:-$(n=$(nproc 2>/dev/null || echo 4); [ "$n" -gt 4 ] && n=4; echo "$n")} $*"

# The helpers are defiant-storage's, installed with its conformance suites;
# the Makefile names the file.
PLANT_LIB="${GHOTI_PLANT_LIB:-}"
if [ ! -r "$PLANT_LIB" ]; then
  printf 'check-defects: defiant-storage installs the plant helpers (plant-lib.sh) with its conformance suites and none was found at "%s": install defiant-storage first, then run this through make (GHOTI_PLANT_LIB names the file)\n' \
    "$PLANT_LIB" >&2
  exit 1
fi
. "$PLANT_LIB"
PLANT_BUILD="$BUILD_DIR"

unset MAKEFLAGS MFLAGS

ROOT=$(pwd)
# A Windows build names its test programs with an extension.
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) EXE_EXT=.exe ;; *) EXE_EXT= ;; esac
SCRATCH="$ROOT/build/defects"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

copy_tree() {
  dest=$1
  mkdir -p "$dest"
  for d in src include tests tools pkgconfig documentation examples; do
    [ -e "$ROOT/$d" ] && cp -a "$ROOT/$d" "$dest/"
  done
  cp -a "$ROOT/Makefile" "$ROOT/Doxyfile" "$dest/"
  rm -rf "$dest/tests/fuzz/corpus"
  plant_warm "$ROOT" "$dest" "$BUILD_DIR"
}

fail=0

copy_tree "$SCRATCH/control"
if ! ( cd "$SCRATCH/control" && "$MAKE_CMD" --no-print-directory test TEST_GATES= $MARGS ) \
    > "$SCRATCH/control.log" 2>&1; then
  printf 'check-defects: the CONTROL (unpatched) copy fails its own tests, so no\n' >&2
  printf 'result below would mean anything. Its log:\n' >&2
  tail -40 "$SCRATCH/control.log" >&2
  exit 1
fi
printf 'check-defects: control passes\n'

# plant <name> <file> <exe> <expect> <old> <new> [stale]: with a seventh
# argument, the objects that depend on the edit are made to look up to date,
# and the plant is a success only if that is refused as a stale object.
plant() {
  name=$1 file=$2 exe=$3 expect=$4 old=$5 new=$6 stale=${7:-}
  dir="$SCRATCH/$name"
  cp -a "$SCRATCH/control" "$dir"
  python3 - "$dir/$file" "$old" "$new" <<'PY'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if text.count(old) != 1:
    sys.stderr.write("anchor matched %d times in %s\n" % (text.count(old), path))
    sys.exit(3)
open(path, "w").write(text.replace(old, new, 1))
PY
  rc=$?
  if [ $rc -ne 0 ]; then
    printf 'check-defects: %s: THE PATCH DID NOT APPLY (stale anchor), nothing was tested\n' "$name" >&2
    fail=1
    return
  fi
  if cmp -s "$ROOT/$file" "$dir/$file"; then
    printf 'check-defects: %s: the patched file equals the original\n' "$name" >&2
    fail=1
    return
  fi
  touch "$dir/$file"
  if [ -n "$stale" ]; then
    for o in $(plant_depends "$dir" "$file"); do
      ( cd "$dir" && touch "$o" )
    done
  fi
  rm -f "$dir/$BUILD_DIR"/apps/test* "$dir/$BUILD_DIR"/apps/*.a \
    "$dir/$BUILD_DIR"/apps/*.so*
  ( cd "$dir" && "$MAKE_CMD" --no-print-directory test TEST_GATES= \
      TEST_EXECUTABLES="$BUILD_DIR/apps/$exe$EXE_EXT" $MARGS ) > "$dir.log" 2>&1
  made=$?
  stale_objs=$(plant_stale_objects "$dir" "$dir.log" "$file")
  if [ -n "$stale" ]; then
    if [ -n "$stale_objs" ]; then
      printf 'check-defects: %s: caught by the stale-object check: %s edited, and not rebuilt\n' \
        "$name" "$file"
    else
      printf 'check-defects: %s: every object that depends on the edit was rebuilt anyway, so the stale-object check was not shown to work\n' \
        "$name" >&2
      fail=1
    fi
    rm -rf "$dir"
    return
  fi
  if [ -n "$stale_objs" ]; then
    printf 'check-defects: %s: STALE OBJECT: the edit was not rebuilt into the test program (%s), so the defect never reached the tests - read %s\n' \
      "$name" "$(printf '%s' "$stale_objs" | tr '\n' ' ')" "$dir.log" >&2
    fail=1
  elif [ $made -eq 0 ]; then
    printf 'check-defects: %s: the tests PASSED with the defect planted\n' "$name" >&2
    fail=1
  elif grep -q "FAILED.*$expect" "$dir.log"; then
    printf 'check-defects: %s: caught by %s\n' "$name" "$expect"
  else
    printf 'check-defects: %s: the run failed, but not in %s - read %s\n' \
      "$name" "$expect" "$dir.log" >&2
    tail -30 "$dir.log" >&2
    fail=1
  fi
}

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

plant step-loses-a-row src/model.c testMigration "$STEP_TEST" \
  "$STEP_OLD" "$STEP_NEW"

# The stale-object plant: the same edit, with the rebuild that should follow
# it made not to happen, which has to be refused as a STALE OBJECT. If it ever
# passes instead, a warm copy could pass with the defect never built in.
plant stale-object src/model.c testMigration "$STEP_TEST" \
  "$STEP_OLD" "$STEP_NEW" stale

if [ "$fail" -ne 0 ]; then
  exit 1
fi
printf 'check-defects: every planted defect turned its gate red\n'
