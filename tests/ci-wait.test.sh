#!/usr/bin/env bash
#
#	ci-wait.test.sh
#
#	Unit tests for the CI-wait clients: the shared waiting core
#	(scripts/ci-common.sh) and the PR/branch/commit waiter built on it
#	(scripts/ci-wait.sh).
#
#	What these tests are actually guarding
#	-------------------------------------
#	These scripts exist so a remote session can learn that CI finished: the
#	process exit IS the notification. So the property under test is not
#	"does it report the right conclusion" alone — it is
#
#	    **the wait always terminates, and never reports success by accident.**
#
#	Both halves have already broken in production:
#	  * a poll loop with no HTTP time limit hung forever, so the session never
#	    noticed CI had finished (fixed in #35);
#	  * a hand-written wait treated "no checks registered yet" and "old run
#	    cancelled by a newer push" as a finished, healthy CI.
#
#	Every case below maps to one of those two failure modes.
#
#	How they run
#	------------
#	  * The core (poll_until, the watchdog, api()) is SOURCED and driven with
#	    fake probes — no network at all.
#	  * ci-wait.sh is run as a real subprocess with a FAKE `curl` first on PATH
#	    (tests/bin/curl, generated below), so the real argument parsing, probe
#	    logic and reporting run against canned GitHub responses. The fake is the
#	    outermost I/O leaf, the same idea as the stubbed api_get/download in
#	    tests/vw-update.test.sh.
#
#	Scenarios are directories of fixtures. A response is picked per URL, and a
#	per-URL call counter allows a scenario to answer differently over successive
#	polls (that is how "checks appear late" and "head moves" are expressed).
#

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMON="${HERE}/../scripts/ci-common.sh"
CI_WAIT="${HERE}/../scripts/ci-wait.sh"

# ---------------------------------------------------------------------------
# Missing-tool policy — identical to tests/vw-update.test.sh: skip locally,
# hard-fail in CI (VW_REQUIRE_SCRIPT_TESTS), so a silent skip can never let the
# suite "pass" without running a single check.
# ---------------------------------------------------------------------------
REQUIRE_TOOLS="${VW_REQUIRE_SCRIPT_TESTS:-}"
case "$REQUIRE_TOOLS" in
	'' | 0 | off | OFF | false | FALSE | no | NO) REQUIRE_TOOLS="" ;;
esac

skip_or_fail() {
	if [ -n "$REQUIRE_TOOLS" ]; then
		echo "ERROR ci-wait.test.sh: $1 (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)." >&2
		exit 1
	fi
	echo "SKIP ci-wait.test.sh: $1."
	exit 0
}

for tool in jq pgrep timeout; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		skip_or_fail "'$tool' not found (the CI-wait clients need it)"
	fi
done
for f in "$COMMON" "$CI_WAIT"; do
	[ -f "$f" ] || skip_or_fail "$f not found"
done

# ---------------------------------------------------------------------------
# Tiny assertion harness, styled after tests/TestFramework.h.
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

