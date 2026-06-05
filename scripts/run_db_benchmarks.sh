#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$REPO_ROOT/build"
LEVELDB_BINARY="$BUILD_DIR/benchmarks/leveldb_db_benchmark"
BDB_BINARY="$BUILD_DIR/benchmarks/bdb_benchmark"
FOSSILDB_BINARY="$BUILD_DIR/benchmarks/fossildb_benchmark"
UNORDERED_MAP_RWLOCK_BINARY="$BUILD_DIR/benchmarks/unordered_map_rwlock_benchmark"
TRINITYDB_FC_BINARY="$BUILD_DIR/benchmarks/trinitydb_fc_benchmark"
TRINITYDB_TL2_BINARY="$BUILD_DIR/benchmarks/trinitydb_tl2_benchmark"
ROMULUSDB_BINARY="$BUILD_DIR/benchmarks/romulusdb_benchmark"
TRINITYDB_FC_NVM_BINARY="$BUILD_DIR/benchmarks/trinitydb_fc_benchmark_nvm"
TRINITYDB_TL2_NVM_BINARY="$BUILD_DIR/benchmarks/trinitydb_tl2_benchmark_nvm"
ROMULUSDB_NVM_BINARY="$BUILD_DIR/benchmarks/romulusdb_benchmark_nvm"

NUM=100000
READS=""
VALUE_SIZE=100
THREADS_SPEC="1-$(nproc)"
SHARDS=""
NVRAM_DIR=""
RUNS=3
OUTPUT="$BUILD_DIR/db_benchmarks.csv"
BENCHMARKS_SPEC=""

DEFAULT_BACKENDS=("leveldb" "bdb" "fossildb" "unordered-map-rwlock" "trinitydb-fc" "trinitydb-tl2" "romulusdb")
BACKENDS=("${DEFAULT_BACKENDS[@]}")

DEFAULT_BENCHMARKS=(
    "fill-seq"
    "fill-random"
    "override"
    "read-seq"
    "read-random"
    "read-write"
)
BENCHMARKS=("${DEFAULT_BENCHMARKS[@]}")

