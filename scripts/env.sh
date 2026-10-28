# Source this file: pinned ESP-IDF environment for all builds.
# Override IDF_PATH to use another checkout of the same version.
IDF_VERSION_PINNED="v5.5.5"
: "${IDF_PATH:=$HOME/esp/esp-idf}"
export IDF_PATH

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export ROOT_DIR

idf_ver="$(git -C "$IDF_PATH" describe --tags 2>/dev/null || echo unknown)"
if [ "$idf_ver" != "$IDF_VERSION_PINNED" ]; then
    echo "error: ESP-IDF at $IDF_PATH is $idf_ver, expected $IDF_VERSION_PINNED" >&2
    return 1 2>/dev/null || exit 1
fi
. "$IDF_PATH/export.sh" >/dev/null || { echo "error: IDF export failed" >&2; return 1 2>/dev/null || exit 1; }