check_le() { # actual limit [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	if [ "$1" -gt "$2" ]; then
		TESTS_FAILED=$((TESTS_FAILED + 1))
		printf 'FAIL [%s] %s\n  limit:  %s\n  actual: %s\n' \
			"$CURRENT" "${3:-value above limit}" "$2" "$1"
	fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Everything that waits is run under `timeout`, and the whole suite has this
# ceiling on top. A regression in the very machinery under test (a deadline that
# stops expiring) would otherwise hang the SUITE — which is the same failure mode
# these tests exist to catch, just moved into CI. Bound it, always.
SUITE_LIMIT="${CI_TEST_SUITE_LIMIT:-600}"
CASE_LIMIT="${CI_TEST_CASE_LIMIT:-60}"

self="$$"
(
	exec >/dev/null 2>&1
	sleep "$SUITE_LIMIT"
	kill -9 "$self"
) &
SUITE_GUARD=$!
disown "$SUITE_GUARD" 2>/dev/null || true

cleanup_suite() {
	[ -n "${SUITE_GUARD:-}" ] && kill "$SUITE_GUARD" 2>/dev/null
	rm -rf "$WORK"
	return 0
}
trap cleanup_suite EXIT

# ===========================================================================
# Part 1 — the waiting core (scripts/ci-common.sh), driven with fake probes.
# ===========================================================================
#
# Sourced in a subshell per case would lose the counters, so source once here
# and keep every case's state in globals the probes own.

CI_TOOL="ci-test"
GITHUB_TOKEN="${GITHUB_TOKEN:-x}"
# shellcheck source=/dev/null
source "$COMMON"
# The library installs EXIT/TERM traps of its own; keep ours on top of them.
trap 'ci_cleanup; cleanup_suite' EXIT

PROBE_CALLS=0

probe_done_now() {
	PROBE_CALLS=$((PROBE_CALLS + 1))
	POLL_STATUS="status=done"
	return 0
}

probe_never() {
	PROBE_CALLS=$((PROBE_CALLS + 1))
	POLL_STATUS="status=running"
	return 1
}

probe_transient() {
	PROBE_CALLS=$((PROBE_CALLS + 1))
	return 3
}

probe_fatal() {
	PROBE_CALLS=$((PROBE_CALLS + 1))
	return 2
}

probe_done_third() {
	PROBE_CALLS=$((PROBE_CALLS + 1))
	POLL_STATUS="status=running"
	[ "$PROBE_CALLS" -ge 3 ] && return 0
	return 1
}

t "poll_until: completes as soon as the probe says so"
POLL=1
TIMEOUT=30
PROBE_CALLS=0
poll_until probe_done_now 2>/dev/null
check_eq "$?" "0" "an immediately-complete probe returns 0"
check_eq "$PROBE_CALLS" "1" "and is not polled again"

t "poll_until: polls until the probe completes"
PROBE_CALLS=0
poll_until probe_done_third 2>/dev/null
check_eq "$?" "0" "returns 0 once the probe completes"
check_eq "$PROBE_CALLS" "3" "after exactly three polls"

t "poll_until: a never-completing probe still returns (deadline)"
# THE regression guard: without the deadline this loop is the hang that made a
# session miss CI completion. Run it as a bounded subprocess — if the deadline
# ever stops working, this case must FAIL, not hang the suite.
never="$WORK/never.sh"
cat >"$never" <<EOF
#!/usr/bin/env bash
set -uo pipefail
CI_TOOL="ci-test"
GITHUB_TOKEN=x
source "$COMMON"
POLL=1
TIMEOUT=3
probe_never() { POLL_STATUS="status=running"; return 1; }
poll_until probe_never 2>/dev/null
printf 'rc=%s\n' "\$?"
EOF
chmod +x "$never"
started="$(date +%s)"
out="$(timeout 30 "$never")"
elapsed=$(($(date +%s) - started))
check_eq "$out" "rc=1" "the deadline reports 'not seen' (1), not success"
check_le "$elapsed" "20" "and returns near the deadline instead of hanging"

t "poll_until: gives up after MAX_API_ERRORS consecutive transient failures"
POLL=1
TIMEOUT=60
MAX_API_ERRORS=3
PROBE_CALLS=0
poll_until probe_transient 2>/dev/null
check_eq "$?" "2" "a persistently failing API aborts the wait"
check_eq "$PROBE_CALLS" "3" "after MAX_API_ERRORS attempts"
MAX_API_ERRORS=10

t "poll_until: a fatal probe result aborts immediately"
PROBE_CALLS=0
poll_until probe_fatal 2>/dev/null
check_eq "$?" "2" "fatal aborts"
check_eq "$PROBE_CALLS" "1" "without further polling"

t "api(): every HTTP call carries connect and transfer time limits"
# White-box on purpose: an unbounded curl inside the poll loop is exactly how
# the wait hung before, and no black-box assertion catches it cheaply.
api_body="$(declare -f api)"
check_contains "$api_body" "--connect-timeout" "connect timeout is passed"
check_contains "$api_body" "--max-time" "transfer timeout is passed"

t "watchdog: kills a wait that hangs outside the poll loop"
# The last line of defence. A subshell that ignores the deadline entirely must
# still die, or the caller never gets its completion notification.
hang="$WORK/hang.sh"
cat >"$hang" <<EOF
#!/usr/bin/env bash
set -uo pipefail
CI_TOOL="ci-test"
GITHUB_TOKEN=x
source "$COMMON"
start_watchdog 2
sleep 120
EOF
chmod +x "$hang"
started="$(date +%s)"
"$hang" >/dev/null 2>&1
elapsed=$(($(date +%s) - started))
check_le "$elapsed" "30" "the watchdog terminates a hung wait"

t "require_positive_int: rejects a non-numeric deadline"
# A deadline that is not a number makes every comparison false — the loop then
# never expires. Fail loudly at startup instead.
( require_positive_int "abc" "--timeout" ) >/dev/null 2>&1
check_eq "$?" "2" "non-numeric is a usage error"
( require_positive_int "0" "--poll" ) >/dev/null 2>&1
check_eq "$?" "2" "zero is a usage error"

# ===========================================================================
# Part 2 — ci-wait.sh end to end, against a fake GitHub.
# ===========================================================================

FAKE_BIN="$WORK/bin"
mkdir -p "$FAKE_BIN"

# The fake curl. It understands the flags api() actually passes, derives a key
# from the URL path, and answers from the scenario directory ($CI_TEST_SCENARIO):
#
#   <key>            response body            (e.g. commits_abc_check-runs)
#   <key>@<n>        body for the n-th call of that URL (1-based; falls back to
#                    <key>, so a scenario only lists the polls that differ)
#   <key>.code[@<n>] HTTP status (default 200)
#
cat >"$FAKE_BIN/curl" <<'FAKE'
#!/usr/bin/env bash
set -uo pipefail
out=""
want_code=""
url=""
while [ "$#" -gt 0 ]; do
	case "$1" in
		-o) out="$2" ; shift 2 ;;
		-w) want_code=1 ; shift 2 ;;
		-H | --connect-timeout | --max-time | --retry | --retry-delay | -X | -d) shift 2 ;;
		-sS | -s | -S | --retry-connrefused) shift ;;
		-*) shift ;;
		*) url="$1" ; shift ;;
	esac
