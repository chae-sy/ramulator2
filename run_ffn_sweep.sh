#!/usr/bin/env bash
set -euo pipefail

CONFIG="my_trace.yaml"
BITS_LIST=(2 4 6 8 16)

for bits in "${BITS_LIST[@]}"; do
  echo "========================================"
  echo "Running ffn_bits=${bits}"
  echo "========================================"

  sed -i -E \
    "s|(path:[[:space:]]+.*_ffn)[0-9]+(\.trace)|\1${bits}\2|" \
    "$CONFIG"

  echo "[INFO] Updated path in $CONFIG:"
  grep "path:" "$CONFIG"

  ./ramulator2 -f "$CONFIG" | tee "ramulator_ffn${bits}.log"
done