#!/usr/bin/env bash
#
#	tidy-cache.test.sh
#
#	Unit tests for the clang-tidy result cache: the per-translation-unit key
#	(scripts/tidy-cache-key.py) and the reuse it drives inside the SDK tidy
#	runner (scripts/clang-tidy-sdk.sh -c).
#
#	What these tests are actually guarding
#	-------------------------------------
#	A lint cache trades work for a promise: **"nothing that could change a
#	diagnostic has changed."** Break that promise and CI goes green on code
#	nobody analysed — the worst failure this repository can have, because it is
#	silent. So the property under test is not "is it faster"; it is
#
#	    **anything that can change a diagnostic must invalidate the entry.**
#
#	Each case below names one input that must do so: the translation unit
#	itself, a header it reaches TRANSITIVELY, the rules (.clang-tidy), the SDK
#	identity, the compile flags. The reuse case (2) is the only one about speed,
#	and it exists mostly to prove the others are not passing by accident.
#
#	The mirror-image property is that the cache never HIDES a diagnostic: a
#	translation unit clang-tidy complained about is not remembered as clean
#	(case 6), and one whose inputs cannot be pinned down is always analysed
#	(cases 7-9).
#
#	How they run
#	------------
#	Against a SYNTHETIC repository built in a temp dir: copies of the two
#	scripts plus a small src/ tree whose files match the runner's hardcoded
#	globs (src/draw/*.cpp, src/Extensions/*.cpp, src/payload/*.cpp and the five
#	named glue units). That is what makes headers editable — the real tree
#	cannot be modified by a test — and it keeps the run to 9 tiny translation
#	units. clang-tidy itself is a stub that records which files it was asked to
#	analyse, so "was this reused?" is answered by the stub's own log rather than
#	by parsing timings.
#

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_RUNNER="${HERE}/../scripts/clang-tidy-sdk.sh"
SRC_KEYGEN="${HERE}/../scripts/tidy-cache-key.py"

# ---------------------------------------------------------------------------
# Missing-tool policy — identical to tests/ci-wait.test.sh: skip locally,
# hard-fail in CI (VW_REQUIRE_SCRIPT_TESTS), so a silent skip can never let the
# suite "pass" without running a single check.
# ---------------------------------------------------------------------------
REQUIRE_TOOLS="${VW_REQUIRE_SCRIPT_TESTS:-}"
case "$REQUIRE_TOOLS" in
	'' | 0 | off | OFF | false | FALSE | no | NO) REQUIRE_TOOLS="" ;;
esac

skip_or_fail() {
	if [ -n "$REQUIRE_TOOLS" ]; then
		echo "ERROR tidy-cache.test.sh: $1 (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)." >&2
		exit 1
	fi
	echo "SKIP tidy-cache.test.sh: $1."
	exit 0
}

PYTHON=""
for candidate in python3 python; do
	if command -v "$candidate" >/dev/null 2>&1; then
		PYTHON="$candidate"
		break
	fi
done
[ -n "$PYTHON" ] || skip_or_fail "'python3' not found (the cache key needs it)"
for f in "$SRC_RUNNER" "$SRC_KEYGEN"; do
	[ -f "$f" ] || skip_or_fail "$f not found"
done

# ---------------------------------------------------------------------------
# Tiny assertion harness, styled after tests/ci-wait.test.sh.
# ---------------------------------------------------------------------------
TESTS_RUN=0
TESTS_FAILED=0
CURRENT="(none)"

t() { CURRENT="$1"; }

check_eq() { # actual expected [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	if [ "$1" != "$2" ]; then
		TESTS_FAILED=$((TESTS_FAILED + 1))
		printf 'FAIL [%s] %s\n  expected: %s\n  actual:   %s\n' \
			"$CURRENT" "${3:-values differ}" "$2" "$1"
	fi
}

check_ne() { # actual unexpected [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	if [ "$1" = "$2" ]; then
		TESTS_FAILED=$((TESTS_FAILED + 1))
		printf 'FAIL [%s] %s\n  must differ from: %s\n' "$CURRENT" "${3:-values equal}" "$2"
	fi
}

check_contains() { # haystack needle [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	case "$1" in
		*"$2"*) : ;;
		*)
			TESTS_FAILED=$((TESTS_FAILED + 1))
			printf 'FAIL [%s] %s\n  missing:  %s\n  in:\n%s\n' \
				"$CURRENT" "${3:-substring not found}" "$2" "$1"
			;;
	esac
}

