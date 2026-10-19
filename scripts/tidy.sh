#!/usr/bin/env bash
# Runs clang-tidy with the repository's .clang-tidy over the sources of native_sim builds that
# belong to the repository it runs in (a submodule's sources are left to the submodule).
#
#   scripts/tidy.sh                     lint every source of the build tracked by this repository
#   scripts/tidy.sh --changed [BASE]    only sources changed since the merge base with BASE
#                                       (default origin/main), working tree included
#   scripts/tidy.sh PATH...             only sources under these files or directories
#   scripts/tidy.sh --fix               apply the fixes clang-tidy offers; review the diff afterwards
#   scripts/tidy.sh --build             first build ./app for native_sim into the (first) -p directory
#   scripts/tidy.sh -p DIR              compile database to use (default build/tidy): DIR holds a
#                                       compile_commands.json, or is searched recursively for them
#                                       (a twister output directory has one per test suite build).
#                                       Give -p more than once to lint the sources of several builds;
#                                       the databases are merged, the first entry of a file wins.
#   scripts/tidy.sh -j N                run N clang-tidy processes at once (default 4)
#
# CLANG_TIDY=/path/to/clang-tidy selects another binary (default clang-tidy-23, then clang-tidy).
# Exit status: 0 without warnings, 1 with warnings, 2 on a usage or setup error.
set -euo pipefail

readonly CLANG_TIDY_VERSION="23" # The major version CI pins (apt.llvm.org pins the major only).
readonly INSTALL_HINT="Install from apt.llvm.org: key https://apt.llvm.org/llvm-snapshot.gpg.key to /etc/apt/trusted.gpg.d/apt.llvm.org.asc, source 'deb http://apt.llvm.org/noble/ llvm-toolchain-noble-$CLANG_TIDY_VERSION main', then apt-get install clang-tidy-$CLANG_TIDY_VERSION clang-tools-$CLANG_TIDY_VERSION."
# Generated, vendored and upstream code that lives in tracked files (rt_gauge's LVGL font and image
# converter output included).
readonly EXCLUDED='/(configuration/cbor/cbor_[a-z_]+_config|libs/nameof|lua|views/assets/(fonts|images))/'

usage() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit "${1:-0}"
}

changed_base=""
fix=false
build=false
build_dirs=()
merged_dir=""
jobs=4
paths=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --changed)
            changed_base="origin/main"
            # The next argument is the base only when it names a commit; otherwise it is a path.
            if [[ $# -gt 1 && "$2" != -* ]] && git rev-parse --verify --quiet "$2^{commit}" > /dev/null; then
                changed_base="$2"
                shift
            fi
            ;;
        --fix) fix=true ;;
        --build) build=true ;;
        -p)
            if [[ $# -lt 2 ]]; then echo "-p needs a directory." >&2; exit 2; fi
            build_dirs+=("$(realpath -m "$2")")
            shift
            ;;
        -j) jobs="${2:-}"; shift ;;
        -j*) jobs="${1#-j}" ;;
        -h|--help) usage 0 ;;
        -*) echo "Unknown option: $1" >&2; usage 2 ;;
        *) paths+=("$(realpath -m "$1")") ;;
    esac
    shift
done

if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "-j needs a positive number." >&2
    exit 2
fi

# The first of the given names found next to the clang-tidy binary, then on PATH.
find_tool() {
    local name path
    for name in "$@"; do
        for path in "$tool_dir/$name" "$name"; do
            command -v "$path" && return 0
        done
    done
    return 1
}

if [[ -n "${CLANG_TIDY:-}" ]]; then
    clang_tidy="$(command -v "$CLANG_TIDY" || true)"
else
    clang_tidy="$(command -v "clang-tidy-$CLANG_TIDY_VERSION" || command -v clang-tidy || true)"
fi
if [[ -z "$clang_tidy" ]]; then
    echo "clang-tidy not found. $INSTALL_HINT" >&2
    exit 2
fi
version="$("$clang_tidy" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if (( ${version%%.*} < CLANG_TIDY_VERSION )); then
    echo "clang-tidy $version found ($clang_tidy), $CLANG_TIDY_VERSION or later needed. $INSTALL_HINT" >&2
    exit 2
elif (( ${version%%.*} > CLANG_TIDY_VERSION )); then
    echo "Note: clang-tidy $version, CI uses $CLANG_TIDY_VERSION." >&2
fi
tool_dir="$(dirname "$clang_tidy")"
run_clang_tidy="$(find_tool "run-clang-tidy-$CLANG_TIDY_VERSION" "run-clang-tidy-$CLANG_TIDY_VERSION.py" \
    run-clang-tidy.py run-clang-tidy || true)"
if [[ -z "$run_clang_tidy" ]]; then
    echo "run-clang-tidy not found next to $clang_tidy or on PATH; it is in clang-tools-$CLANG_TIDY_VERSION." \
        "$INSTALL_HINT" >&2
    exit 2
fi
apply_replacements=""
if $fix; then
    apply_replacements="$(find_tool "clang-apply-replacements-$CLANG_TIDY_VERSION" clang-apply-replacements || true)"
    if [[ -z "$apply_replacements" ]]; then
        echo "clang-apply-replacements not found next to $clang_tidy or on PATH; it is in" \
            "clang-tools-$CLANG_TIDY_VERSION. $INSTALL_HINT" >&2
        exit 2
    fi
fi

root="$(git rev-parse --show-toplevel)"
cd "$root"
if [[ ! -f .clang-tidy ]]; then
    echo "No .clang-tidy in $root." >&2
    exit 2
