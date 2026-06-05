#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$REPO_ROOT/build"
MAP_BINARY="$BUILD_DIR/benchmarks/toggle_map_benchmark"
VECTOR_BINARY="$BUILD_DIR/benchmarks/vector_microbenchmark"
ROMULUS_BINARY="$BUILD_DIR/benchmarks/romulus_microbenchmark"
TRINITY_FC_BINARY="$BUILD_DIR/benchmarks/trinity_fc_microbenchmark"
TRINITY_TL2_BINARY="$BUILD_DIR/benchmarks/trinity_tl2_microbenchmark"
ROMULUS_NVM_BINARY="$BUILD_DIR/benchmarks/romulus_microbenchmark_nvm"
TRINITY_FC_NVM_BINARY="$BUILD_DIR/benchmarks/trinity_fc_microbenchmark_nvm"
TRINITY_TL2_NVM_BINARY="$BUILD_DIR/benchmarks/trinity_tl2_microbenchmark_nvm"
NUM=10000
KEYS=10000
THREADS_SPEC="1-$(nproc)"
SHARDS=""
RUNS=3
OUTPUT="$BUILD_DIR/toggle_map_benchmarks.csv"
NVRAM_DIR=""
PMDK_VOLATILE=0

BASE_BENCHMARKS=(
    "std-map"
    "std-unordered-map"
    "fossil-prbtree"
    "fossil-pmap"
    "fossil-sharded-pmap"
    "leveldb"
    "bdb"
    "std-vector"
    "fossil-pvec"
    "romulus-rbtree"
    "romulus-hashmap"
    "romulus-vector"
    "trinity-fc-rbtree"
    "trinity-fc-hashmap"
    "trinity-fc-vector"
    "trinity-tl2-rbtree"
    "trinity-tl2-hashmap"
    "trinity-tl2-vector"
)
PMDK_BENCHMARKS=(
    "pmdk-hashmap"
    "pmdk-rbtree"
    "pmdk-vector"
)
BENCHMARKS=("${BASE_BENCHMARKS[@]}")
BENCHMARKS_SPEC=""
PMDK_BENCHMARKS_ENABLED=0

usage() {
    cat <<'EOF'
Usage: run_toggle_map_benchmarks.sh [options]

Options:
  --num N             Total operations per benchmark run. Default: 10000
  --threads SPEC      Thread sweep. Supports values, comma lists, and ranges.
                      Examples: 1, 1,2,4,8, 1-16, 1-16:2
                      Bare ranges double each step. Default: 1-$(nproc)
  --shards N          Fixed shard count for fossil-sharded-pmap. Default: same as threads
  --runs N            Repetitions per benchmark/thread pair. Default: 3
  --keys N            Key space [0, N-1]
  --nvram-dir PATH    Directory on the NVRAM/PMEM mount used for persistent benchmark files
  --volatile-pmdk     Run PMDK benchmarks in volatile mmap mode with flushes disabled
  --benchmarks LIST   Comma-separated benchmark ids to run. Default: all available
  --output PATH       Output CSV path. Default: build/toggle_map_benchmarks.csv
  --build-dir DIR     CMake build directory. Default: ./build
  --map-binary PATH   toggle_map_benchmark binary path
  --vector-binary PATH
                     vector_microbenchmark binary path
  --romulus-binary PATH
  --trinity-fc-binary PATH
  --trinity-tl2-binary PATH
  --help              Show this help text
EOF
}

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

