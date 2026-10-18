#!/usr/bin/env bash
set -euo pipefail
# Run from the repository root on Linux/WSL. Override arguments as needed.
build_dir="${1:-build/profile}"
workload="${2:-noncrossing}"
size="${3:-1000000}"
output_dir="${4:-results/profile}"
mkdir -p "$output_dir"
if ! command -v perf >/dev/null 2>&1; then
    echo 'Linux perf is unavailable. Install the perf package matching your kernel; see docs/performance.md.' >&2
    exit 1
fi
for mode in baseline pool; do
    perf stat -r 3 -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses \
        -o "$output_dir/$mode-stat.txt" -- \
        "$build_dir/apexmatch_bench" --profile-only --workload "$workload" --size "$size" --mode "$mode" --warmups 1 --repeats 3 \
        > "$output_dir/$mode-stat-workload.jsonl"
    perf record -g -o "$output_dir/$mode.data" -- \
        "$build_dir/apexmatch_bench" --profile-only --workload "$workload" --size "$size" --mode "$mode" --warmups 1 --repeats 3 \
        > "$output_dir/$mode-record-workload.jsonl"
    perf report --stdio -i "$output_dir/$mode.data" > "$output_dir/$mode-report.txt"
done
echo "Saved perf counters and call stacks to $output_dir"