done

scen="${CI_TEST_SCENARIO:?fake curl: CI_TEST_SCENARIO is unset}"
path="${url#*/repos/*/*/}"
path="${path%%\?*}"
key="$(printf '%s' "$path" | tr -c 'A-Za-z0-9._-' '_')"

n_file="$scen/.n.$key"
n=$(($(cat "$n_file" 2>/dev/null || echo 0) + 1))
printf '%s' "$n" >"$n_file"

pick() { # base-name -> path of the file to use, or empty
	if [ -f "$scen/$1@$n" ]; then printf '%s' "$scen/$1@$n"
	elif [ -f "$scen/$1" ]; then printf '%s' "$scen/$1"
	fi
}

body="$(pick "$key")"
code_file="$(pick "$key.code")"
code="200"
[ -n "$code_file" ] && code="$(cat "$code_file")"
[ -n "$body" ] || { body="$scen/.empty" ; printf '{}' >"$body" ; }

if [ -n "$out" ]; then cp "$body" "$out"; else cat "$body"; fi
[ -n "$want_code" ] && printf '%s' "$code"
exit 0
FAKE
chmod +x "$FAKE_BIN/curl"

# scenario <name>: make a fresh scenario directory and echo its path.
scenario() {
	local d="$WORK/scen-$1"
	rm -rf "$d"
	mkdir -p "$d"
	printf '%s' "$d"
}

# checks_json <status> <conclusion>...: build a check-runs response from pairs.
checks_json() {
	local out="[]" name=1
	while [ "$#" -gt 0 ]; do
		out="$(jq -c --arg n "check-$name" --arg s "$1" --arg c "$2" \
			'. + [{name: $n, status: $s, conclusion: (if $c == "" then null else $c end),
			       html_url: ("https://example.invalid/" + $n)}]' <<<"$out")"
		name=$((name + 1))
		shift 2
	done
	jq -c --argjson r "$out" -n '{total_count: ($r | length), check_runs: $r}'
}

# run_ci_wait <scenario-dir> <args...>: run the real script against the fake API.
run_ci_wait() {
	local scen="$1"
	shift
	CI_TEST_SCENARIO="$scen" PATH="$FAKE_BIN:$PATH" GITHUB_TOKEN=x \
		timeout "$CASE_LIMIT" "$CI_WAIT" --poll 1 --timeout 20 "$@" 2>&1
}