trim_spaces() {
    local value=$1
    value=${value//[[:space:]]/}
    printf '%s' "$value"
}

parse_range_spec() {
    local spec=$1
    local out_name=$2
    local -n out_ref="$out_name"
    local -a tokens=()
    local token start end step value next_value
    local -A seen=()

    out_ref=()
    IFS=',' read -r -a tokens <<<"$spec"
    for token in "${tokens[@]}"; do
        token=$(trim_spaces "$token")
        [[ -z $token ]] && continue

        if [[ $token =~ ^([0-9]+)-([0-9]+)(:([0-9]+))?$ ]]; then
            start=${BASH_REMATCH[1]}
            end=${BASH_REMATCH[2]}
            (( end >= start )) || die "Descending ranges are not supported: $token"

            if [[ -n ${BASH_REMATCH[4]:-} ]]; then
                step=${BASH_REMATCH[4]}
                (( step >= 1 )) || die "Invalid step in range token: $token"

                for (( value = start; value <= end; value += step )); do
                    if [[ -z ${seen[$value]+x} ]]; then
                        out_ref+=("$value")
                        seen[$value]=1
                    fi
                done
            else
                value=$start
                while (( value <= end )); do
                    if [[ -z ${seen[$value]+x} ]]; then
                        out_ref+=("$value")
                        seen[$value]=1
                    fi

                    if (( value > end / 2 )); then
                        break
                    fi

                    next_value=$(( value * 2 ))
                    (( next_value > value )) || die "Range expansion overflowed for token: $token"
                    value=$next_value
                done
            fi
            continue
        fi

        [[ $token =~ ^[0-9]+$ ]] || die "Invalid range token: $token"
        value=$token
        (( value >= 1 )) || die "Range values must be positive integers: $token"
        if [[ -z ${seen[$value]+x} ]]; then
            out_ref+=("$value")
            seen[$value]=1
        fi
    done

    ((${#out_ref[@]} > 0)) || die "At least one thread value is required: $spec"
}

clean_persistence_files() {
    shopt -s nullglob
    local files=(
        "$REPO_ROOT"/fossil_persistence_*
        "/dev/shm"/fossil_romulus_microbenchmark_shared*
        "/dev/shm"/fossil_trinity_fc_microbenchmark_shared*
        "/dev/shm"/fossil_trinity_tl2_microbenchmark_shared*
        "/dev/shm"/trinityvrfc_shared_main
        "/dev/shm"/trinityvrtl2_shared_main
    )
    local nvram_files=()
    if [[ -n ${NVRAM_DIR:-} ]]; then
        nvram_files=(
            "$NVRAM_DIR"/fossil_persistence_*
            "$NVRAM_DIR"/fossil_romulus_microbenchmark_shared*
            "$NVRAM_DIR"/fossil_trinity_fc_microbenchmark_shared*
            "$NVRAM_DIR"/fossil_trinity_tl2_microbenchmark_shared*
        )
    fi
    if ((${#files[@]} > 0)); then
        rm -f -- "${files[@]}"
    fi
    if ((${#nvram_files[@]} > 0)); then
        rm -f -- "${nvram_files[@]}"
    fi
    shopt -u nullglob
}

is_vector_benchmark() {
    local benchmark=$1
    [[ $benchmark == "std-vector" || $benchmark == "fossil-pvec" || $benchmark == "pmdk-vector" || $benchmark == *"-vector" ]]
}

is_pmdk_benchmark() {
    local benchmark=$1
    [[ $benchmark == "pmdk-hashmap" || $benchmark == "pmdk-rbtree" || $benchmark == "pmdk-vector" ]]
}

is_fossil_persistent_benchmark() {
    local benchmark=$1
    [[ $benchmark == "fossil-prbtree" || $benchmark == "fossil-pmap" || $benchmark == "fossil-sharded-pmap" || $benchmark == "fossil-pvec" ]]
}

is_durabletx_benchmark() {
    local benchmark=$1
    [[ $benchmark == romulus-* || $benchmark == trinity-fc-* || $benchmark == trinity-tl2-* ]]
}

durabletx_backend_for() {
    local benchmark=$1

    case "$benchmark" in
    romulus-*) printf 'romulus' ;;
    trinity-fc-*) printf 'trinity-fc' ;;
    trinity-tl2-*) printf 'trinity-tl2' ;;
    *) return 1 ;;
    esac
}

durabletx_benchmark_kind_for() {
    local benchmark=$1

    case "$benchmark" in
    *-rbtree) printf 'rb-tree' ;;
    *-hashmap) printf 'hashmap' ;;
    *-vector) printf 'vector' ;;
    *) return 1 ;;
    esac
}

durabletx_binary_for() {
    local backend=$1

    if [[ -n $NVRAM_DIR ]]; then
        case "$backend" in
        romulus) printf '%s' "$ROMULUS_NVM_BINARY" ;;
        trinity-fc) printf '%s' "$TRINITY_FC_NVM_BINARY" ;;
        trinity-tl2) printf '%s' "$TRINITY_TL2_NVM_BINARY" ;;
        *) return 1 ;;
        esac
        return
    fi

    case "$backend" in
    romulus) printf '%s' "$ROMULUS_BINARY" ;;
    trinity-fc) printf '%s' "$TRINITY_FC_BINARY" ;;
    trinity-tl2) printf '%s' "$TRINITY_TL2_BINARY" ;;
    *) return 1 ;;
    esac
}

