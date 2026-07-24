#!/usr/bin/env bash
#
# lint.sh — run the same code-style checks locally that CI enforces
# (.github/workflows/lint.yml), so you can catch (and auto-fix) problems before
# pushing. The checks cover far more than the C/C++ sources:
#
#   * clang-format          — C/C++ formatting (src/ + tests/)
#   * clang-tidy            — C/C++ static analysis (SDK-free updater logic)
#   * cmake-format          — CMakeLists.txt layout (.cmake-format.yaml)
#   * cmake-lint            — bug-prone CMake patterns
#   * actionlint            — GitHub workflow validity (+ shellcheck on inline
#                             run: scripts)
#   * shellcheck            — the stand-alone scripts/*.sh
#   * PSScriptAnalyzer      — PowerShell static analysis for scripts/*.ps1
#                             (PSScriptAnalyzerSettings.psd1)
#   * yamllint              — structural YAML style (.yamllint.yaml)
#   * editorconfig-checker  — final newline / trailing whitespace / UTF-8 / LF
#                             across every text file (.editorconfig)
#
# Usage:
#   scripts/lint.sh            # check only; non-zero exit if anything is off
#   scripts/lint.sh --fix      # auto-fix what can be fixed (clang-format,
#                              # clang-tidy, cmake-format), then check the rest
#
# Any tool that is not installed is reported and skipped, so a partial local run
# still works — CI remains the complete gate. Install hints are printed per tool.
# Pin clang to a version with CLANG_SUFFIX, e.g.  CLANG_SUFFIX=-18 scripts/lint.sh

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

SUFFIX="${CLANG_SUFFIX:-}"
CLANG_FORMAT="clang-format${SUFFIX}"
CLANG_TIDY="clang-tidy${SUFFIX}"
FIX=0
[ "${1:-}" = "--fix" ] && FIX=1

# Track overall result and what was skipped, so the summary is honest about
# which checks actually ran.
FAILED=0
SKIPPED=()

have() { command -v "$1" >/dev/null 2>&1; }

# skip <tool> <install-hint>: note a missing tool and keep going.
skip() {
	echo "==> $1: NOT INSTALLED — skipping ($2)"
	SKIPPED+=("$1")
}

# report <label>: turn the previous command's exit status into "OK" or a
# recorded failure. Usage:  some_command; report "some-tool"
report() {
	if [ "$?" -eq 0 ]; then
		echo "    OK"
	else
		echo "    FAILED: $1"
		FAILED=1
	fi
}

# ---------------------------------------------------------------------------
# clang-format — C/C++ formatting. Skips the plist template.
if have "$CLANG_FORMAT"; then
	mapfile -t CXX_FILES < <(find src tests \
		\( -name '*.cpp' -o -name '*.h' \) ! -name '*.plist.in' | sort)
	echo "==> clang-format (${#CXX_FILES[@]} files)"
	if [ "$FIX" -eq 1 ]; then
		"$CLANG_FORMAT" -i "${CXX_FILES[@]}"
		echo "    formatted in place"
	else
		"$CLANG_FORMAT" --dry-run --Werror "${CXX_FILES[@]}"
		report "clang-format"
	fi
else
	skip "$CLANG_FORMAT" "apt-get install clang-format, or set CLANG_SUFFIX"
fi

# ---------------------------------------------------------------------------
# clang-tidy — C/C++ static analysis over the SDK-free updater logic. Needs a
# compile database describing how each translation unit is built.
if have "$CLANG_TIDY" && have cmake; then
	echo "==> clang-tidy (SDK-free updater logic)"
	if cmake -S . -B build-lint \
		-DCMAKE_BUILD_TYPE=Debug \
		-DVW_BUILD_PLUGIN=OFF \
		-DVW_BUILD_TESTS=ON \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null; then
		TIDY_ARGS=(-p build-lint --warnings-as-errors='*')
		[ "$FIX" -eq 1 ] && TIDY_ARGS+=(--fix --fix-errors)
		"$CLANG_TIDY" "${TIDY_ARGS[@]}" src/UpdaterFlow.cpp
		report "clang-tidy"
	else
		false
		report "clang-tidy (cmake configure)"
	fi
else
	skip "$CLANG_TIDY" "apt-get install clang-tidy cmake"
fi

