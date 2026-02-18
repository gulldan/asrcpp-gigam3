# Quality Workflow

This repository uses `scripts/` + CMake custom targets for static checks.

## Prerequisites

- CMake 3.24+
- `clang-format`
- `clang-tidy` (with `run-clang-tidy` or `run-clang-tidy.py`)
- `cppcheck`
- Optional: `include-what-you-use` + `iwyu_tool.py`

Generate compile database first:

```bash
cmake --preset debug
cmake --build --preset debug
```

## Local Commands

Run formatter in-place:

```bash
scripts/format.sh fix
```

Run formatter check (CI style):

```bash
scripts/format.sh check
```

Run clang-tidy:

```bash
scripts/lint.sh build/debug
```

Run cppcheck:

```bash
scripts/cppcheck.sh build/debug
```

Run IWYU:

```bash
scripts/iwyu.sh build/debug
```

Run full checks in one command:

```bash
scripts/check-all.sh build/debug
scripts/check-all.sh build/debug --with-iwyu
```

## CMake Targets

The same checks are available through build targets:

```bash
cmake --build build/debug --target format
cmake --build build/debug --target format-check
cmake --build build/debug --target lint
cmake --build build/debug --target cppcheck
cmake --build build/debug --target iwyu
cmake --build build/debug --target quality
cmake --build build/debug --target quality-full
```

## Recommended Team Workflow

- Keep `format-check` and tests mandatory in CI.
- Run `lint` and `cppcheck` on every merge request.
- For large codebases, use `clang-tidy-diff.py` on changed lines in PRs and run full `lint` nightly.
- Run `iwyu` as a scheduled or pre-release job (it is stricter and toolchain-sensitive).
- Use sanitizer presets (`asan`, `tsan`) regularly for regression detection.

## References (Official Docs)

- CMake Presets: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
- clang-tidy: https://clang.llvm.org/extra/clang-tidy/
- run-clang-tidy: https://clang.llvm.org/extra/clang-tidy/#running-clang-tidy-in-parallel
- clang-tidy-diff: https://clang.llvm.org/extra/clang-tidy/#running-clang-tidy-on-diff
- C++ sanitizers (`-fsanitize`): https://clang.llvm.org/docs/UsersManual.html#controlling-code-generation
- cppcheck manual (`--project=compile_commands.json`): https://cppcheck.sourceforge.io/manual.html
- include-what-you-use: https://include-what-you-use.org/
- IWYU repo docs: https://github.com/include-what-you-use/include-what-you-use