detect_pmdk_benchmarks() {
    local cache_file="$BUILD_DIR/CMakeCache.txt"

    PMDK_BENCHMARKS_ENABLED=0
    if [[ -f $cache_file ]] && rg -q '^FOSSIL_HAS_PMDK_BENCHMARKS:BOOL=ON$' "$cache_file"; then
        PMDK_BENCHMARKS_ENABLED=1
    fi
}

refresh_default_benchmarks() {
    BENCHMARKS=("${BASE_BENCHMARKS[@]}")
    detect_pmdk_benchmarks
    if (( PMDK_BENCHMARKS_ENABLED )); then
        BENCHMARKS+=("${PMDK_BENCHMARKS[@]}")
    fi
}

benchmark_supported() {
    local benchmark=$1

    case "$benchmark" in
    std-map|std-unordered-map|fossil-prbtree|fossil-pmap|fossil-sharded-pmap|leveldb|bdb|std-vector|fossil-pvec|romulus-rbtree|romulus-hashmap|romulus-vector|trinity-fc-rbtree|trinity-fc-hashmap|trinity-fc-vector|trinity-tl2-rbtree|trinity-tl2-hashmap|trinity-tl2-vector)
        return 0
        ;;
    pmdk-hashmap|pmdk-rbtree|pmdk-vector)
        (( PMDK_BENCHMARKS_ENABLED ))
        return
        ;;
    *)
        return 1
        ;;
    esac
}

