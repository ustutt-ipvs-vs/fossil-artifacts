#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage:
  scripts/run_crash_recovery_pmem.sh --nvram-dir /path/to/nvram-dir [--runs N]
  FOSSIL_NVRAM_DIR=/path/to/nvram-dir FOSSIL_CRASH_RUNS=N scripts/run_crash_recovery_pmem.sh

The directory must be on a DAX-capable persistent-memory filesystem because
the test maps Fossil files with MAP_SYNC. It does not run a volatile fallback.
EOF
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${FOSSIL_BUILD_DIR:-"$REPO_ROOT/build"}
NVRAM_DIR=${FOSSIL_NVRAM_DIR:-}
RUNS=${FOSSIL_CRASH_RUNS:-24}

while (($#)); do
    case "$1" in
        --nvram-dir)
            if (($# < 2)); then
                usage
                exit 2
            fi
            NVRAM_DIR=$2
            shift 2
            ;;
        --runs)
            if (($# < 2)); then
                usage
                exit 2
            fi
            RUNS=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$NVRAM_DIR" ]]; then
    usage
    exit 2
fi

if [[ ! "$RUNS" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: --runs must be a positive integer" >&2
    exit 2
fi

mkdir -p "$NVRAM_DIR"

cmake -S "$REPO_ROOT" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" --target crash_recovery_test

exec "$BUILD_DIR/tests/crash_recovery_test" --nvram-dir "$NVRAM_DIR" --runs "$RUNS"
