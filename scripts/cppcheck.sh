#!/usr/bin/env bash
set -euo pipefail

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "cppcheck is not installed. Please install cppcheck and retry." >&2
  exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${1:-${ROOT_DIR}/build/debug}"
COMPILE_DB="${BUILD_DIR}/compile_commands.json"
CPPCHECK_BUILD_DIR="${2:-${BUILD_DIR}/cppcheck-cache}"

if [[ ! -f "${COMPILE_DB}" ]]; then
  cat >&2 <<MSG
compile_commands.json not found at:
  ${COMPILE_DB}

Generate it with:
  cmake --preset debug
  cmake --build --preset debug
MSG
  exit 1
fi

mkdir -p "${CPPCHECK_BUILD_DIR}"

EXCLUDES=()
for dir in build third_party; do
  if [[ -d "${ROOT_DIR}/${dir}" ]]; then
    EXCLUDES+=("-i${ROOT_DIR}/${dir}")
  fi
done

cppcheck \
  --project="${COMPILE_DB}" \
  --cppcheck-build-dir="${CPPCHECK_BUILD_DIR}" \
  --enable=warning,style,performance,portability,information \
  --inconclusive \
  --std=c++17 \
  --error-exitcode=1 \
  --inline-suppr \
  --suppress=missingIncludeSystem \
  --suppress=checkersReport \
  --suppress=syntaxError:*tests/* \
  --suppress=noExplicitConstructor:*span.h \
  --suppress=normalCheckLevelMaxBranches \
  --suppress=*:*/_deps/* \
  --suppress=*:*/third_party/* \
  --suppress=*:*dr_wav* \
  --suppress=unmatchedSuppression \
  --template=gcc \
  -j"$(nproc)" \
  "${EXCLUDES[@]}"

echo "cppcheck completed without errors."