check_not_contains() { # haystack needle [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	case "$1" in
		*"$2"*)
			TESTS_FAILED=$((TESTS_FAILED + 1))
			printf 'FAIL [%s] %s\n  unexpected: %s\n  in:\n%s\n' \
				"$CURRENT" "${3:-substring present}" "$2" "$1"
			;;
		*) : ;;
	esac
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# The synthetic repository.
#
# The runner's translation-unit list is DELIBERATELY hardcoded (a name-by-name
# list once let a new module go unanalysed — see the script), so the way to
# drive it is to build a tree its globs match rather than to teach it an
# override. Nine tiny units, three headers, one of them reached only
# transitively (Alpha.cpp -> core/Shared.h -> core/Deep.h) because that is the
# edge a naive "did the .cpp change?" key would miss.
# ---------------------------------------------------------------------------
REPO="$WORK/repo"
mkdir -p "$REPO/scripts" "$REPO/src/draw" "$REPO/src/Extensions" "$REPO/src/payload" "$REPO/src/core"
cp "$SRC_RUNNER" "$REPO/scripts/clang-tidy-sdk.sh"
cp "$SRC_KEYGEN" "$REPO/scripts/tidy-cache-key.py"
chmod +x "$REPO/scripts/clang-tidy-sdk.sh"

printf 'Checks: -*\n' >"$REPO/.clang-tidy"

printf '#include "core/Shared.h"\nint alpha() { return 1; }\n' >"$REPO/src/draw/Alpha.cpp"
printf 'int beta() { return 2; }\n' >"$REPO/src/draw/Beta.cpp"
printf '#include "core/Other.h"\nint ext() { return 3; }\n' >"$REPO/src/Extensions/ExtAlpha.cpp"
printf 'int payload() { return 4; }\n' >"$REPO/src/payload/PayloadMain.cpp"
for named in ModuleMain Updater PayloadHost PayloadSession FeedbackLoopHost; do
	printf 'int %s_unit() { return 0; }\n' "$named" >"$REPO/src/$named.cpp"
done

printf '#pragma once\n#include "core/Deep.h"\n' >"$REPO/src/core/Shared.h"
printf '#pragma once\nstatic const int kDeep = 1;\n' >"$REPO/src/core/Deep.h"
printf '#pragma once\nstatic const int kOther = 1;\n' >"$REPO/src/core/Other.h"

UNITS=9

# The compile database. Nothing is compiled, so the command only has to be the
# string the key folds in — but it carries the real shape (flags + the file) so
# a flag change is a realistic edit (case 5).
write_db() { # extra-define
	local extra="${1:-}" first=1 f
	{
		printf '[\n'
		for f in src/draw/Alpha.cpp src/draw/Beta.cpp src/Extensions/ExtAlpha.cpp \
			src/payload/PayloadMain.cpp src/ModuleMain.cpp src/Updater.cpp \
			src/PayloadHost.cpp src/PayloadSession.cpp src/FeedbackLoopHost.cpp; do
			[ "$first" -eq 1 ] || printf ',\n'
			first=0
			printf '  {"directory": "%s", "file": "%s/%s", "command": "c++ -I%s/src %s -c %s/%s"}' \
				"$REPO" "$REPO" "$f" "$REPO" "$extra" "$REPO" "$f"
		done
		printf '\n]\n'
	} >"$REPO/compile_commands.json"
}
write_db

# The stub clang-tidy: records the unit it was handed, and can be told to
# complain about one of them (case 6).
BIN="$WORK/bin"
mkdir -p "$BIN"
cat >"$BIN/clang-tidy" <<'STUB'
#!/usr/bin/env bash
for a in "$@"; do
	if [ "$a" = "--version" ]; then
		# 本物の clang-tidy と同じ形。最後の行が**実行機ごとに変わる**ところで、
		# GitHub のランナープールは機種が混在している。
		echo "stub LLVM version 18.0.0"
		echo "  Optimized build."
		echo "  Default target: arm64-apple-darwin24.0.0"
		echo "  Host CPU: ${TIDY_HOST_CPU:-apple-m1}"
		exit 0
	fi
done
eval "file=\${$#}"
echo "$file" >>"$TIDY_CALLS"
if [ -n "${TIDY_FAIL_ON:-}" ] && [ "$file" = "$TIDY_FAIL_ON" ]; then
	echo "$file:1:1: error: stubbed diagnostic [stub-check]"
	exit 1
fi
exit 0
STUB
chmod +x "$BIN/clang-tidy"

CACHE="$WORK/cache"
CALLS="$WORK/calls.log"

