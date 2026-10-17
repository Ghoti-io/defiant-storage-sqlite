#!/bin/sh
#
# Shared by the check-*.sh gates that plant a defect in the library source.
# Sourced, not run.
#
# A gate that only shows its test can fail (an `if` inside the test body)
# says nothing about the library. These plants edit a scratch COPY of the
# source by an exact text edit, rebuild, and require that a named test then
# fails. The rules the helpers enforce:
#
#  - the unpatched copy passes the named tests first (the control), and the
#    run really ran tests, so a filter that matches nothing is not a pass;
#  - every anchor must match exactly once, and the edited file must differ
#    from the original. An edit that did not apply leaves a copy that passes
#    for the wrong reason, so it stops the script with a loud error;
#  - the red run must be a test failure BY NAME. A build error, a crash, a
#    timeout or some other test failing does not count;
#  - nothing here is in the library: no macro or environment variable in
#    src/ selects a defect.
#
# Usage, from the library root:
#
#   . tools/plant-lib.sh
#   plant_setup "$MAKE" "$BUILD_DIR" PREFIX=...
#   plant_control testOracle 'Oracle.*'
#   plant NAME src/access/access.c testOracle 'Oracle.Fields' \
#       'old text' 'new text' ['old2' 'new2' ...]
#   plant_finish

set -u

unset MAKEFLAGS MFLAGS

plant_setup() {
  PLANT_MAKE="${1:?plant_setup <make> <build-dir> [make args]}"
  PLANT_BUILD="${2:?plant_setup <make> <build-dir> [make args]}"
  shift 2
  PLANT_ARGS="-j${GHOTI_CHECK_JOBS:-$(n=$(nproc 2>/dev/null || echo 4); [ "$n" -gt 4 ] && n=4; echo "$n")} $*"
  PLANT_ROOT=$(pwd)
  PLANT_SCRATCH="$PLANT_ROOT/build/plants-$$"
  PLANT_FAIL=0
  PLANT_CONTROL_DONE=0
  case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) PLANT_EXE_EXT=.exe ;; *) PLANT_EXE_EXT= ;; esac
  rm -rf "$PLANT_SCRATCH"
  mkdir -p "$PLANT_SCRATCH"
  trap 'rm -rf "$PLANT_SCRATCH"' EXIT
}

plant_copy_tree() {
  dest=$1
  mkdir -p "$dest"
  for d in src include tests tools pkgconfig documentation examples; do
    [ -e "$PLANT_ROOT/$d" ] && cp -a "$PLANT_ROOT/$d" "$dest/"
  done
  cp -a "$PLANT_ROOT/Makefile" "$PLANT_ROOT/Doxyfile" "$dest/"
  rm -rf "$dest/tests/fuzz/corpus"
}

# plant_run <dir> <exe> <filter> <log>: build the one test program in <dir>
# and run the tests matching <filter>. Exit status is the test run's.
plant_run() {
  dir=$1 exe=$2 filter=$3 log=$4
  ( cd "$dir" && GTEST_FILTER="$filter" "$PLANT_MAKE" --no-print-directory test \
      TEST_GATES= TEST_EXECUTABLES="$PLANT_BUILD/apps/$exe$PLANT_EXE_EXT" \
      $PLANT_ARGS ) > "$log" 2>&1
}

# plant_control <exe> <filter>: the unpatched copy must pass and run tests.
plant_control() {
  exe=$1 filter=$2
  if [ "$PLANT_CONTROL_DONE" -eq 0 ]; then
    plant_copy_tree "$PLANT_SCRATCH/control"
    PLANT_CONTROL_DONE=1
  fi
  log="$PLANT_SCRATCH/control-$exe.log"
  if ! plant_run "$PLANT_SCRATCH/control" "$exe" "$filter" "$log"; then
    printf 'plant: the CONTROL (unpatched) copy fails %s %s, so no plant below would mean anything. Its log:\n' \
      "$exe" "$filter" >&2
    tail -40 "$log" >&2
    exit 1
  fi
  if ! grep -Eq '\[  PASSED  \] [1-9][0-9]* test' "$log"; then
    printf 'plant: the CONTROL run of %s %s passed but ran no tests (a filter that matches nothing)\n' \
      "$exe" "$filter" >&2
    exit 1
  fi
  printf 'plant: control passes: %s %s\n' "$exe" "$filter"
}

