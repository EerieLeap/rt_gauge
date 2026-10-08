#!/usr/bin/env bash
# Update the west workspace (Zephyr and its modules) to the revisions in west.yml.
#
# The build applies patches to checkouts outside this repo (see app/CMakeLists.txt),
# which leaves Zephyr, hal_espressif and LVGL dirty. This script reverts those patches,
# runs `west update`, fetches the Espressif blobs, then checks and re-applies every patch
# against the new checkouts and reports the ones that no longer apply.
#
# Usage: tools/update_zephyr.sh [--check] [--no-blobs]
#   --check     only check the patches against the current checkouts, do not update
#   --no-blobs  skip `west blobs fetch hal_espressif`

set -euo pipefail

CHECK_ONLY=0
FETCH_BLOBS=1
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --no-blobs) FETCH_BLOBS=0 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

project_path() {
    west list -f '{abspath}' "$1"
}

# "patch directory (relative to the repo root)|west project name"
PATCH_SETS=(
    "modules/eerie_leap_rt_core/patches/zephyr|zephyr"
    "modules/eerie_leap_rt_core/patches/modules/hal_espressif|hal_espressif"
    "app/patches/modules/lvgl|lvgl"
)

patches_in() {
    find "$REPO_ROOT/$1" -name '*.patch' | sort
}

is_applied() {
    git -C "$2" apply --reverse --check "$1" >/dev/null 2>&1
}

revert_patches() {
    local entry dir project target patch
    for entry in "${PATCH_SETS[@]}"; do
        dir="${entry%%|*}"
        project="${entry##*|}"
        target="$(project_path "$project")"
        while read -r patch; do
            if is_applied "$patch" "$target"; then
                echo "  revert  $project: $(basename "$patch")"
                git -C "$target" apply --reverse "$patch"
            fi
        done < <(patches_in "$dir")

        if [[ -n "$(git -C "$target" status --porcelain --untracked-files=no)" ]]; then
            echo "error: $target has local changes that are not from $dir:" >&2
            git -C "$target" status --short --untracked-files=no >&2
            echo "Commit, stash or discard them, then run this script again." >&2
            exit 1
        fi
    done
}

# Applies every patch that applies cleanly and returns the number that do not.
apply_patches() {
    local failed=0 entry dir project target patch
    for entry in "${PATCH_SETS[@]}"; do
        dir="${entry%%|*}"
        project="${entry##*|}"
        target="$(project_path "$project")"
        while read -r patch; do
            if is_applied "$patch" "$target"; then
                echo "  ok      $project: $(basename "$patch") (already applied)"
            elif git -C "$target" apply --check "$patch" 2>/dev/null; then
                if (( CHECK_ONLY )); then
                    echo "  ok      $project: $(basename "$patch") (applies cleanly)"
                else
                    git -C "$target" apply "$patch"
                    echo "  apply   $project: $(basename "$patch")"
                fi
            else
                echo "  FAILED  $project: ${patch#"$REPO_ROOT"/}"
                git -C "$target" apply --check "$patch" 2>&1 | sed 's/^/          /' || true
                failed=$((failed + 1))
            fi
        done < <(patches_in "$dir")
    done
    return "$failed"
}

revisions() {
    west list -f '{name} {sha}' 2>/dev/null | grep -v '^manifest '
}

if (( CHECK_ONLY )); then
    echo "Checking patches against the current checkouts"
    if apply_patches; then
        echo "All patches apply."
    else
        exit 1
    fi
    exit 0
fi

ZEPHYR_DIR="$(project_path zephyr)"
OLD_ZEPHYR="$(git -C "$ZEPHYR_DIR" rev-parse HEAD)"
OLD_REVISIONS="$(revisions)"

echo "Reverting applied patches"
revert_patches

echo "Updating the west workspace"
west update

if (( FETCH_BLOBS )); then
    echo "Fetching Espressif blobs"
    west blobs fetch hal_espressif
fi

echo "Re-applying patches"
set +e
apply_patches
FAILED=$?
set -e

echo
echo "Changed projects:"
diff <(echo "$OLD_REVISIONS") <(revisions) | sed -n 's/^> /  /p' || true
echo
echo "Zephyr: $(git -C "$ZEPHYR_DIR" log -1 --format='%h %cd %s' --date=short)"
echo "  previous: $OLD_ZEPHYR"
echo "  log:      git -C $ZEPHYR_DIR log --oneline $OLD_ZEPHYR..HEAD"

SDK_REQUIRED="$(cat "$ZEPHYR_DIR/SDK_VERSION")"
echo "Zephyr SDK required: $SDK_REQUIRED"
SDK_DIRS=("${ZEPHYR_SDK_INSTALL_DIR:-}" "$HOME"/zephyr-sdk-* /opt/zephyr-sdk-*)
if ! grep -qxF "$SDK_REQUIRED" "${SDK_DIRS[@]/%//sdk_version}" 2>/dev/null; then
    echo "warning: Zephyr SDK $SDK_REQUIRED is not installed in \$ZEPHYR_SDK_INSTALL_DIR, ~ or /opt"
fi

if (( FAILED )); then
    echo
    echo "$FAILED patch(es) no longer apply; rebase them on the new checkouts before building." >&2
    exit 1
fi