usage() {
    cat <<'EOF'
Usage: run_db_benchmarks.sh [options]

Options:
  --num N             Number of keys written for fill/override workloads. Default: 100000
  --reads N           Number of operations for read and read-write workloads. Default: same as --num
  --value-size N      Value size in bytes. Default: 100
  --threads SPEC      Thread sweep. Supports values, comma lists, and ranges.
                      Examples: 1, 1,2,4,8, 1-16, 1-16:2
                      Bare ranges double each step. Default: 1-$(nproc)
  --shards N          Fixed shard count for fossildb. Default: same as threads
  --nvram-dir DIR     Directory on Optane/NVRAM used for benchmark storage files.
  --runs N            Repetitions per backend/thread pair. Default: 3
  --benchmarks LIST   Comma-separated benchmark ids. Default: all workloads
  --backends LIST     Comma-separated backend ids:
                      leveldb,bdb,fossildb,unordered-map-rwlock,trinitydb-fc,trinitydb-tl2,romulusdb
  --output PATH       Output CSV path. Default: build/db_benchmarks.csv
  --build-dir DIR     CMake build directory. Default: ./build
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

cleanup_run_artifacts() {
    shopt -s nullglob

    local repo_files=("$REPO_ROOT"/fossil_persistence_*)
    local tmp_leveldb=("/tmp"/fossil_leveldb_benchmark_*)
    local tmp_bdb=("/tmp"/fossil_bdb_benchmark_*)
    local trinityfc_files=(
        "/dev/shm"/trinityfc_shared
        "/dev/shm"/trinityvrfc_shared
        "/dev/shm"/trinityvrfc_shared_main
        "/dev/shm"/fossil_trinitydb_fc_benchmark_shared*
    )
    local trinitytl2_files=(
        "/dev/shm"/trinitytl2_shared
        "/dev/shm"/trinityvrtl2_shared
        "/dev/shm"/trinityvrtl2_shared_main
        "/dev/shm"/fossil_trinitydb_tl2_benchmark_shared*
    )
    local romulus_files=(
        "/dev/shm"/romulus_log_shared
        "/dev/shm"/romuluslog_shared
        "/dev/shm"/fossil_romulusdb_benchmark_shared*
    )
    local nvram_fossil=()
    local nvram_leveldb=()
    local nvram_bdb=()
    local nvram_durabletx=()
    local nvram_trinityfc=()
    local nvram_trinitytl2=()
    local nvram_romulus=()

    if [[ -n ${NVRAM_DIR:-} ]]; then
        nvram_fossil=("$NVRAM_DIR"/fossil_persistence_*)
        nvram_leveldb=("$NVRAM_DIR"/fossil_leveldb_benchmark_*)
        nvram_bdb=("$NVRAM_DIR"/fossil_bdb_benchmark_*)
        nvram_durabletx=("$NVRAM_DIR"/fossil_durabletx_ptmdb_benchmark_*)
        nvram_trinityfc=("$NVRAM_DIR"/fossil_trinitydb_fc_benchmark_shared*)
        nvram_trinitytl2=("$NVRAM_DIR"/fossil_trinitydb_tl2_benchmark_shared*)
        nvram_romulus=("$NVRAM_DIR"/fossil_romulusdb_benchmark_shared*)
    fi

    if ((${#repo_files[@]} > 0)); then
        rm -f -- "${repo_files[@]}"
    fi
    if ((${#tmp_leveldb[@]} > 0)); then
        rm -rf -- "${tmp_leveldb[@]}"
    fi
    if ((${#tmp_bdb[@]} > 0)); then
        rm -rf -- "${tmp_bdb[@]}"
    fi
    if ((${#trinityfc_files[@]} > 0)); then
        rm -f -- "${trinityfc_files[@]}"
    fi
    if ((${#trinitytl2_files[@]} > 0)); then
        rm -f -- "${trinitytl2_files[@]}"
    fi
    if ((${#romulus_files[@]} > 0)); then
        rm -f -- "${romulus_files[@]}"
    fi
    if ((${#nvram_fossil[@]} > 0)); then
        rm -f -- "${nvram_fossil[@]}"
    fi
    if ((${#nvram_leveldb[@]} > 0)); then
        rm -rf -- "${nvram_leveldb[@]}"
    fi
    if ((${#nvram_bdb[@]} > 0)); then
        rm -rf -- "${nvram_bdb[@]}"
    fi
    if ((${#nvram_durabletx[@]} > 0)); then
        rm -rf -- "${nvram_durabletx[@]}"
    fi
    if ((${#nvram_trinityfc[@]} > 0)); then
        rm -f -- "${nvram_trinityfc[@]}"
    fi
    if ((${#nvram_trinitytl2[@]} > 0)); then
        rm -f -- "${nvram_trinitytl2[@]}"
    fi
    if ((${#nvram_romulus[@]} > 0)); then
        rm -f -- "${nvram_romulus[@]}"
    fi

    shopt -u nullglob
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

benchmark_supported() {
    local benchmark=$1
    case "$benchmark" in
    fill-seq|fillseq|fill-random|fillrandom|override|overwrite|read-seq|readseq|read-random|readrandom|read-write|readwrite|read-write-50|readwrite50)
        return 0
        ;;
    *)
        return 1
        ;;
    esac
}

normalize_benchmark() {
    local benchmark=$1
    case "$benchmark" in
    fillseq) printf 'fill-seq' ;;
    fillrandom) printf 'fill-random' ;;
    overwrite) printf 'override' ;;
    readseq) printf 'read-seq' ;;
    readrandom) printf 'read-random' ;;
    readwrite|read-write-50|readwrite50) printf 'read-write' ;;
    *) printf '%s' "$benchmark" ;;
    esac
}

backend_supported() {
    local backend=$1
    case "$backend" in
    leveldb|bdb|fossildb|unordered-map-rwlock|umap-rwlock|trinitydb-fc|trinityfc|trinitydb-tl2|trinitytl2|romulusdb|romulus)
        return 0
        ;;
    *)
        return 1
        ;;
    esac
}

parse_csv_list() {
    local spec=$1
    local validator=$2
    local normalizer=$3
    local out_name=$4
    local -n out_ref="$out_name"
    local -a tokens=()
    local -A seen=()
    local token value

    out_ref=()
    IFS=',' read -r -a tokens <<<"$spec"
    for token in "${tokens[@]}"; do
        value=$(trim_spaces "$token")
        [[ -z $value ]] && continue

        if ! "$validator" "$value"; then
            die "Unknown value in list: $value"
        fi

        value=$("$normalizer" "$value")
        if [[ -z ${seen[$value]+x} ]]; then
            out_ref+=("$value")
            seen[$value]=1
        fi
    done

    ((${#out_ref[@]} > 0)) || die "List must contain at least one value"
}

normalize_backend() {
    case "$1" in
    trinityfc) printf 'trinitydb-fc' ;;
    trinitytl2) printf 'trinitydb-tl2' ;;
    romulus) printf 'romulusdb' ;;
    umap-rwlock) printf 'unordered-map-rwlock' ;;
    *) printf '%s' "$1" ;;
    esac
}

binary_for_backend() {
    local backend=$1
    case "$backend" in
    leveldb) printf '%s' "$LEVELDB_BINARY" ;;
    bdb) printf '%s' "$BDB_BINARY" ;;
    fossildb) printf '%s' "$FOSSILDB_BINARY" ;;
    unordered-map-rwlock) printf '%s' "$UNORDERED_MAP_RWLOCK_BINARY" ;;
    trinitydb-fc)
        if [[ -n $NVRAM_DIR ]]; then
            printf '%s' "$TRINITYDB_FC_NVM_BINARY"
        else
            printf '%s' "$TRINITYDB_FC_BINARY"
        fi
        ;;
    trinitydb-tl2)
        if [[ -n $NVRAM_DIR ]]; then
            printf '%s' "$TRINITYDB_TL2_NVM_BINARY"
        else
            printf '%s' "$TRINITYDB_TL2_BINARY"
        fi
        ;;
    romulusdb)
        if [[ -n $NVRAM_DIR ]]; then
            printf '%s' "$ROMULUSDB_NVM_BINARY"
        else
            printf '%s' "$ROMULUSDB_BINARY"
        fi
        ;;
    *) die "No binary configured for backend: $backend" ;;
    esac
}

display_name_for_backend() {
    local backend=$1
    case "$backend" in
    leveldb) printf 'LevelDB' ;;
    bdb) printf 'Berkeley DB' ;;
    fossildb) printf 'FossilDB' ;;
    unordered-map-rwlock) printf 'std::unordered_map + RW lock' ;;
    trinitydb-fc) printf 'TrinityDB (FC)' ;;
    trinitydb-tl2) printf 'TrinityDB (TL2)' ;;
    romulusdb) printf 'RomulusDB' ;;
    *) printf '%s' "$backend" ;;
    esac
}

benchmark_cli_name_for_backend() {
    local backend=$1
    local benchmark=$2

    : "$backend"
    printf '%s' "$benchmark"
}

actual_shards_for_backend() {
    local backend=$1
    local threads=$2

    if [[ $backend != "fossildb" ]]; then
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

parse_raw_output_lines() {
    local backend=$1
    local threads=$2
    local run_index=$3
    local actual_shards=$4
    local stdout_file=$5
    local line benchmark average median min max stddev wall_time throughput
    local matched=0

    while IFS= read -r line; do
        if [[ $line =~ ^([a-z-]+)[[:space:]]+average_ns=([-+0-9.eE]+)[[:space:]]+median_ns=([-+0-9.eE]+)[[:space:]]+min_ns=([-+0-9.eE]+)[[:space:]]+max_ns=([-+0-9.eE]+)[[:space:]]+stddev_ns=([-+0-9.eE]+)[[:space:]]+wall_time_ns=([-+0-9.eE]+)[[:space:]]+throughput_ops_per_s=([-+0-9.eE]+)$ ]]; then
            benchmark=${BASH_REMATCH[1]}
            average=${BASH_REMATCH[2]}
            median=${BASH_REMATCH[3]}
            min=${BASH_REMATCH[4]}
            max=${BASH_REMATCH[5]}
            stddev=${BASH_REMATCH[6]}
            wall_time=${BASH_REMATCH[7]}
            throughput=${BASH_REMATCH[8]}
            matched=1

            printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                "$backend" \
                "$(display_name_for_backend "$backend")" \
                "$benchmark" \
                "$threads" \
                "$actual_shards" \
                "$run_index" \
                "$NUM" \
                "${READS:-$NUM}" \
                "$VALUE_SIZE" \
                "$average" \
                "$median" \
                "$min" \
                "$max" \
                "$stddev" \
                "$wall_time" \
                "$throughput" >>"$OUTPUT"

            printf 'backend=%s benchmark=%s threads=%s run=%s average=%s throughput=%s\n' \
                "$backend" "$benchmark" "$threads" "$run_index" "$average" "$throughput"
        fi
    done <"$stdout_file"

    (( matched == 1 )) || die "Could not parse benchmark output for backend=$backend"
}

run_one() {
    local backend=$1
    local threads=$2
    local run_index=$3
    local benchmark=$4
    local stdout stderr binary actual_shards benchmark_arg
    local -a cmd

    binary=$(binary_for_backend "$backend")
    actual_shards=$(actual_shards_for_backend "$backend" "$threads")
    stdout=$(mktemp)
    stderr=$(mktemp)

    cleanup_run_artifacts

    benchmark_arg=$(benchmark_cli_name_for_backend "$backend" "$benchmark")

    cmd=(
        "$binary"
        "--benchmarks=$benchmark_arg"
        "--num=$NUM"
        "--threads=$threads"
        "--value_size=$VALUE_SIZE"
    )

    if [[ -n $READS ]]; then
        cmd+=("--reads=$READS")
    fi
    cmd+=("--raw")
    if [[ $backend == "fossildb" && -n $SHARDS ]]; then
        cmd+=("--shards=$SHARDS")
    fi
    if [[ -n $NVRAM_DIR ]]; then
        cmd+=("--nvram-dir=$NVRAM_DIR")
    fi

    if ! (
        cd "$REPO_ROOT"
        "${cmd[@]}" >"$stdout" 2>"$stderr"
    ); then
        printf 'Benchmark command failed for backend=%s benchmark=%s threads=%s run=%s.\nCommand:' \
            "$backend" "$benchmark" "$threads" "$run_index" >&2
        printf ' %q' "${cmd[@]}" >&2
        printf '\nstdout:\n' >&2
        cat "$stdout" >&2 || true
        printf '\nstderr:\n' >&2
        cat "$stderr" >&2 || true
        cleanup_run_artifacts
        rm -f "$stdout" "$stderr"
        exit 1
    fi

    if [[ -n $NVRAM_DIR ]] && grep -q "WARNING: running without DAX enabled" "$stderr"; then
        printf 'Benchmark command did not get a DAX/MAP_SYNC mapping for backend=%s benchmark=%s threads=%s run=%s.\nCommand:' \
            "$backend" "$benchmark" "$threads" "$run_index" >&2
        printf ' %q' "${cmd[@]}" >&2
        printf '\nstderr:\n' >&2
        cat "$stderr" >&2 || true
        cleanup_run_artifacts
        rm -f "$stdout" "$stderr"
        exit 1
    fi

    parse_raw_output_lines "$backend" "$threads" "$run_index" "$actual_shards" "$stdout"
    cleanup_run_artifacts
    rm -f "$stdout" "$stderr"
}

while (($# > 0)); do
    case "$1" in
    --num=*)
        NUM=${1#--num=}
        ;;
    --reads=*)
        READS=${1#--reads=}
        ;;
    --value-size=*)
        VALUE_SIZE=${1#--value-size=}
        ;;
    --threads=*)
        THREADS_SPEC=${1#--threads=}
        ;;
    --shards=*)
        SHARDS=${1#--shards=}
        ;;
    --nvram-dir=*)
        NVRAM_DIR=${1#--nvram-dir=}
        ;;
    --nvram-dir)
        shift
        [[ $# -gt 0 ]] || die "--nvram-dir requires a directory argument"
        NVRAM_DIR=$1
        ;;
    --runs=*)
        RUNS=${1#--runs=}
        ;;
    --benchmarks=*)
        BENCHMARKS_SPEC=${1#--benchmarks=}
        ;;
    --backends=*)
        BACKENDS_SPEC=${1#--backends=}
        ;;
    --output=*)
        OUTPUT=${1#--output=}
        ;;
    --build-dir=*)
        BUILD_DIR=${1#--build-dir=}
        LEVELDB_BINARY="$BUILD_DIR/benchmarks/leveldb_db_benchmark"
        BDB_BINARY="$BUILD_DIR/benchmarks/bdb_benchmark"
        FOSSILDB_BINARY="$BUILD_DIR/benchmarks/fossildb_benchmark"
        UNORDERED_MAP_RWLOCK_BINARY="$BUILD_DIR/benchmarks/unordered_map_rwlock_benchmark"
        TRINITYDB_FC_BINARY="$BUILD_DIR/benchmarks/trinitydb_fc_benchmark"
        TRINITYDB_TL2_BINARY="$BUILD_DIR/benchmarks/trinitydb_tl2_benchmark"
        ROMULUSDB_BINARY="$BUILD_DIR/benchmarks/romulusdb_benchmark"
        TRINITYDB_FC_NVM_BINARY="$BUILD_DIR/benchmarks/trinitydb_fc_benchmark_nvm"
        TRINITYDB_TL2_NVM_BINARY="$BUILD_DIR/benchmarks/trinitydb_tl2_benchmark_nvm"
        ROMULUSDB_NVM_BINARY="$BUILD_DIR/benchmarks/romulusdb_benchmark_nvm"
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
[[ -z $READS || $READS =~ ^[0-9]+$ ]] || die "--reads must be a positive integer"
[[ $VALUE_SIZE =~ ^[0-9]+$ ]] || die "--value-size must be a positive integer"
[[ $RUNS =~ ^[0-9]+$ ]] || die "--runs must be a positive integer"
(( NUM > 0 )) || die "--num must be greater than 0"
(( VALUE_SIZE > 0 )) || die "--value-size must be greater than 0"
(( RUNS > 0 )) || die "--runs must be greater than 0"
if [[ -n $READS ]]; then
    (( READS > 0 )) || die "--reads must be greater than 0"
fi

if [[ -n $SHARDS ]]; then
    [[ $SHARDS =~ ^[0-9]+$ ]] || die "--shards must be a positive integer"
    (( SHARDS > 0 )) || die "--shards must be greater than 0"
fi
if [[ -n $NVRAM_DIR && ! -d $NVRAM_DIR ]]; then
    die "--nvram-dir must be an existing directory: $NVRAM_DIR"
fi

declare -a THREAD_VALUES=()
parse_range_spec "$THREADS_SPEC" THREAD_VALUES

if [[ -n ${BENCHMARKS_SPEC:-} ]]; then
    parse_csv_list "$BENCHMARKS_SPEC" benchmark_supported normalize_benchmark BENCHMARKS
fi

if [[ -n ${BACKENDS_SPEC:-} ]]; then
    parse_csv_list "$BACKENDS_SPEC" backend_supported normalize_backend BACKENDS
fi

mkdir -p "$BUILD_DIR"

declare -a CMAKE_TARGETS=()

for backend in "${BACKENDS[@]}"; do
    case "$backend" in
    leveldb) CMAKE_TARGETS+=("leveldb_db_benchmark") ;;
    bdb) CMAKE_TARGETS+=("bdb_benchmark") ;;
    fossildb) CMAKE_TARGETS+=("fossildb_benchmark") ;;
    unordered-map-rwlock) CMAKE_TARGETS+=("unordered_map_rwlock_benchmark") ;;
    trinitydb-fc)
        if [[ -n $NVRAM_DIR ]]; then
            CMAKE_TARGETS+=("trinitydb_fc_benchmark_nvm")
        else
            CMAKE_TARGETS+=("trinitydb_fc_benchmark")
        fi
        ;;
    trinitydb-tl2)
        if [[ -n $NVRAM_DIR ]]; then
            CMAKE_TARGETS+=("trinitydb_tl2_benchmark_nvm")
        else
            CMAKE_TARGETS+=("trinitydb_tl2_benchmark")
        fi
        ;;
    romulusdb)
        if [[ -n $NVRAM_DIR ]]; then
            CMAKE_TARGETS+=("romulusdb_benchmark_nvm")
        else
            CMAKE_TARGETS+=("romulusdb_benchmark")
        fi
        ;;
    esac
done

if ((${#CMAKE_TARGETS[@]} > 0)); then
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR"
    cmake --build "$BUILD_DIR" --target "${CMAKE_TARGETS[@]}"
fi

mkdir -p "$(dirname -- "$OUTPUT")"
printf '%s\n' \
    'backend,backend_display,benchmark,threads,shards,run,num,reads,value_size,average_ns,median_ns,min_ns,max_ns,stddev_ns,wall_time_ns,throughput_ops_per_s' \
    >"$OUTPUT"

for backend in "${BACKENDS[@]}"; do
    for threads in "${THREAD_VALUES[@]}"; do
        for (( run = 1; run <= RUNS; ++run )); do
            for benchmark in "${BENCHMARKS[@]}"; do
                run_one "$backend" "$threads" "$run" "$benchmark"
            done
        done
    done
done