# plant <name> <file> <exe> <expect> <old> <new> [<old> <new> ...]
# <expect> is one or more gtest names, separated by spaces.
plant() {
  name=$1 file=$2 exe=$3 expect=$4
  shift 4
  dir="$PLANT_SCRATCH/$name"
  cp -a "$PLANT_SCRATCH/control" "$dir"
  python3 - "$dir/$file" "$@" <<'PY'
import sys
path = sys.argv[1]
pairs = sys.argv[2:]
if len(pairs) == 0 or len(pairs) % 2 != 0:
    sys.stderr.write("plant: edits must come in old/new pairs\n")
    sys.exit(2)
text = open(path).read()
for i in range(0, len(pairs), 2):
    old, new = pairs[i], pairs[i + 1]
    n = text.count(old)
    if n != 1:
        sys.stderr.write("plant: anchor matched %d times in %s: %r\n" % (n, path, old))
        sys.exit(3)
    if old == new:
        sys.stderr.write("plant: an edit that changes nothing: %r\n" % old)
        sys.exit(3)
    text = text.replace(old, new, 1)
open(path, "w").write(text)
PY
  rc=$?
  if [ $rc -ne 0 ]; then
    printf 'plant: %s: THE EDIT DID NOT APPLY, nothing was tested\n' "$name" >&2
    PLANT_FAIL=1
    return
  fi
  if cmp -s "$PLANT_ROOT/$file" "$dir/$file"; then
    printf 'plant: %s: the edited file equals the original, nothing was tested\n' "$name" >&2
    PLANT_FAIL=1
    return
  fi
  # The copy was cp -a'd, so its objects are as new as the source: touch the
  # one file that changed, and drop the test program so it relinks.
  touch "$dir/$file"
  rm -f "$dir/$PLANT_BUILD"/apps/test*
  filter=$(printf '%s' "$expect" | tr ' ' ':')
  log="$dir.log"
  if plant_run "$dir" "$exe" "$filter" "$log"; then
    printf 'plant: %s: %s PASSED with the defect planted\n' "$name" "$expect" >&2
    PLANT_FAIL=1
    return
  fi
  if ! grep -Eq '\[==========\] .* ran\.' "$log"; then
    printf 'plant: %s: the run failed without finishing (a build error or a crash), which is not a test catching a defect - read %s\n' \
      "$name" "$log" >&2
    tail -30 "$log" >&2
    PLANT_FAIL=1
    return
  fi
  for t in $expect; do
    if ! grep -Eq "\[  FAILED  \] $t( |\$)" "$log"; then
      printf 'plant: %s: the run failed, but %s was not among the failures - read %s\n' \
        "$name" "$t" "$log" >&2
      grep -E '\[  FAILED  \]' "$log" >&2
      PLANT_FAIL=1
      return
    fi
    printf 'plant: %s: caught by %s\n' "$name" "$t"
    grep -E "\[  FAILED  \] $t( |\$)" "$log" | head -1 | sed 's/^/plant:   /'
  done
}

# plantm <name> <exe> <expect> <file> <old> <new> [<file> <old> <new> ...]
# A plant whose defect needs edits in more than one file (a variable one file
# sets and another reads). Every anchor must match exactly once in its file;
# every file named must differ from the original afterwards.
plantm() {
  name=$1 exe=$2 expect=$3
  shift 3
  dir="$PLANT_SCRATCH/$name"
  cp -a "$PLANT_SCRATCH/control" "$dir"
  python3 - "$dir" "$@" <<'PY'
import sys
root = sys.argv[1]
args = sys.argv[2:]
if len(args) == 0 or len(args) % 3 != 0:
    sys.stderr.write("plantm: edits must come in file/old/new triples\n")
    sys.exit(2)
for i in range(0, len(args), 3):
    path = root + "/" + args[i]
    old, new = args[i + 1], args[i + 2]
    text = open(path).read()
    n = text.count(old)
    if n != 1:
        sys.stderr.write("plantm: anchor matched %d times in %s: %r\n" % (n, path, old))
        sys.exit(3)
    if old == new:
        sys.stderr.write("plantm: an edit that changes nothing: %r\n" % old)
        sys.exit(3)
    open(path, "w").write(text.replace(old, new, 1))
PY
  rc=$?
  if [ $rc -ne 0 ]; then
    printf 'plant: %s: THE EDIT DID NOT APPLY, nothing was tested\n' "$name" >&2
    PLANT_FAIL=1
    return
  fi
  files=$(python3 - "$@" <<'PY'
import sys
a = sys.argv[1:]
print(" ".join(sorted(set(a[i] for i in range(0, len(a), 3)))))
PY
)
  for file in $files; do
    if cmp -s "$PLANT_ROOT/$file" "$dir/$file"; then
      printf 'plant: %s: %s equals the original, nothing was tested\n' "$name" "$file" >&2
      PLANT_FAIL=1
      return
    fi
    touch "$dir/$file"
  done
  rm -f "$dir/$PLANT_BUILD"/apps/test*
  filter=$(printf '%s' "$expect" | tr ' ' ':')
  log="$dir.log"
  if plant_run "$dir" "$exe" "$filter" "$log"; then
    printf 'plant: %s: %s PASSED with the defect planted\n' "$name" "$expect" >&2
    PLANT_FAIL=1
    return
  fi
  if ! grep -Eq '\[==========\] .* ran\.' "$log"; then
    printf 'plant: %s: the run failed without finishing (a build error or a crash), which is not a test catching a defect - read %s\n' \
      "$name" "$log" >&2
    tail -30 "$log" >&2
    PLANT_FAIL=1
    return
  fi
  for t in $expect; do
    if ! grep -Eq "\[  FAILED  \] $t( |\$)" "$log"; then
      printf 'plant: %s: the run failed, but %s was not among the failures - read %s\n' \
        "$name" "$t" "$log" >&2
      grep -E '\[  FAILED  \]' "$log" >&2
      PLANT_FAIL=1
      return
    fi
    printf 'plant: %s: caught by %s\n' "$name" "$t"
    grep -E "\[  FAILED  \] $t( |\$)" "$log" | head -1 | sed 's/^/plant:   /'
  done
}

plant_finish() {
  if [ "$PLANT_FAIL" -ne 0 ]; then
    exit 1
  fi
  printf 'plant: every planted defect turned its named test red\n'
}
