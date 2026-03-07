#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)

if [[ $# -gt 0 ]]; then
  PRESETS=("$@")
else
  PRESETS=(debug coverage asan tsan)
fi

JOBS=${JOBS:-$(nproc)}

for preset in "${PRESETS[@]}"; do
  echo "=== [${preset}] configure ==="
  cmake --preset "${preset}"

  echo "=== [${preset}] build ==="
  cmake --build "${ROOT_DIR}/build/${preset}" --parallel "${JOBS}"

  echo "=== [${preset}] test ==="
  if ctest --preset "${preset}" -N >/dev/null 2>&1; then
    ctest --preset "${preset}"
  else
    ctest --test-dir "${ROOT_DIR}/build/${preset}" --output-on-failure
  fi
done

echo "All requested preset test runs passed."