t "ci-wait: all checks green"
s="$(scenario green)"
echo '{"sha":"aaa111"}' >"$s/commits_main"
checks_json completed success completed success >"$s/commits_aaa111_check-runs"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_aaa111_status"
out="$(run_ci_wait "$s" --ref main)"
rc="$?"
check_eq "$rc" "0" "green CI exits 0"
check_contains "$out" "conclusion=success" "reports success"
check_contains "$out" "ci-wait: done (conclusion=success exit=0)" "prints the final line"

t "ci-wait: a failing check is reported and exits non-zero"
s="$(scenario red)"
echo '{"sha":"bbb222"}' >"$s/commits_main"
checks_json completed success completed failure >"$s/commits_bbb222_check-runs"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_bbb222_status"
out="$(run_ci_wait "$s" --ref main)"
rc="$?"
check_eq "$rc" "1" "a red check exits 1"
check_contains "$out" "conclusion=failure" "reports failure"
check_contains "$out" "https://example.invalid/check-2" "and links the failing check"

t "ci-wait: a cancelled check is NOT success"
# This is the "old run cancelled by a newer push" case that was misread as
# "CI already passed".
s="$(scenario cancelled)"
echo '{"sha":"ccc333"}' >"$s/commits_main"
checks_json completed success completed cancelled >"$s/commits_ccc333_check-runs"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_ccc333_status"
out="$(run_ci_wait "$s" --ref main)"
rc="$?"
check_eq "$rc" "1" "cancelled exits 1"
check_contains "$out" "conclusion=failure" "cancelled counts as a failed CI"
check_not_contains "$out" "conclusion=success" "and is never success"

t "ci-wait: waits while checks are still running"
s="$(scenario running)"
echo '{"sha":"ddd444"}' >"$s/commits_main"
checks_json in_progress "" completed success >"$s/commits_ddd444_check-runs"
checks_json completed success completed success >"$s/commits_ddd444_check-runs@3"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_ddd444_status"
out="$(run_ci_wait "$s" --ref main)"
rc="$?"
check_eq "$rc" "0" "finishes once the last check completes"
check_contains "$out" "checks=1/2 完了" "and reports progress while waiting"

t "ci-wait: no checks yet is not success (grace, then no-checks)"
# The "CI has not even started" trap: an empty check list must never read as a
# green build.
s="$(scenario empty)"
echo '{"sha":"eee555"}' >"$s/commits_main"
echo '{"total_count":0,"check_runs":[]}' >"$s/commits_eee555_check-runs"
out="$(run_ci_wait "$s" --ref main --grace 2)"
rc="$?"
check_eq "$rc" "1" "no checks exits 1"
check_contains "$out" "conclusion=no-checks" "and says so explicitly"

t "ci-wait: checks that appear late are picked up"
s="$(scenario late)"
echo '{"sha":"fff666"}' >"$s/commits_main"
echo '{"total_count":0,"check_runs":[]}' >"$s/commits_fff666_check-runs"
checks_json completed success >"$s/commits_fff666_check-runs@3"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_fff666_status"
out="$(run_ci_wait "$s" --ref main --grace 30)"
rc="$?"
check_eq "$rc" "0" "waits out the registration delay"
check_contains "$out" "conclusion=success" "then reports the real result"

t "ci-wait: pending commit statuses keep the wait open"
s="$(scenario statuses)"
echo '{"sha":"999aaa"}' >"$s/commits_main"
checks_json completed success >"$s/commits_999aaa_check-runs"
echo '{"total_count":1,"state":"pending"}' >"$s/commits_999aaa_status"
echo '{"total_count":1,"state":"success"}' >"$s/commits_999aaa_status@2"
out="$(run_ci_wait "$s" --ref main)"
rc="$?"
check_eq "$rc" "0" "finishes when the status turns green"
check_contains "$out" "commit status=pending" "after reporting the pending status"