# Runs the real runner over the synthetic repo. Sets OUT / RC / CALLED.
run_tidy() { # [extra args...]
	: >"$CALLS"
	OUT="$(TIDY_CALLS="$CALLS" TIDY_FAIL_ON="${FAIL_ON:-}" VW_SDK_CACHE_KEY="${SDK_KEY:-sdk-v1}" \
		"$REPO/scripts/clang-tidy-sdk.sh" -p "$REPO" -t "$BIN/clang-tidy" -j 2 "$@" 2>&1)"
	RC="$?"
	# awk は空のファイルでも 0 を出して正常終了する（grep -c は 0 を出しつつ 1 を返すので、
	# `|| echo 0` を添えると "0" が 2 行になって数の比較が壊れる）。
	CALLED="$(awk 'END {print NR}' "$CALLS" 2>/dev/null || echo 0)"
}

# ---------------------------------------------------------------------------
t "cold run analyses everything and fills the cache"
run_tidy -c "$CACHE"
check_eq "$RC" "0" "a clean cold run succeeds"
check_eq "$CALLED" "$UNITS" "every unit is handed to clang-tidy"
check_contains "$OUT" "0 reused / $UNITS analysed" "and none of it was reused"

# ---------------------------------------------------------------------------
t "warm run reuses every entry"
run_tidy -c "$CACHE"
check_eq "$RC" "0" "reuse still succeeds"
check_eq "$CALLED" "0" "clang-tidy is not invoked at all"
check_contains "$OUT" "$UNITS reused / 0 analysed" "the summary says so"
check_contains "$OUT" "src/draw/Alpha.cpp (cached)" "and each line says which units were reused"

# ---------------------------------------------------------------------------
# The case that matters most: a header the unit only reaches through ANOTHER
# header. Alpha.cpp does not mention Deep.h; it includes Shared.h, which does.
t "a transitively included header invalidates only what reaches it"
printf '#pragma once\nstatic const int kDeep = 2;\n' >"$REPO/src/core/Deep.h"
run_tidy -c "$CACHE"
check_eq "$RC" "0" "still succeeds"
check_eq "$CALLED" "1" "exactly the one unit that reaches the header is re-analysed"
check_eq "$(cat "$CALLS")" "src/draw/Alpha.cpp" "and it is the right one"

# ---------------------------------------------------------------------------
t "editing the translation unit itself invalidates it"
printf 'int beta() { return 20; }\n' >"$REPO/src/draw/Beta.cpp"
run_tidy -c "$CACHE"
check_eq "$CALLED" "1" "only the edited unit is re-analysed"
check_eq "$(cat "$CALLS")" "src/draw/Beta.cpp" "and it is the right one"

# ---------------------------------------------------------------------------
t "the rules and the compile flags invalidate everything"
printf 'Checks: -*,readability-*\n' >"$REPO/.clang-tidy"
run_tidy -c "$CACHE"
check_eq "$CALLED" "$UNITS" "a .clang-tidy edit re-analyses every unit"
run_tidy -c "$CACHE"
check_eq "$CALLED" "0" "(settled again)"
write_db "-DVW_DEV_BUILD=1"
run_tidy -c "$CACHE"
check_eq "$CALLED" "$UNITS" "a compile-flag change re-analyses every unit"

# ---------------------------------------------------------------------------
t "the SDK identity invalidates everything"
run_tidy -c "$CACHE"
check_eq "$CALLED" "0" "(settled)"
SDK_KEY="sdk-v2"
run_tidy -c "$CACHE"
check_eq "$CALLED" "$UNITS" "a new VW_SDK_CACHE_KEY re-analyses every unit"
unset SDK_KEY

# ---------------------------------------------------------------------------
# The cache must never remember a complaint as silence.
t "a unit clang-tidy complained about is not remembered as clean"
rm -rf "$CACHE"
FAIL_ON="src/draw/Beta.cpp"
run_tidy -c "$CACHE"
check_eq "$RC" "1" "the diagnostic fails the run"
check_contains "$OUT" "stubbed diagnostic" "and is reported"
run_tidy -c "$CACHE"
check_eq "$RC" "1" "it fails again rather than being cached green"
check_contains "$OUT" "stubbed diagnostic" "with the same diagnostic"
check_eq "$(cat "$CALLS")" "src/draw/Beta.cpp" "only that unit is re-analysed"
unset FAIL_ON
run_tidy -c "$CACHE"
check_eq "$RC" "0" "and it is cached once it comes back clean"

