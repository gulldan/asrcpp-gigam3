#!/usr/bin/env bash
set -euo pipefail

find_run_clang_tidy() {
  if command -v run-clang-tidy >/dev/null 2>&1; then
    command -v run-clang-tidy
    return 0
  fi
  if command -v run-clang-tidy.py >/dev/null 2>&1; then
    command -v run-clang-tidy.py
    return 0
  fi
  return 1
}

if ! RUN_CLANG_TIDY=$(find_run_clang_tidy); then
  echo "run-clang-tidy is not installed. Install clang-tools (or clang-tidy package) and retry." >&2
  exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${1:-${ROOT_DIR}/build/debug}"
COMPILE_DB="${BUILD_DIR}/compile_commands.json"

if [[ ! -f "${COMPILE_DB}" ]]; then
  cat >&2 <<EOF
compile_commands.json not found at:
  ${COMPILE_DB}

Generate it with:
  cmake --preset debug
  cmake --build --preset debug
EOF
  exit 1
fi

TARGET_DIRS=()
for dir in src include tests; do
  if [[ -d "${ROOT_DIR}/${dir}" ]]; then
    TARGET_DIRS+=("${ROOT_DIR}/${dir}")
  fi
done

if [[ ${#TARGET_DIRS[@]} -eq 0 ]]; then
  echo "No target directories found to lint." >&2
  exit 1
fi

ROOT_REGEX=$(printf '%s\n' "${ROOT_DIR}" | sed 's/[][\\.^$*+?(){}|]/\\&/g')

"${RUN_CLANG_TIDY}" \
  -j"$(nproc)" \
  -quiet \
  -p "${BUILD_DIR}" \
  -header-filter="^${ROOT_REGEX}/(src|include|tests)/" \
  "${TARGET_DIRS[@]}"
