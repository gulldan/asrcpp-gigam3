#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)

BUILD_DIR=""
WITH_IWYU=0

for arg in "$@"; do
  case "${arg}" in
    --with-iwyu)
      WITH_IWYU=1
      ;;
    *)
      if [[ -z "${BUILD_DIR}" ]]; then
        BUILD_DIR="${arg}"
      else
        echo "Unknown argument: ${arg}" >&2
        echo "Usage: scripts/check-all.sh [build-dir] [--with-iwyu]" >&2
        exit 1
      fi
      ;;
  esac
done

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/debug}"

echo "[1/4] format-check"
"${SCRIPT_DIR}/format.sh" check

echo "[2/4] clang-tidy"
"${SCRIPT_DIR}/lint.sh" "${BUILD_DIR}"

echo "[3/4] cppcheck"
"${SCRIPT_DIR}/cppcheck.sh" "${BUILD_DIR}"

if [[ ${WITH_IWYU} -eq 1 ]]; then
  if command -v include-what-you-use >/dev/null 2>&1; then
    echo "[4/4] iwyu"
    "${SCRIPT_DIR}/iwyu.sh" "${BUILD_DIR}"
  else
    echo "[4/4] iwyu (SKIPPED: include-what-you-use not installed)"
  fi
else
  echo "[4/4] iwyu (skipped, pass --with-iwyu to enable)"
fi

echo "All enabled checks passed."
