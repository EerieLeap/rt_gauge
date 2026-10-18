#!/usr/bin/env bash
# Checks or fixes the C/C++ formatting of the repository it runs in, with the repository's
# .clang-format. Files listed in a .clang-format-ignore are skipped by clang-format itself.
#
#   scripts/format.sh                     check every tracked C/C++ file
#   scripts/format.sh --fix               reformat them in place
#   scripts/format.sh --changed [BASE]    only files changed since the merge base with BASE
#                                         (default origin/main), working tree included
#   scripts/format.sh --recursive         also every submodule that carries its own .clang-format
#   scripts/format.sh [PATH...]           only these files or directories of this repository
#   scripts/format.sh -j N                run N clang-format processes at once (default 4)
#
# Exit status: 0 when clean (or fixed), 1 when files need formatting, 2 on a usage or setup error.
set -euo pipefail

readonly CLANG_FORMAT_VERSION="23.1.1" # The version CI pins; majors differ in how they wrap.
readonly EXTENSIONS=('*.c' '*.cc' '*.cpp' '*.h' '*.hpp')

usage() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit "${1:-0}"
}

mode="check"
changed_base=""
recursive=false
jobs=4
paths=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check) mode="check" ;;
        --fix) mode="fix" ;;
        --changed)
            changed_base="origin/main"
            # The next argument is the base only when it names a commit; otherwise it is a path.
            if [[ $# -gt 1 && "$2" != -* ]] && git rev-parse --verify --quiet "$2^{commit}" > /dev/null; then
                changed_base="$2"
                shift
            fi
            ;;
        --recursive) recursive=true ;;
        -j) jobs="${2:-}"; shift ;;
        -j*) jobs="${1#-j}" ;;
        -h|--help) usage 0 ;;
        -*) echo "Unknown option: $1" >&2; usage 2 ;;
        *) paths+=("$1") ;;
    esac
    shift
done

if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "-j needs a positive number." >&2
    exit 2
fi
if [[ ${#paths[@]} -gt 0 && ( -n "$changed_base" || $recursive == true ) ]]; then
    echo "Paths cannot be combined with --changed or --recursive." >&2
    exit 2
fi

clang_format="${CLANG_FORMAT:-clang-format}"
if ! command -v "$clang_format" > /dev/null; then
    echo "clang-format not found. Install it with: pip install clang-format==$CLANG_FORMAT_VERSION" >&2
    exit 2
fi
version="$("$clang_format" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if [[ "${version%%.*}" != "${CLANG_FORMAT_VERSION%%.*}" ]]; then
    echo "clang-format $version found, $CLANG_FORMAT_VERSION expected:" \
        "pip install clang-format==$CLANG_FORMAT_VERSION" >&2
    exit 2
elif [[ "$version" != "$CLANG_FORMAT_VERSION" ]]; then
    echo "Note: clang-format $version, CI uses $CLANG_FORMAT_VERSION." >&2
fi

root="$(git rev-parse --show-toplevel)"

# Paths are given relative to where the script is called; the listing runs from the repository root.
for i in "${!paths[@]}"; do
    paths[i]="$(realpath -m --relative-to="$root" "${paths[i]}")"
done

failed=0

# Prints the C/C++ files to check in the repository in the current directory, NUL-separated and
# relative to its root: the given paths, the changed files or every tracked one.
list_files() {
    if [[ ${#paths[@]} -gt 0 ]]; then
        local path
        for path in "${paths[@]}"; do
            # A file named explicitly is taken even when untracked; a directory means its tracked files.
            # In a pathspec "*" also matches "/", so "dir/*.cpp" covers the whole tree below dir.
            if [[ -f "$path" ]]; then
                printf '%s\0' "$path"
            else
                git ls-files -z -- "${EXTENSIONS[@]/#/${path%/}/}"
            fi
        done
    elif [[ -n "$changed_base" ]]; then
        local base
        if ! base="$(git merge-base "$changed_base" HEAD 2> /dev/null)"; then
            echo "$(basename "$PWD"): no merge base with $changed_base." >&2
            exit 2
        fi
        git diff -z --name-only --diff-filter=ACMR "$base" -- "${EXTENSIONS[@]}"
    else
        git ls-files -z -- "${EXTENSIONS[@]}"
    fi
}

# Checks or fixes the repository in the current directory; $1 is its path from the top repository.
run_repository() {
    local prefix="$1"

    if [[ ! -f .clang-format ]]; then
        echo "No .clang-format in ${prefix:-$root}." >&2
        exit 2
    fi
    # A configuration clang-format cannot read would otherwise pass every file as clean.
    if ! "$clang_format" --dump-config --assume-filename=check.cpp > /dev/null; then
        exit 2
    fi

    local files
    files="$(list_files | tr '\0' '\n')"
    if [[ -z "$files" ]]; then
        echo "${prefix:-.}: no C/C++ files to check." >&2
        return
    fi
    local count
    count="$(printf '%s\n' "$files" | wc -l)"

    if [[ "$mode" == "fix" ]]; then
        printf '%s\n' "$files" | tr '\n' '\0' | xargs -0 -P "$jobs" -n 16 "$clang_format" -i
        echo "${prefix:-.}: formatted $count files." >&2
        return
    fi

    # One process per file that prints only its name: parallel processes sharing one pipe would
    # interleave their diagnostics, and a line this short is written in one piece.
    local unformatted
    unformatted="$(printf '%s\n' "$files" | tr '\n' '\0' |
        xargs -0 -P "$jobs" -n 1 sh -c '"$0" --dry-run -Werror "$1" > /dev/null 2>&1 || printf "%s\n" "$1"' \
            "$clang_format" | sort || true)"
    if [[ -n "$unformatted" ]]; then
        printf '%s\n' "$unformatted" | sed "s|^|${prefix:+$prefix/}|"
        failed=$((failed + $(printf '%s\n' "$unformatted" | wc -l)))
    fi
    echo "${prefix:-.}: checked $count files." >&2
}

cd "$root"
run_repository ""

if $recursive; then
    # A submodule takes part only when it carries its own .clang-format, so upstream code is left alone.
    submodules="$(git submodule foreach --quiet --recursive 'echo "$displaypath"')"
    while IFS= read -r submodule; do
        if [[ -n "$submodule" && -f "$root/$submodule/.clang-format" ]]; then
            cd "$root/$submodule"
            run_repository "$submodule"
        fi
    done <<< "$submodules"
    cd "$root"
fi

if [[ "$mode" == "check" && $failed -gt 0 ]]; then
    echo "$failed files need formatting; run $0 --fix${changed_base:+ --changed $changed_base}" \
        "to fix them." >&2
    exit 1
fi