# ---------------------------------------------------------------------------
# When the inputs cannot be pinned down, the answer is "analyse", never "skip".
t "a unit whose includes cannot be resolved is always analysed"
printf '#define PICK "core/Deep.h"\n#include PICK\nint beta() { return 20; }\n' \
	>"$REPO/src/draw/Beta.cpp"
run_tidy -c "$CACHE"
check_contains "$(cat "$CALLS")" "src/draw/Beta.cpp" "it is analysed"
check_contains "$OUT" "(cache)" "and the reason is printed rather than swallowed"
run_tidy -c "$CACHE"
check_contains "$(cat "$CALLS")" "src/draw/Beta.cpp" "again on the next run — never cached"
printf 'int beta() { return 20; }\n' >"$REPO/src/draw/Beta.cpp"

# ---------------------------------------------------------------------------
# 実機で最初に壊れたのがここ。キャッシュは actions/cache から復元できているのに
# 1 件も再利用されず（0 reused / 21 analysed）、速さのための仕組みが黙って何も
# しなくなっていた。原因は鍵に入れていた `clang-tidy --version` の出力で、そこには
# `Host CPU:` という**実行機ごとに変わる行**がある。ランナーが変われば鍵も変わる。
t "the key does not depend on the machine clang-tidy runs on"
rm -rf "$CACHE"
export TIDY_HOST_CPU="apple-m1"
run_tidy -c "$CACHE"
check_eq "$CALLED" "$UNITS" "(cold on the first machine)"
export TIDY_HOST_CPU="apple-m2"
run_tidy -c "$CACHE"
check_eq "$CALLED" "0" "a different host CPU must not invalidate a single entry"
unset TIDY_HOST_CPU

# ---------------------------------------------------------------------------
# 上の壊れ方は CI を緑のまま通り抜ける（結果は正しく、ただ遅いだけ）ので、気付ける
# 手立てを 1 つ持たせてある。規則を変えた実行のように**正当に全件外れる**場面でも
# 出るが、黙って何もしないよりよい。
t "a restored cache that reuses nothing is called out"
run_tidy -c "$CACHE"
check_eq "$CALLED" "0" "(settled)"
check_not_contains "$OUT" "::warning::" "no warning while the cache is doing its job"
printf 'Checks: -*,bugprone-*\n' >"$REPO/.clang-tidy"
run_tidy -c "$CACHE"
check_eq "$CALLED" "$UNITS" "changing the rules invalidates everything"
check_contains "$OUT" "::warning::" "and that is said out loud, not swallowed"

# ---------------------------------------------------------------------------
t "a unit missing from the compile database is always analysed"
printf 'int gamma() { return 5; }\n' >"$REPO/src/draw/Gamma.cpp"
run_tidy -c "$CACHE"
check_contains "$(cat "$CALLS")" "src/draw/Gamma.cpp" "the unlisted unit is analysed"
run_tidy -c "$CACHE"
check_contains "$(cat "$CALLS")" "src/draw/Gamma.cpp" "and stays analysed"
rm -f "$REPO/src/draw/Gamma.cpp"

# ---------------------------------------------------------------------------
# The cache is a speed-up, never a dependency: without -c (and, in CI, without a
# usable python3) the runner must behave exactly as it did before it existed.
t "without -c the runner behaves as before"
run_tidy
check_eq "$RC" "0" "it still succeeds"
check_eq "$CALLED" "$UNITS" "and analyses everything"
check_not_contains "$OUT" "result cache" "with no cache reporting at all"

# ---------------------------------------------------------------------------
t "the key is stable and sensitive on its own"
key_of() { # source [extra-args...]
	local src="$1"
	shift
	"$PYTHON" "$REPO/scripts/tidy-cache-key.py" -p "$REPO" --sdk-key sdk-v1 \
		--tidy-version "stub 18" "$@" "$REPO/$src"
}
k1="$(key_of src/draw/Alpha.cpp)"
k2="$(key_of src/draw/Alpha.cpp)"
check_eq "$k1" "$k2" "the same inputs give the same key"
check_ne "$k1" "$(key_of src/draw/Beta.cpp)" "different units give different keys"
check_ne "$k1" "$(key_of src/draw/Alpha.cpp '--extra=--extra-arg=-fsomething')" \
	"extra clang-tidy arguments are part of the key"

# ---------------------------------------------------------------------------
if [ "$TESTS_FAILED" -ne 0 ]; then
	printf '\ntidy-cache.test.sh: %d/%d checks FAILED\n' "$TESTS_FAILED" "$TESTS_RUN"
	exit 1
fi
printf '\ntidy-cache.test.sh: all %d checks passed\n' "$TESTS_RUN"
exit 0
