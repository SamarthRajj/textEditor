#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -x "./main_c" ]]; then
    make "$ROOT/main_c"
fi

"$ROOT/scripts/generate_test_files.sh"

RESULTS="$ROOT/bench/results.txt"
mkdir -p "$ROOT/bench"
{
    echo "Benchmark run: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo "Host: $(uname -a)"
    if command -v lscpu >/dev/null 2>&1; then
        echo "CPU: $(lscpu | awk -F: '/Model name/{gsub(/^ +/,"",$2); print $2; exit}')"
    fi
    echo
} > "$RESULTS"

for size in 1mb 10mb 50mb; do
    file="$ROOT/bench/${size}.txt"
    if [[ ! -f "$file" ]]; then
        echo "Missing $file" | tee -a "$RESULTS"
        continue
    fi
    echo "Running benchmark on $file..." | tee -a "$RESULTS"
    if /usr/bin/time -f "external_max_rss_kb: %M" -o "$ROOT/bench/time_${size}.txt" \
        ./main_c --benchmark "$file" 2>>"$RESULTS"; then
        cat "$ROOT/bench/time_${size}.txt" >> "$RESULTS"
    else
        echo "FAILED: $file" | tee -a "$RESULTS"
    fi
    echo >> "$RESULTS"
done

echo "Results written to $RESULTS"
