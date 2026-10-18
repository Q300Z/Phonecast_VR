#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

DEFAULT_PRESET="linux-arm64"
case "$(uname -m 2>/dev/null || echo '')" in
  x86_64|amd64) DEFAULT_PRESET="linux-x64" ;;
  aarch64|arm64) DEFAULT_PRESET="linux-arm64" ;;
esac

PRESET="$DEFAULT_PRESET"
BUILD_DIR=""
RUN_TESTS=1
INSTALL_ON_HEADSET=0
JOBS=""

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required command not found: $1" >&2
    exit 1
  fi
}

usage() {
  cat <<USAGE
Usage: verify-and-build-release.sh [options]

Workflow script tailored for compiling, testing, and deploying PhoneCast VR.

Options:
  -p, --preset NAME            CMake preset to use (default: $DEFAULT_PRESET)
  -l, --list-presets           List available CMake presets and exit
  -b, --build-dir PATH         Build directory for release and test checks
  -j, --jobs N                 Number of concurrent jobs for tests (default: auto)
  -i, --install                Automatically run install-phonecast.sh after build
      --skip-tests             Skip ctest execution
  -h, --help                   Show this help
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -l|--list-presets)
      require_command cmake
      cd "$REPO_ROOT"
      cmake --list-presets
      exit 0
      ;;
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
    -j|--jobs)
      [ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      JOBS="$2"
      shift 2
      ;;
    -i|--install)
      INSTALL_ON_HEADSET=1
      shift
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

if [ -z "$JOBS" ]; then
  JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
fi


require_command cmake
require_command ninja
if [ "$RUN_TESTS" -eq 1 ]; then

  require_command ctest
fi

cd "$REPO_ROOT"

printf 'Configuring preset %s on headset (%s)\n' "$PRESET" "$(uname -m 2>/dev/null || echo 'unknown')"
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
  printf 'Running automated test suites from %s (parallel jobs: %s)\n' "$BUILD_DIR" "$JOBS"
  ctest --test-dir "$BUILD_DIR" -j "$JOBS" --output-on-failure
fi

if [ "$INSTALL_ON_HEADSET" -eq 1 ]; then
  INSTALLER="$BUILD_DIR/bin/steam-frame-installer/install-phonecast.sh"
  if [ -f "$INSTALLER" ]; then
    printf 'Installing PhoneCast VR directly into SteamVR Dashboard...\n'
    sh "$INSTALLER"
  else
    echo "Installer script not found at expected path: $INSTALLER" >&2
    exit 1
  fi
fi

echo "Done. PhoneCast VR build successfully verified."
