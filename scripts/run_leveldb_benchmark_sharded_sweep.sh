#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

PMEM_SHARDS_RANGE="1-256"
THREADS_RANGE="1-64"
BENCHMARK_SHARDS_RANGE="1-1024"
REPETITIONS=5
BENCHMARKS="fillseq,fillrandom,readrandom"
NUM=""
READS=""
VALUE_SIZE=""
BUILD_DIR="$REPO_ROOT/build"
SOURCE_ROOT="$REPO_ROOT"
BINARY="$BUILD_DIR/benchmarks/leveldb_benchmark_sharded"
BUILD_TARGET="leveldb_benchmark_sharded"
RUN_ROOT="$BUILD_DIR/benchmark_sweep_runs"
OUTPUT="$BUILD_DIR/leveldb_benchmark_sharded_sweep.csv"
APPEND=0
ALLOW_LARGE_SWEEP=0
DRY_RUN=0

HEADER_PATH=""
HEADER_BACKUP=""
ORIGINAL_PMEM_SHARDS=""
ACTIVE_PMEM_SHARDS=""
RESTORE_REQUIRED=0

usage() {
    cat <<'EOF'
Usage: run_leveldb_benchmark_sharded_sweep.sh [options]

Options:
  --pmem-shards-range SPEC        Inclusive ranges or comma-separated values. Bare ranges double each step. Default: 1-256
  --threads-range SPEC            Inclusive ranges or comma-separated values. Bare ranges double each step. Default: 1-64
  --benchmark-shards-range SPEC   Inclusive ranges or comma-separated values. Bare ranges double each step. Default: 1-1024
  --repetitions N                 Process-level repetitions per configuration. Default: 5
  --benchmarks LIST               Value passed to --benchmarks. Default: fillseq,fillrandom,readrandom
  --num N                         Optional benchmark --num value (for example: 100000)
  --reads N                       Optional benchmark --reads value
  --value-size N                  Optional benchmark --value_size value
  --output PATH                   Output CSV path
  --append                        Append to an existing CSV instead of replacing it
  --build-dir PATH                CMake build directory
  --source-root PATH              Repository root
  --binary PATH                   Benchmark binary path
  --build-target NAME             CMake target rebuilt after pmem changes
  --run-root PATH                 Parent directory for isolated temp benchmark runs
  --allow-large-sweep             Required for very large sweeps
  --dry-run                       Validate inputs and print sweep size only
  --help                          Show this help text

Range syntax examples:
  1-8
  3-24
  1-64:2
  1,2,4,8,16
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

    ((${#out_ref[@]} > 0)) || die "At least one value is required in range spec: $spec"
}

resolve_path() {
    local path=$1
    local dir
    local base

    dir=$(dirname -- "$path")
    base=$(basename -- "$path")
    mkdir -p "$dir"
    (
        cd "$dir"
        printf '%s/%s\n' "$PWD" "$base"
    )
}

current_pmem_shards() {
    sed -nE 's/.*using pmem = sharded_pmem<([0-9]+)>;.*/\1/p' "$HEADER_PATH" | head -n 1
}

write_pmem_alias() {
    local pmem_shards=$1
    local temp_file

    temp_file=$(mktemp "${HEADER_PATH}.XXXXXX")
    sed -E "s/using pmem = sharded_pmem<[0-9]+>;/using pmem = sharded_pmem<${pmem_shards}>;/" \
        "$HEADER_PATH" >"$temp_file"
    mv "$temp_file" "$HEADER_PATH"
}

build_target() {
    cmake --build "$BUILD_DIR" --target "$BUILD_TARGET"
}

restore_header() {
    local exit_code=$?
    if [[ -n $HEADER_BACKUP && -f $HEADER_BACKUP ]]; then
        if (( RESTORE_REQUIRED )); then
            cp "$HEADER_BACKUP" "$HEADER_PATH"
            if [[ ${ACTIVE_PMEM_SHARDS:-} != "$ORIGINAL_PMEM_SHARDS" ]]; then
                printf 'Restoring pmem alias to sharded_pmem<%s> and rebuilding\n' "$ORIGINAL_PMEM_SHARDS" >&2
                set +e
                build_target
                set -e
            fi
        fi
        rm -f "$HEADER_BACKUP"
    fi
    exit "$exit_code"
}

prepare_command() {
    local threads=$1
    local benchmark_shards=$2
    local -n cmd_ref=$3

    cmd_ref=(
        "$BINARY"
        "--benchmarks=$BENCHMARKS"
        "--threads=$threads"
        "--shards=$benchmark_shards"
    )

    if [[ -n $NUM ]]; then
        cmd_ref+=("--num=$NUM")
    fi
    if [[ -n $READS ]]; then
        cmd_ref+=("--reads=$READS")
    fi
    if [[ -n $VALUE_SIZE ]]; then
        cmd_ref+=("--value_size=$VALUE_SIZE")
    fi
}

clean_persistence_files() {
    local run_dir=$1
    rm -f "$run_dir"/fossil_persistence_*
}

run_benchmark_once() {
    local threads=$1
    local benchmark_shards=$2
    local repeat_index=$3
    local sample_dir=$4
    local run_dir stdout_file stderr_file benchmark_found=0
    local -a cmd

    prepare_command "$threads" "$benchmark_shards" cmd

    run_dir=$(mktemp -d "$RUN_ROOT/leveldb_benchmark_sharded_XXXXXX")
    stdout_file="$run_dir/stdout.log"
    stderr_file="$run_dir/stderr.log"

    if ! (
        cd "$run_dir"
        "${cmd[@]}" >"$stdout_file" 2>"$stderr_file"
    ); then
        printf 'Benchmark command failed.\nCommand:' >&2
        printf ' %q' "${cmd[@]}" >&2
        printf '\nstdout:\n' >&2
        cat "$stdout_file" >&2 || true
        printf '\nstderr:\n' >&2
        cat "$stderr_file" >&2 || true
        rm -rf "$run_dir"
        exit 1
    fi

    while IFS= read -r line; do
        if [[ $line =~ ^([A-Za-z0-9_]+)[[:space:]]+:[[:space:]]+([0-9.]+)[[:space:]]+micros/op\; ]]; then
            printf '%s,%s\n' "$repeat_index" "${BASH_REMATCH[2]}" >>"$sample_dir/${BASH_REMATCH[1]}.samples"
            benchmark_found=1
        fi
    done <"$stdout_file"

    if (( ! benchmark_found )); then
        printf 'Could not parse benchmark output.\nCommand:' >&2
        printf ' %q' "${cmd[@]}" >&2
        printf '\nstdout:\n' >&2
        cat "$stdout_file" >&2 || true
        printf '\nstderr:\n' >&2
        cat "$stderr_file" >&2 || true
        rm -rf "$run_dir"
        exit 1
    fi

    clean_persistence_files "$run_dir"
    rm -rf "$run_dir"
}

write_csv_header_if_needed() {
    mkdir -p "$(dirname "$OUTPUT")"
    if (( APPEND )); then
        [[ -e $OUTPUT && -s $OUTPUT ]] || printf '%s\n' \
            'pmem_shards,threads,benchmark_shards,benchmark,repetition,micros_per_op' \
            >"$OUTPUT"
    else
        printf '%s\n' \
            'pmem_shards,threads,benchmark_shards,benchmark,repetition,micros_per_op' \
            >"$OUTPUT"
    fi
}

append_result_rows() {
    local pmem_shards=$1
    local threads=$2
    local benchmark_shards=$3
    local sample_dir=$4
    local sample_file benchmark_name repetition micros_per_op
    local -a sample_files

    shopt -s nullglob
    sample_files=("$sample_dir"/*.samples)
    shopt -u nullglob
    ((${#sample_files[@]} > 0)) || die "No samples were collected for the current configuration"

    IFS=$'\n' sample_files=($(printf '%s\n' "${sample_files[@]}" | sort))
    unset IFS

    for sample_file in "${sample_files[@]}"; do
        benchmark_name=$(basename "$sample_file" .samples)
        while IFS=',' read -r repetition micros_per_op; do
            [[ -n $repetition && -n $micros_per_op ]] || die "Malformed sample row in $sample_file"
            printf '%s,%s,%s,%s,%s,%s\n' \
                "$pmem_shards" \
                "$threads" \
                "$benchmark_shards" \
                "$benchmark_name" \
                "$repetition" \
                "$micros_per_op" \
                >>"$OUTPUT"
        done <"$sample_file"
    done
}

parse_args() {
    while (($# > 0)); do
        case "$1" in
            --pmem-shards-range)
                PMEM_SHARDS_RANGE=$2
                shift 2
                ;;
            --threads-range)
                THREADS_RANGE=$2
                shift 2
                ;;
            --benchmark-shards-range)
                BENCHMARK_SHARDS_RANGE=$2
                shift 2
                ;;
            --repetitions)
                REPETITIONS=$2
                shift 2
                ;;
            --benchmarks)
                BENCHMARKS=$2
                shift 2
                ;;
            --num)
                NUM=$2
                shift 2
                ;;
            --reads)
                READS=$2
                shift 2
                ;;
            --value-size)
                VALUE_SIZE=$2
                shift 2
                ;;
            --output)
                OUTPUT=$2
                shift 2
                ;;
            --append)
                APPEND=1
                shift
                ;;
            --build-dir)
                BUILD_DIR=$2
                shift 2
                ;;
            --source-root)
                SOURCE_ROOT=$2
                shift 2
                ;;
            --binary)
                BINARY=$2
                shift 2
                ;;
            --build-target)
                BUILD_TARGET=$2
                shift 2
                ;;
            --run-root)
                RUN_ROOT=$2
                shift 2
                ;;
            --allow-large-sweep)
                ALLOW_LARGE_SWEEP=1
                shift
                ;;
            --dry-run)
                DRY_RUN=1
                shift
                ;;
            --help)
                usage
                exit 0
                ;;
            *)
                die "Unknown argument: $1"
                ;;
        esac
    done
}

main() {
    local -a pmem_values thread_values benchmark_shard_values
    local config_count invocation_count pmem_shards threads benchmark_shards repeat_index
    local sample_dir

    parse_args "$@"

    [[ $REPETITIONS =~ ^[0-9]+$ ]] || die "--repetitions must be a positive integer"
    (( REPETITIONS >= 1 )) || die "--repetitions must be at least 1"

    BUILD_DIR=$(resolve_path "$BUILD_DIR")
    SOURCE_ROOT=$(resolve_path "$SOURCE_ROOT")
    BINARY=$(resolve_path "$BINARY")
    OUTPUT=$(resolve_path "$OUTPUT")
    RUN_ROOT=$(resolve_path "$RUN_ROOT")

    HEADER_PATH="$SOURCE_ROOT/include/fossil/detail/pmem.hpp"
    [[ -f $HEADER_PATH ]] || die "Could not find pmem header at $HEADER_PATH"
    [[ -x $BINARY ]] || die "Benchmark binary is not executable: $BINARY"
    mkdir -p "$RUN_ROOT"

    parse_range_spec "$PMEM_SHARDS_RANGE" pmem_values
    parse_range_spec "$THREADS_RANGE" thread_values
    parse_range_spec "$BENCHMARK_SHARDS_RANGE" benchmark_shard_values

    config_count=$((${#pmem_values[@]} * ${#thread_values[@]} * ${#benchmark_shard_values[@]}))
    invocation_count=$((config_count * REPETITIONS))

    printf 'Sweep size: %s pmem settings x %s thread settings x %s benchmark shard settings = %s configurations\n' \
        "${#pmem_values[@]}" "${#thread_values[@]}" "${#benchmark_shard_values[@]}" "$config_count"
    printf 'Process-level repetitions: %s\n' "$REPETITIONS"
    printf 'Total benchmark invocations: %s\n' "$invocation_count"

    if (( invocation_count > 10000 && ! ALLOW_LARGE_SWEEP )); then
        printf '%s\n' 'Refusing to start such a large sweep without --allow-large-sweep.' >&2
        exit 2
    fi

    if (( DRY_RUN )); then
        exit 0
    fi

    ORIGINAL_PMEM_SHARDS=$(current_pmem_shards)
    [[ -n $ORIGINAL_PMEM_SHARDS ]] || die "Could not determine the current pmem shard count"
    ACTIVE_PMEM_SHARDS=$ORIGINAL_PMEM_SHARDS
    HEADER_BACKUP=$(mktemp "${RUN_ROOT}/pmem.hpp.backup.XXXXXX")
    cp "$HEADER_PATH" "$HEADER_BACKUP"
    trap restore_header EXIT

    write_csv_header_if_needed

    for pmem_shards in "${pmem_values[@]}"; do
        if [[ $pmem_shards != "$ACTIVE_PMEM_SHARDS" ]]; then
            printf 'Updating pmem alias to sharded_pmem<%s> and rebuilding\n' "$pmem_shards"
            write_pmem_alias "$pmem_shards"
            build_target
            ACTIVE_PMEM_SHARDS=$pmem_shards
            RESTORE_REQUIRED=1
        fi

        for threads in "${thread_values[@]}"; do
            for benchmark_shards in "${benchmark_shard_values[@]}"; do
                printf 'Running pmem_shards=%s threads=%s benchmark_shards=%s\n' \
                    "$pmem_shards" "$threads" "$benchmark_shards"
                sample_dir=$(mktemp -d "$RUN_ROOT/samples.XXXXXX")

                for (( repeat_index = 1; repeat_index <= REPETITIONS; repeat_index += 1 )); do
                    printf '  repetition %s/%s\n' "$repeat_index" "$REPETITIONS"
                    run_benchmark_once "$threads" "$benchmark_shards" "$repeat_index" "$sample_dir"
                done

                append_result_rows "$pmem_shards" "$threads" "$benchmark_shards" "$sample_dir"
                rm -rf "$sample_dir"
            done
        done
    done
}

main "$@"
