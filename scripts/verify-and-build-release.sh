#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

PRESET="linux-arm64"
BUILD_DIR=""
RUN_TESTS=1

usage() {
  cat <<'USAGE'
Usage: verify-and-build-release.sh [options]

Options:
  -p, --preset NAME            CMake preset to use (default: linux-arm64)
  -b, --build-dir PATH         Build directory for release and test checks
      --skip-tests             Skip ctest execution
  -h, --help                   Show this help
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -p|--preset)
      [ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      PRESET="$2"
      shift 2
      ;;
    -b|--build-dir)
      [ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      BUILD_DIR="$2"
      shift 2
      ;;
    --skip-tests)
      RUN_TESTS=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ -z "$BUILD_DIR" ]; then
  BUILD_DIR="$REPO_ROOT/out/build/$PRESET"
fi

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required command not found: $1" >&2
    exit 1
  fi
}

require_command cmake
if [ "$RUN_TESTS" -eq 1 ]; then
  require_command ctest
fi

cd "$REPO_ROOT"

printf 'Configuring preset %s\n' "$PRESET"
cmake --preset "$PRESET"

CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
if [ ! -f "$CACHE_FILE" ]; then
  echo "Missing CMake cache after configure: $CACHE_FILE" >&2
  exit 1
fi

if ! grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$CACHE_FILE"; then
  echo "Preset $PRESET did not configure a Release build." >&2
  exit 1
fi

printf 'Building preset %s\n' "$PRESET"
cmake --build --preset "$PRESET"

if [ "$RUN_TESTS" -eq 1 ]; then
  printf 'Running tests from %s\n' "$BUILD_DIR"
  ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo "Done."
