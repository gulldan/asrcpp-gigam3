#!/usr/bin/env bash
set -euo pipefail

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is not installed. Please install ripgrep and retry." >&2
  exit 1
fi

if ! command -v include-what-you-use >/dev/null 2>&1; then
  echo "include-what-you-use is not installed. Please install IWYU and retry." >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required to run iwyu_tool.py." >&2
  exit 1
fi

find_iwyu_tool() {
  if command -v iwyu_tool.py >/dev/null 2>&1; then
    command -v iwyu_tool.py
    return 0
  fi
  if command -v iwyu-tool >/dev/null 2>&1; then
    command -v iwyu-tool
    return 0
  fi

  local candidate
  while IFS= read -r candidate; do
    if [[ -x "${candidate}" ]]; then
      echo "${candidate}"
      return 0
    fi
  done < <(compgen -G "/usr/lib/llvm-*/share/include-what-you-use/iwyu_tool.py" || true)

  if [[ -x "/usr/share/include-what-you-use/iwyu_tool.py" ]]; then
    echo "/usr/share/include-what-you-use/iwyu_tool.py"
    return 0
  fi

  return 1
}

if ! IWYU_TOOL=$(find_iwyu_tool); then
  cat >&2 <<MSG
iwyu_tool.py was not found.
Install the include-what-you-use tooling package that provides iwyu_tool.py,
or add it to PATH and retry.
MSG
  exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${1:-${ROOT_DIR}/build/debug}"
COMPILE_DB="${BUILD_DIR}/compile_commands.json"

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

SEARCH_DIRS=()
for dir in src tests; do
  if [[ -d "${ROOT_DIR}/${dir}" ]]; then
    SEARCH_DIRS+=("${dir}")
  fi
done

if [[ ${#SEARCH_DIRS[@]} -eq 0 ]]; then
  echo "No source directories found (expected at least src/ or tests/)."
  exit 0
fi

mapfile -d '' FILES < <(
  cd "${ROOT_DIR}"
  rg --files -0 "${SEARCH_DIRS[@]}" \
    --iglob '*.c' \
    --iglob '*.cc' \
    --iglob '*.cpp' \
    --iglob '*.cxx'
)

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No C/C++ source files found in src/ or tests/."
  exit 0
fi

IWYU_ARGS=(
  -w
  -Wno-unknown-warning-option
  -Wno-unused-parameter
  -Xiwyu
  --error_always
)
if [[ -f "${ROOT_DIR}/.iwyu.imp" ]]; then
  IWYU_ARGS+=(-Xiwyu "--mapping_file=${ROOT_DIR}/.iwyu.imp")
fi

IWYU_OUTPUT=$(
  cd "${ROOT_DIR}"
  python3 "${IWYU_TOOL}" \
    -p "${BUILD_DIR}" \
    -j "$(nproc)" \
    "${FILES[@]}" \
    -- \
    "${IWYU_ARGS[@]}" 2>&1
) || true

filter_iwyu_output() {
  python3 - <<'PY'
import sys

lines = sys.stdin.readlines()
out = []
i = 0
suppressed = 0

while i < len(lines):
    line = lines[i]
    if line.startswith("/") and " should add these lines:" in line:
        block = [line]
        j = i + 1
        while j < len(lines):
            block.append(lines[j])
            if lines[j].strip() == "---":
                j += 1
                break
            j += 1
        block_text = "".join(block)
        is_math_false_positive = (
            "#include <math>" in block_text
            and "#include <math.h>" in block_text
            and "should add these lines:" in block_text
            and "should remove these lines:" in block_text
        )
        if is_math_false_positive:
            suppressed += 1
        else:
            out.extend(block)
        i = j
        continue

    out.append(line)
    i += 1

sys.stdout.write("".join(out))
if suppressed:
    sys.stdout.write(f"IWYU note: suppressed {suppressed} Darwin <math>/<math.h> false-positive block(s).\n")
PY
}

IWYU_OUTPUT="$(printf '%s' "${IWYU_OUTPUT}" | filter_iwyu_output)"

echo "${IWYU_OUTPUT}"

if echo "${IWYU_OUTPUT}" | grep -q "should add these lines\|should remove these lines"; then
  echo ""
  echo "IWYU found include violations (see above)."
  exit 1
fi

echo "IWYU completed without include violations."
