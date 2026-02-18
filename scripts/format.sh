#!/usr/bin/env bash
set -euo pipefail

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is not installed. Please install ripgrep and retry." >&2
  exit 1
fi

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format is not installed. Please install it (for example: apt install clang-format)." >&2
  exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
MODE="${1:-fix}"

if [[ "${MODE}" != "fix" && "${MODE}" != "check" ]]; then
  cat >&2 <<EOF
Usage:
  scripts/format.sh [fix|check]

Modes:
  fix    Format files in place (default)
  check  Verify formatting only (non-zero exit on mismatch)
EOF
  exit 1
fi

SEARCH_DIRS=()
for dir in src include tests; do
  if [[ -d "${ROOT_DIR}/${dir}" ]]; then
    SEARCH_DIRS+=("${dir}")
  fi
done

if [[ ${#SEARCH_DIRS[@]} -eq 0 ]]; then
  echo "No source directories found (expected at least one of: src, include, tests)."
  exit 0
fi

mapfile -d '' FILES < <(
  cd "${ROOT_DIR}"
  rg --files -0 "${SEARCH_DIRS[@]}" \
    --iglob '*.c' \
    --iglob '*.cc' \
    --iglob '*.cpp' \
    --iglob '*.cxx' \
    --iglob '*.h' \
    --iglob '*.hh' \
    --iglob '*.hpp' \
    --iglob '*.hxx'
)

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No C/C++ files found. Nothing to do."
  exit 0
fi

if [[ "${MODE}" == "check" ]]; then
  (
    cd "${ROOT_DIR}"
    printf '%s\0' "${FILES[@]}" | xargs -0 clang-format --dry-run --Werror
  )
  echo "clang-format check passed."
else
  (
    cd "${ROOT_DIR}"
    printf '%s\0' "${FILES[@]}" | xargs -0 clang-format -i
  )
  echo "clang-format completed."
fi