# ---------------------------------------------------------------------------
# cmake-format + cmake-lint — CMake layout and bug-prone patterns. Both ship in
# the "cmakelang" pip package.
CMAKE_FILES=(CMakeLists.txt tests/CMakeLists.txt)
if have cmake-format; then
	echo "==> cmake-format (${#CMAKE_FILES[@]} files)"
	if [ "$FIX" -eq 1 ]; then
		cmake-format -i "${CMAKE_FILES[@]}"
		echo "    formatted in place"
	else
		cmake-format --check "${CMAKE_FILES[@]}"
		report "cmake-format"
	fi
else
	skip "cmake-format" "pip install cmakelang pyyaml"
fi

if have cmake-lint; then
	echo "==> cmake-lint"
	cmake-lint "${CMAKE_FILES[@]}"
	report "cmake-lint"
else
	skip "cmake-lint" "pip install cmakelang pyyaml"
fi

# ---------------------------------------------------------------------------
# actionlint — GitHub workflow validity. Uses shellcheck for inline run: scripts
# when it is on PATH.
if have actionlint; then
	echo "==> actionlint"
	actionlint
	report "actionlint"
else
	skip "actionlint" "go install github.com/rhysd/actionlint/cmd/actionlint@latest"
fi

# ---------------------------------------------------------------------------
# Run shellcheck over the stand-alone shell scripts. (A comment must not start
# with the word "shellcheck" or the tool reads it as a directive.)
if have shellcheck; then
	echo "==> shellcheck (scripts/*.sh)"
	shellcheck scripts/*.sh
	report "shellcheck"
else
	skip "shellcheck" "apt-get install shellcheck"
fi

# ---------------------------------------------------------------------------
# PSScriptAnalyzer — static analysis for the PowerShell updater script under
# scripts/ (the same production-only scope as clang-tidy and shellcheck; the test
# harness under tests/ is not analysed). Needs pwsh AND the PSScriptAnalyzer
# module. Rules: PSScriptAnalyzerSettings.psd1. With --fix, the correctable rules
# are auto-applied in place.
if have pwsh; then
	if pwsh -NoProfile -Command 'exit ([bool](Get-Module -ListAvailable PSScriptAnalyzer) ? 0 : 1)' \
		>/dev/null 2>&1; then
		echo "==> PSScriptAnalyzer (scripts/*.ps1)"
		PSSA_FIX="\$false"
		[ "$FIX" -eq 1 ] && PSSA_FIX="\$true"
		pwsh -NoProfile -Command "
			\$ErrorActionPreference = 'Stop'
			Import-Module PSScriptAnalyzer
			\$p = @{ Path = 'scripts'; Settings = './PSScriptAnalyzerSettings.psd1'; Fix = $PSSA_FIX }
			\$r = Invoke-ScriptAnalyzer @p
			if (\$r) {
				\$r | Format-Table -AutoSize Severity, RuleName, ScriptName, Line, Message
				exit 1
			}
			exit 0"
		report "PSScriptAnalyzer"
	else
		skip "PSScriptAnalyzer" "pwsh: Install-Module PSScriptAnalyzer -Scope CurrentUser"
	fi
else
	skip "pwsh (PSScriptAnalyzer)" "install PowerShell 7 (pwsh)"
fi

# ---------------------------------------------------------------------------
# yamllint — structural YAML style.
if have yamllint; then
	echo "==> yamllint"
	yamllint --strict .
	report "yamllint"
else
	skip "yamllint" "pip install yamllint"
fi

# ---------------------------------------------------------------------------
# editorconfig-checker — whitespace/encoding hygiene across every text file. The
# binary is "editorconfig-checker" (sometimes installed as "ec").
EC=""
if have editorconfig-checker; then
	EC=editorconfig-checker
elif have ec; then
	EC=ec
fi
if [ -n "$EC" ]; then
	echo "==> editorconfig-checker"
	"$EC"
	report "editorconfig-checker"
else
	skip "editorconfig-checker" \
		"go install github.com/editorconfig-checker/editorconfig-checker/v3/cmd/editorconfig-checker@latest"
fi

# ---------------------------------------------------------------------------
echo
if [ "${#SKIPPED[@]}" -gt 0 ]; then
	echo "Skipped (not installed): ${SKIPPED[*]}"
fi
if [ "$FAILED" -ne 0 ]; then
	echo "Some lint checks FAILED."
	exit 1
fi
echo "All available lint checks passed."