parse_benchmark_list() {
    local spec=$1
    local out_name=$2
    local -n out_ref="$out_name"
    local -a tokens=()
    local token benchmark
    local -A seen=()

    out_ref=()
    IFS=',' read -r -a tokens <<<"$spec"
    for token in "${tokens[@]}"; do
        benchmark=$(trim_spaces "$token")
        [[ -z $benchmark ]] && continue

        if ! benchmark_supported "$benchmark"; then
            die "Unknown benchmark in --benchmarks: $benchmark"
        fi

        if [[ -z ${seen[$benchmark]+x} ]]; then
            out_ref+=("$benchmark")
            seen[$benchmark]=1
        fi
    done

    ((${#out_ref[@]} > 0)) || die "--benchmarks must select at least one benchmark"
}

actual_shards_for() {
    local benchmark=$1
    local threads=$2

    if [[ $benchmark != "fossil-sharded-pmap" ]]; then
        printf '1'
        return
    fi

    if [[ -n $SHARDS ]]; then
        printf '%s' "$SHARDS"
        return
    fi

    if (( threads < NUM )); then
        printf '%s' "$threads"
    else
        printf '%s' "$NUM"
    fi
}

parse_raw_output() {
    local line=$1

    if [[ ! $line =~ ^average=([-+0-9.eE]+)[[:space:]]+median=([-+0-9.eE]+)[[:space:]]+min=([-+0-9.eE]+)[[:space:]]+max=([-+0-9.eE]+)[[:space:]]+stddev=([-+0-9.eE]+)$ ]]; then
        die "Could not parse benchmark output: $line"
    fi

    RAW_AVERAGE=${BASH_REMATCH[1]}
    RAW_MEDIAN=${BASH_REMATCH[2]}
    RAW_MIN=${BASH_REMATCH[3]}
    RAW_MAX=${BASH_REMATCH[4]}
    RAW_STDDEV=${BASH_REMATCH[5]}
}

run_benchmark_binary_once() {
    local stdout=$1
    local stderr=$2
    shift 2

    clean_persistence_files
    if ! (
        cd "$REPO_ROOT"
        exec "$@" >"$stdout" 2>"$stderr"
    ); then
        printf 'Benchmark command failed.\nCommand:' >&2
        printf ' %q' "$@" >&2
        printf '\nstdout:\n' >&2
        cat "$stdout" >&2 || true
        printf '\nstderr:\n' >&2
        cat "$stderr" >&2 || true
        rm -f "$stdout" "$stderr"
        exit 1
    fi
    clean_persistence_files
}

run_one() {
    local benchmark=$1
    local threads=$2
    local run_index=$3
    local actual_shards stdout stderr raw_line binary durabletx_backend durabletx_kind
    local -a cmd=(
        "--benchmark=$benchmark"
        "--num=$NUM"
        "--threads=$threads"
        "--raw"
        "--keys=$KEYS"
    )

    if is_durabletx_benchmark "$benchmark"; then
        durabletx_backend=$(durabletx_backend_for "$benchmark")
        durabletx_kind=$(durabletx_benchmark_kind_for "$benchmark")
        binary=$(durabletx_binary_for "$durabletx_backend")
        cmd=(
            "$binary"
            "--benchmark=$durabletx_kind"
            "--num=$NUM"
            "--threads=$threads"
            "--raw"
            "--keys=$KEYS"
        )
    elif is_vector_benchmark "$benchmark"; then
        binary=$VECTOR_BINARY
        cmd=("$binary" "${cmd[@]}")
    else
        binary=$MAP_BINARY
        cmd=("$binary" "${cmd[@]}")
    fi

    if [[ $benchmark == "fossil-sharded-pmap" && -n $SHARDS ]]; then
        cmd+=("--shards=$SHARDS")
    fi
    if (is_pmdk_benchmark "$benchmark" || is_fossil_persistent_benchmark "$benchmark") && [[ -n $NVRAM_DIR ]]; then
        cmd+=("--nvram-dir=$NVRAM_DIR")
    fi
    if is_durabletx_benchmark "$benchmark" && [[ -n $NVRAM_DIR ]]; then
        cmd+=("--nvram-dir=$NVRAM_DIR")
    fi
    if is_pmdk_benchmark "$benchmark" && (( PMDK_VOLATILE )); then
        cmd+=("--volatile-pmdk")
    fi

    actual_shards=$(actual_shards_for "$benchmark" "$threads")
    stdout=$(mktemp)
    stderr=$(mktemp)

    run_benchmark_binary_once "$stdout" "$stderr" "${cmd[@]}"

    raw_line=$(grep -E '^average=[-+0-9.eE]+[[:space:]]+median=' "$stdout" | tail -n 1 || true)
    [[ -n $raw_line ]] || die "Could not find raw metrics in benchmark output: $(cat "$stdout")"
    parse_raw_output "$raw_line"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$benchmark" \
        "$threads" \
        "$actual_shards" \
        "$run_index" \
        "$NUM" \
        "$RAW_AVERAGE" \
        "$RAW_MEDIAN" \
        "$RAW_MIN" \
        "$RAW_MAX" \
        "$RAW_STDDEV" >>"$OUTPUT"

    printf 'benchmark=%s threads=%s run=%s average=%s\n' \
        "$benchmark" "$threads" "$run_index" "$RAW_AVERAGE"

    rm -f "$stdout" "$stderr"
}

while (($# > 0)); do
    case "$1" in
    --num=*)
        NUM=${1#--num=}
        ;;
    --keys=*)
        KEYS=${1#--keys=}
        ;;
    --threads=*)
        THREADS_SPEC=${1#--threads=}
        ;;
    --shards=*)
        SHARDS=${1#--shards=}
        ;;
    --runs=*)
        RUNS=${1#--runs=}
        ;;
    --nvram-dir=*)
        NVRAM_DIR=${1#--nvram-dir=}
        ;;
    --nvram-dir)
        shift
        [[ $# -gt 0 ]] || die "--nvram-dir requires a directory argument"
        NVRAM_DIR=$1
        ;;
    --volatile-pmdk)
        PMDK_VOLATILE=1
        ;;
    --output=*)
        OUTPUT=${1#--output=}
        ;;
    --build-dir=*)
        BUILD_DIR=${1#--build-dir=}
        MAP_BINARY="$BUILD_DIR/benchmarks/toggle_map_benchmark"
        VECTOR_BINARY="$BUILD_DIR/benchmarks/vector_microbenchmark"
        ROMULUS_BINARY="$BUILD_DIR/benchmarks/romulus_microbenchmark"
        TRINITY_FC_BINARY="$BUILD_DIR/benchmarks/trinity_fc_microbenchmark"
        TRINITY_TL2_BINARY="$BUILD_DIR/benchmarks/trinity_tl2_microbenchmark"
        ROMULUS_NVM_BINARY="$BUILD_DIR/benchmarks/romulus_microbenchmark_nvm"
        TRINITY_FC_NVM_BINARY="$BUILD_DIR/benchmarks/trinity_fc_microbenchmark_nvm"
        TRINITY_TL2_NVM_BINARY="$BUILD_DIR/benchmarks/trinity_tl2_microbenchmark_nvm"
        ;;
    --map-binary=*)
        MAP_BINARY=${1#--map-binary=}
        ;;
    --vector-binary=*)
        VECTOR_BINARY=${1#--vector-binary=}
        ;;
    --romulus-binary=*)
        ROMULUS_BINARY=${1#--romulus-binary=}
        ;;
    --trinity-fc-binary=*)
        TRINITY_FC_BINARY=${1#--trinity-fc-binary=}
        ;;
    --trinity-tl2-binary=*)
        TRINITY_TL2_BINARY=${1#--trinity-tl2-binary=}
        ;;
    --benchmarks=*)
        BENCHMARKS_SPEC=${1#--benchmarks=}
        ;;
    --help)
        usage
        exit 0
        ;;
    *)
        die "Unknown option: $1"
        ;;
    esac
    shift
done

[[ $NUM =~ ^[0-9]+$ ]] || die "--num must be a positive integer"
[[ $RUNS =~ ^[0-9]+$ ]] || die "--runs must be a positive integer"
[[ $KEYS =~ ^[0-9]+$ ]] || die "--keys must be a positive integer"
(( NUM > 0 )) || die "--num must be greater than 0"
(( RUNS > 0 )) || die "--runs must be greater than 0"

if [[ -n $SHARDS ]]; then
    [[ $SHARDS =~ ^[0-9]+$ ]] || die "--shards must be a positive integer"
    (( SHARDS > 0 )) || die "--shards must be greater than 0"
fi

declare -a THREAD_VALUES=()
parse_range_spec "$THREADS_SPEC" THREAD_VALUES

mkdir -p "$BUILD_DIR"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR"

refresh_default_benchmarks
if [[ -n $BENCHMARKS_SPEC ]]; then
    parse_benchmark_list "$BENCHMARKS_SPEC" BENCHMARKS
fi

mkdir -p "$(dirname -- "$OUTPUT")"

cmake --build "$BUILD_DIR" --target \
    toggle_map_benchmark \
    vector_microbenchmark \
    romulus_microbenchmark \
    trinity_fc_microbenchmark \
    trinity_tl2_microbenchmark \
    romulus_microbenchmark_nvm \
    trinity_fc_microbenchmark_nvm \
    trinity_tl2_microbenchmark_nvm

printf 'benchmark,threads,shards,run,num,average_ns,median_ns,min_ns,max_ns,stddev_ns\n' >"$OUTPUT"

for benchmark in "${BENCHMARKS[@]}"; do
    for threads in "${THREAD_VALUES[@]}"; do
        for (( run = 1; run <= RUNS; ++run )); do
            run_one "$benchmark" "$threads" "$run"
        done
    done
done