t "ci-wait: follows the PR head when a new push lands"
s="$(scenario follow)"
echo '{"head":{"sha":"old111"},"state":"open","merged":false}' >"$s/pulls_7"
echo '{"head":{"sha":"new222"},"state":"open","merged":false}' >"$s/pulls_7@2"
echo '{"head":{"sha":"new222"},"state":"open","merged":false}' >"$s/pulls_7@3"
checks_json completed cancelled >"$s/commits_old111_check-runs"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_old111_status"
checks_json completed success >"$s/commits_new222_check-runs"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_new222_status"
out="$(run_ci_wait "$s" --pr 7)"
rc="$?"
check_eq "$rc" "0" "the new head's green CI is what counts"
check_contains "$out" "head が動きました" "the move is reported"
check_contains "$out" "sha=new222" "and the final result is the new head's"

t "ci-wait: --no-follow reports head-moved instead of a stale result"
s="$(scenario nofollow)"
echo '{"head":{"sha":"old111"},"state":"open","merged":false}' >"$s/pulls_7"
echo '{"head":{"sha":"new222"},"state":"open","merged":false}' >"$s/pulls_7@2"
checks_json completed success >"$s/commits_old111_check-runs"
echo '{"total_count":0,"state":"pending"}' >"$s/commits_old111_status"
out="$(run_ci_wait "$s" --pr 7 --no-follow)"
rc="$?"
check_eq "$rc" "1" "a stale green does not exit 0"
check_contains "$out" "conclusion=head-moved" "the reason is explicit"

t "ci-wait: an unresolvable ref fails fast"
s="$(scenario missing)"
echo '{"message":"Not Found"}' >"$s/commits_nope"
echo '404' >"$s/commits_nope.code"
out="$(run_ci_wait "$s" --ref nope)"
rc="$?"
check_eq "$rc" "2" "a bad target is a usage error, not a silent wait"
check_contains "$out" "解決できません" "with a reason"

t "ci-wait: a persistently failing API ends the wait (api-error)"
s="$(scenario apierr)"
echo '{"sha":"777bbb"}' >"$s/commits_main"
echo '{"message":"boom"}' >"$s/commits_777bbb_check-runs"
echo '500' >"$s/commits_777bbb_check-runs.code"
started="$(date +%s)"
out="$(CI_MAX_API_ERRORS=2 run_ci_wait "$s" --ref main)"
rc="$?"
elapsed=$(($(date +%s) - started))
check_eq "$rc" "1" "an unreachable API is not success"
check_contains "$out" "conclusion=api-error" "and is distinguishable from a red build"
check_le "$elapsed" "30" "and stops instead of spinning"

t "ci-wait: an unauthorised token stops the wait immediately"
s="$(scenario noauth)"
echo '{"sha":"888ccc"}' >"$s/commits_main"
echo '{"message":"Bad credentials"}' >"$s/commits_888ccc_check-runs"
echo '401' >"$s/commits_888ccc_check-runs.code"
out="$(run_ci_wait "$s" --ref main)"
rc="$?"
check_eq "$rc" "1" "no silent spinning on a fatal HTTP status"
check_contains "$out" "conclusion=api-error" "reported as api-error"

t "ci-wait: the deadline ends the wait with a distinguishable conclusion"
s="$(scenario slow)"
echo '{"sha":"666ddd"}' >"$s/commits_main"
checks_json in_progress "" >"$s/commits_666ddd_check-runs"
started="$(date +%s)"
out="$(CI_TEST_SCENARIO="$s" PATH="$FAKE_BIN:$PATH" GITHUB_TOKEN=x \
	timeout "$CASE_LIMIT" "$CI_WAIT" --ref main --poll 1 --timeout 3 2>&1)"
rc="$?"
elapsed=$(($(date +%s) - started))
check_eq "$rc" "1" "an unfinished CI is not success"
check_contains "$out" "conclusion=timed-out-waiting" "distinguishable from a real failure"
check_contains "$out" "ci-wait: done" "and the process still prints its final line"
check_le "$elapsed" "30" "near the deadline"

# ---------------------------------------------------------------------------
if [ "$TESTS_FAILED" -ne 0 ]; then
	printf '\nci-wait.test.sh: %d/%d checks FAILED\n' "$TESTS_FAILED" "$TESTS_RUN"
	exit 1
fi
printf '\nci-wait.test.sh: all %d checks passed\n' "$TESTS_RUN"
exit 0