fi
[[ ${#build_dirs[@]} -gt 0 ]] || build_dirs=("$root/build/tidy")

if $build; then
    if [[ ! -d app ]]; then
        echo "No ./app to build; pass the build directory of a native_sim test with -p." >&2
        exit 2
    fi
    west build -p auto -b native_sim ./app -d "${build_dirs[0]}" -o="-j$jobs" >&2
fi

# One compile database out of every -p directory, entries deduplicated by file (the first wins).
# The entries keep their own "directory", so they resolve as they did in their builds.
merged_dir="$(mktemp -d)"
trap 'rm -rf "$merged_dir"' EXIT
db_count="$(python3 -I - "$merged_dir" "${build_dirs[@]}" <<'PY'
import json, os, sys

merged, *directories = sys.argv[1:]
databases = []
for directory in directories:
    found = []
    if os.path.isfile(os.path.join(directory, "compile_commands.json")):
        found.append(os.path.join(directory, "compile_commands.json"))
    else:
        for current, subdirs, files in os.walk(directory):
            subdirs.sort()
            if "compile_commands.json" in files:
                found.append(os.path.join(current, "compile_commands.json"))
                subdirs.clear()  # A build directory has no builds inside.
    if not found:
        print(f"No compile_commands.json in or under {directory}; build one (--build) or pass -p DIR.",
              file=sys.stderr)
        sys.exit(2)
    databases += [f for f in found if f not in databases]

entries, seen = [], set()
for database in databases:
    for entry in json.load(open(database)):
        path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
        if path not in seen:
            seen.add(path)
            entries.append(entry)
with open(os.path.join(merged, "compile_commands.json"), "w") as out:
    json.dump(entries, out)
print(len(databases))
PY
)" || exit 2

# The sources to lint: in the compile database, tracked here (so not in a submodule or the build),
# not excluded, and within the given paths or changes.
tracked="$(git ls-files)"
changed=""
if [[ -n "$changed_base" ]]; then
    if ! base="$(git merge-base "$changed_base" HEAD 2> /dev/null)"; then
        echo "No merge base with $changed_base." >&2
        exit 2
    fi
    changed="$(git diff --name-only --diff-filter=ACMR "$base")"
fi
only_changed="$([[ -n "$changed_base" ]] && echo 1 || echo 0)"
sources="$(python3 -I - "$merged_dir/compile_commands.json" "$root" "$EXCLUDED" "$only_changed" "${paths[@]}" \
    3<<< "$tracked" 4<<< "$changed" <<'PY'
import json, os, re, sys

database, root, excluded, only_changed, *paths = sys.argv[1:]
tracked = set(open(3).read().split("\n"))
changed = set(filter(None, open(4).read().split("\n")))
excluded = re.compile(excluded)
seen = set()
for entry in json.load(open(database)):
    path = os.path.realpath(os.path.join(entry["directory"], entry["file"]))
    relative = os.path.relpath(path, root)
    if path in seen or relative not in tracked or excluded.search("/" + relative):
        continue
    if only_changed == "1" and relative not in changed:
        continue
    if paths and not any(path == p or path.startswith(p.rstrip("/") + "/") for p in paths):
        continue
    seen.add(path)
    print(path)
PY
)"

if [[ -z "$sources" ]]; then
    echo "No sources to lint." >&2
    exit 0
fi
count="$(printf '%s\n' "$sources" | wc -l)"
echo "Linting $count sources from $db_count compile database(s) with $(basename "$clang_tidy") $version..." >&2

# run-clang-tidy takes regular expressions; anchor each path so it selects that file only.
patterns=()
while IFS= read -r source; do
    patterns+=("^$(printf '%s' "$source" | sed 's/[][\.*^$+?(){}|]/\\&/g')\$")
done <<< "$sources"

args=(-p "$merged_dir" -j "$jobs" -quiet -hide-progress -clang-tidy-binary "$clang_tidy")

# Headers of a submodule are its own repository's business: add them to the configured exclusions.
# Header paths can come unnormalised (app/../modules/...), so the pattern matches on the path tail.
submodules="$(git config --file .gitmodules --get-regexp '\.path$' 2> /dev/null | awk '{print $2}' | paste -sd '|' || true)"
if [[ -n "$submodules" ]]; then
    configured="$("$clang_tidy" --dump-config "$root/check.cpp" 2> /dev/null |
        sed -n "s/^ExcludeHeaderFilterRegex: *'\(.*\)'$/\1/p")"
    args+=(-exclude-header-filter "${configured:+$configured|}.*/($submodules)/.*")
fi
if $fix; then
    args+=(-fix -clang-apply-replacements-binary "$apply_replacements")
fi
output="$("$run_clang_tidy" "${args[@]}" "${patterns[@]}" 2>&1 || true)"

warnings="$(printf '%s\n' "$output" | grep -E '^/.+:[0-9]+:[0-9]+: (warning|error): ' | sort -u || true)"
if [[ -n "$warnings" ]]; then
    printf '%s\n' "$output" | grep -vE '^(clang-tidy|Running clang-tidy|Applying fixes| *[0-9]+ warnings? (and|generated)|Suppressed|Use -header-filter)' >&2 || true
    total="$(printf '%s\n' "$warnings" | wc -l)"
    echo "$total warnings in $(printf '%s\n' "$warnings" | cut -d: -f1 | sort -u | wc -l) files." >&2
    $fix && echo "Fixes applied where clang-tidy offers one; review them with git diff." >&2
    exit 1
fi
echo "No warnings." >&2
