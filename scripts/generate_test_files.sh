#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT/bench"
mkdir -p "$BENCH_DIR"

# ~80 chars per line; line count chosen for approximate file sizes
generate_file() {
    local path="$1"
    local lines="$2"
    python3 - "$path" "$lines" <<'PY'
import sys
path, lines = sys.argv[1], int(sys.argv[2])
line = "A" * 80 + "\n"
with open(path, "w", encoding="utf-8") as f:
    for _ in range(lines):
        f.write(line)
PY
    echo "Created $path ($(wc -c < "$path") bytes, $lines lines)"
}

generate_file "$BENCH_DIR/1mb.txt" 12500
generate_file "$BENCH_DIR/10mb.txt" 125000
generate_file "$BENCH_DIR/50mb.txt" 625000

echo "Test files ready in $BENCH_DIR"
