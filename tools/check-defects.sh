#!/bin/sh
#
# Show that the planted migration defect turns Migration.OldRowSurvives red.
#
# Copies the tree, proves the unpatched copy passes, then plants one defect
# by an exact textual edit and requires that test to fail by name. A patch
# that does not match exactly once is an error: a stale anchor would
# otherwise leave a pristine copy that passes.

set -u

MAKE_CMD="${1:?usage: check-defects.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-defects.sh <make> <build-dir> [make args]}"
shift 2
MARGS="-j${GHOTI_CHECK_JOBS:-$(n=$(nproc 2>/dev/null || echo 4); [ "$n" -gt 4 ] && n=4; echo "$n")} $*"

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

plant() {
  name=$1 file=$2 exe=$3 expect=$4 old=$5 new=$6
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
  rm -f "$dir/$BUILD_DIR"/apps/test* "$dir/$BUILD_DIR"/apps/*.a \
    "$dir/$BUILD_DIR"/apps/*.so*
  if ( cd "$dir" && "$MAKE_CMD" --no-print-directory test TEST_GATES= \
      TEST_EXECUTABLES="$BUILD_DIR/apps/$exe$EXE_EXT" $MARGS ) > "$dir.log" 2>&1; then
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

plant migration src/sqlite.c testMigration \
  'Migration.OldRowSurvives' \
  'static int gdefssqlite_plant_drop_row = 0; /* plant:migration */' \
  'static int gdefssqlite_plant_drop_row = 1; /* plant:migration */'

if [ "$fail" -ne 0 ]; then
  exit 1
fi
printf 'check-defects: every planted defect turned its gate red\n'
