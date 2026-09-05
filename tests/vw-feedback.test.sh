#!/usr/bin/env bash
#
#	vw-feedback.test.sh
#
#	Unit tests for the macOS feedback back end (scripts/vw-feedback.sh) — the
#	script the plug-in drives to post its run report to the pull request
#	(docs/DEV-NOTES.md M23).
#
#	The script is SOURCED (its `main` is guarded, see the tail of vw-feedback.sh),
#	so the real functions run in-process and only their outermost I/O leaves are
#	replaced:
#
#	  * curl      the network boundary. The stub understands the two flag shapes
#	              the script uses (-o <file> … and -o/-w for the POST), writes a
#	              canned response body and echoes a canned status code — so the
#	              REAL payload building, argument order and response handling run.
#	  * plutil    the JSON reader (macOS-only), emulated with python3 exactly as
#	              tests/vw-update.test.sh does, so this runs on a Linux runner.
#	  * security  the keychain (macOS-only), emulated with a scratch file so the
#	              REAL login / logout / lookup-order logic runs.
#	  * gh_path   the gh CLI probe; off by default so the lookup order is
#	              deterministic, turned on in the one case that tests it.
#
#	**トークンが出力へ漏れないこと**もここで確かめる——漏れたら PR コメントや診断ログに
#	載りうるので、これは単なる整形の話ではない。
#
#	This is a unit test, not end-to-end (that would post a real comment).
#

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/../scripts/vw-feedback.sh"

# ---------------------------------------------------------------------------
# Missing-tool policy — identical to tests/vw-update.test.sh: a developer box
# skips gracefully, CI (VW_REQUIRE_SCRIPT_TESTS) turns a missing tool into a
# hard failure so the suite can never "pass" without running.
# ---------------------------------------------------------------------------
REQUIRE_TOOLS="${VW_REQUIRE_SCRIPT_TESTS:-}"
case "$REQUIRE_TOOLS" in
	'' | 0 | off | OFF | false | FALSE | no | NO) REQUIRE_TOOLS="" ;;
esac

skip_or_fail() {
	if [ -n "$REQUIRE_TOOLS" ]; then
		echo "ERROR vw-feedback.test.sh: $1 (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)." >&2
		exit 1
	fi
	echo "SKIP vw-feedback.test.sh: $1."
	exit 0
}

[ -f "$SCRIPT" ] || skip_or_fail "$SCRIPT not found"
command -v python3 >/dev/null 2>&1 || skip_or_fail "python3 is required to emulate plutil"
command -v awk >/dev/null 2>&1 || skip_or_fail "awk is required by the script under test"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Tiny harness (same shape as the other script tests).
# ---------------------------------------------------------------------------
FAILURES=0
CHECKS=0

check() { # description, actual, expected
	CHECKS=$((CHECKS + 1))
	if [ "$2" = "$3" ]; then
		echo "[ PASS ] $1"
	else
		echo "[ FAIL ] $1"
		echo "         expected: $3"
		echo "         actual:   $2"
		FAILURES=$((FAILURES + 1))
	fi
}

check_contains() { # description, haystack, needle
	CHECKS=$((CHECKS + 1))
	case "$2" in
		*"$3"*) echo "[ PASS ] $1" ;;
		*)
			echo "[ FAIL ] $1"
			echo "         expected to contain: $3"
			echo "         actual:              $2"
			FAILURES=$((FAILURES + 1))
			;;
	esac
}

check_not_contains() { # description, haystack, needle
	CHECKS=$((CHECKS + 1))
	case "$2" in
		*"$3"*)
			echo "[ FAIL ] $1"
			echo "         must NOT contain: $3"
			echo "         actual:           $2"
			FAILURES=$((FAILURES + 1))
			;;
		*) echo "[ PASS ] $1" ;;
	esac
}

# ---------------------------------------------------------------------------
# Environment: no real token, no real keychain, no gh.
# ---------------------------------------------------------------------------
unset HOMESKZ_IFC_FEEDBACK_TOKEN
export VW_REPO="min-nano/vectorworks-plugin-import-ifc-homeskz"

# shellcheck source=../scripts/vw-feedback.sh
. "$SCRIPT"

# plutil emulation (JSON read), mirroring tests/vw-update.test.sh.
jval() { # json-file, keypath -> raw scalar
	python3 - "$1" "$2" <<'PY' 2>/dev/null || true
import json, sys
try:
	with open(sys.argv[1], encoding="utf-8") as handle:
		data = json.load(handle)
except Exception:
	sys.exit(0)
for part in sys.argv[2].split("."):
	if isinstance(data, list):
		try:
			data = data[int(part)]
		except (ValueError, IndexError):
			sys.exit(0)
	elif isinstance(data, dict):
		if part not in data:
			sys.exit(0)
		data = data[part]
	else:
		sys.exit(0)
if isinstance(data, (dict, list)):
	sys.exit(0)
print(data)
PY
}

# Keychain emulation. The scratch file stands in for the login keychain so the
# real add/find/delete ordering in the script is exercised.
KEYCHAIN="$WORK/keychain"
security() {
	case "${1:-}" in
		find-generic-password)
			[ -f "$KEYCHAIN" ] || return 1
			cat "$KEYCHAIN"
			;;
		add-generic-password)
			# … -w <token> の最後の引数が値（script の呼び方に合わせる）。
			local value=""
			while [ "$#" -gt 0 ]; do
				[ "$1" = "-w" ] && { value="${2:-}"; break; }
				shift
			done
			printf '%s' "$value" > "$KEYCHAIN"
			;;
		delete-generic-password) rm -f "$KEYCHAIN" ;;
		*) return 1 ;;
	esac
}

# gh CLI: absent unless a case turns it on.
GH_STUB=""
gh_path() {
	[ -n "$GH_STUB" ] || return 1
	printf '%s' "$GH_STUB"
}

# curl stub. Writes CURL_BODY into the -o file and echoes CURL_CODE when -w is
# used; captures the --data-binary payload so the built JSON can be inspected.
CURL_BODY=""
CURL_CODE="201"
CURL_FAIL=0
CURL_URL_FILE="$WORK/url.txt"
CURL_PAYLOAD="$WORK/payload.json"
curl() {
	# 呼ばれるのは $(...) の中（＝サブシェル）なので、覗きたい値は変数ではなく
	# **ファイルへ**残す。変数に入れても呼び出し側からは見えない。
	local out="" wants_code=0 arg
	rm -f "$CURL_PAYLOAD" "$CURL_URL_FILE"
	while [ "$#" -gt 0 ]; do
		arg="$1"
		case "$arg" in
			-o) out="${2:-}"; shift ;;
			-w) wants_code=1; shift ;;
			--data-binary) cp "${2#@}" "$CURL_PAYLOAD" 2>/dev/null || true; shift ;;
			https://*) printf '%s' "$arg" > "$CURL_URL_FILE" ;;
		esac
		shift
	done
	if [ "$CURL_FAIL" -eq 1 ]; then
		[ "$wants_code" -eq 1 ] && printf '000'
		return 1
	fi
	[ -n "$out" ] && printf '%s' "$CURL_BODY" > "$out"
	[ "$wants_code" -eq 1 ] && printf '%s' "$CURL_CODE"
	return 0
}

# ---------------------------------------------------------------------------
# token-status — the lookup order, and that the token never appears in output.
# ---------------------------------------------------------------------------
rm -f "$KEYCHAIN"
out="$(mode_token_status)"
check "token-status: nothing configured" "$out" "source=none
ok=no"

printf '%s' "keychain-secret-value" > "$KEYCHAIN"
out="$(mode_token_status)"
check "token-status: keychain wins when no env var" "$out" "source=keychain
ok=yes"
check_not_contains "token-status never prints the token" "$out" "keychain-secret-value"

HOMESKZ_IFC_FEEDBACK_TOKEN="env-secret-value"
export HOMESKZ_IFC_FEEDBACK_TOKEN
out="$(mode_token_status)"
check "token-status: the env var takes priority" "$out" "source=env
ok=yes"
check_not_contains "token-status never prints the env token" "$out" "env-secret-value"
check "resolve_token returns the env token" "$(resolve_token)" "env-secret-value"
unset HOMESKZ_IFC_FEEDBACK_TOKEN

rm -f "$KEYCHAIN"
GH_STUB="$WORK/gh"
cat > "$GH_STUB" <<'GH'
#!/usr/bin/env bash
[ "$1" = "auth" ] && [ "$2" = "token" ] && echo "gh-secret-value"
GH
chmod +x "$GH_STUB"
out="$(mode_token_status)"
check "token-status: falls back to the gh CLI" "$out" "source=gh
ok=yes"
GH_STUB=""

# ---------------------------------------------------------------------------
# login / logout
# ---------------------------------------------------------------------------
rm -f "$KEYCHAIN"
TOKEN_FILE="$WORK/token.txt"
printf 'ghp_exampletoken\n' > "$TOKEN_FILE"
out="$(mode_login "$TOKEN_FILE")"
check "login: stores the token" "$out" "ok"
check "login: the token reached the store" "$(cat "$KEYCHAIN")" "ghp_exampletoken"
CHECKS=$((CHECKS + 1))
if [ -f "$TOKEN_FILE" ]; then
	echo "[ FAIL ] login: the hand-off file must be deleted"
	FAILURES=$((FAILURES + 1))
else
	echo "[ PASS ] login: the hand-off file is deleted"
fi

printf '' > "$WORK/empty.txt"
check "login: an empty token is refused" "$(mode_login "$WORK/empty.txt")" \
	"error=トークンが空です。"
check "login: a missing file is refused" "$(mode_login "$WORK/nope.txt")" \
	"error=トークンのファイルが見つかりません。"

check "logout: succeeds" "$(mode_logout)" "ok"
check "logout: the store is gone" "$(mode_token_status)" "source=none
ok=no"

# ---------------------------------------------------------------------------
# find-pr — resolving the PR from the branch, so nobody types a number.
# ---------------------------------------------------------------------------
printf '%s' "keychain-secret-value" > "$KEYCHAIN"
CURL_BODY='[{"number":123,"title":"M23: feedback"}]'
CURL_CODE="200"
out="$(mode_find_pr "min-nano/vectorworks-plugin-import-ifc-homeskz" "claude/feedback")"
check "find-pr: reports the open PR" "$out" "pr=123
title=M23: feedback
ok"
check_contains "find-pr: queries head=<owner>:<branch>" "$(cat "$CURL_URL_FILE")" \
	"head=min-nano:claude/feedback"

CURL_BODY='[]'
out="$(mode_find_pr "min-nano/vectorworks-plugin-import-ifc-homeskz" "no-such-branch")"
check "find-pr: no open PR" "$out" "error=ブランチ no-such-branch に open な PR がありません。"

check "find-pr: a missing branch is refused" "$(mode_find_pr "o/r" "")" \
	"error=ブランチが指定されていません。"

CURL_FAIL=1
out="$(mode_find_pr "o/r" "b")"
CURL_FAIL=0
check "find-pr: a network failure is reported, not fatal" "$out" \
	"error=PR を検索できませんでした（ネットワークか権限）。"

# ---------------------------------------------------------------------------
# post — the payload, the success line and the failure wording.
# ---------------------------------------------------------------------------
BODY_FILE="$WORK/body.md"
cat > "$BODY_FILE" <<'BODY'
## 実機フィードバック "round 1"
バックスラッシュ \ と "引用符"
BODY

CURL_BODY='{"html_url":"https://github.com/o/r/pull/123#issuecomment-1"}'
CURL_CODE="201"
out="$(mode_post "o/r" "123" "$BODY_FILE")"
check "post: reports the comment URL" "$out" "url=https://github.com/o/r/pull/123#issuecomment-1
ok"
check_contains "post: targets the issue-comments endpoint" "$(cat "$CURL_URL_FILE")" \
	"https://api.github.com/repos/o/r/issues/123/comments"

# The built payload must be valid JSON that round-trips the body exactly —
# 本文は診断ログと利用者の所見なので、引用符もバックスラッシュも日本語も通る。
decoded="$(python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    print(json.load(handle)["body"], end="")
' "$CURL_PAYLOAD")"
check "post: the payload round-trips the body" "$decoded" "$(cat "$BODY_FILE")"

CURL_CODE="403"
CURL_BODY='{"message":"Resource not accessible by integration"}'
out="$(mode_post "o/r" "123" "$BODY_FILE")"
check "post: relays GitHub's own reason" "$out" \
	"error=コメントを投稿できませんでした（Resource not accessible by integration）。"

CURL_CODE="201"
rm -f "$KEYCHAIN"
out="$(mode_post "o/r" "123" "$BODY_FILE")"
check "post: without a token" "$out" \
	"error=GitHub のトークンがありません（先に login してください）。"

printf '%s' "keychain-secret-value" > "$KEYCHAIN"
check "post: a missing body file is refused" "$(mode_post "o/r" "123" "$WORK/none.md")" \
	"error=引数が不足しています。"

# ---------------------------------------------------------------------------
# JSON escaping in isolation (the one piece of hand-written encoding).
# ---------------------------------------------------------------------------
printf 'a"b\\c\td\n' > "$WORK/esc.txt"
check "json_string_from_file escapes quote, backslash and tab" \
	"$(json_string_from_file "$WORK/esc.txt")" '"a\"b\\c\td\n"'

# ---------------------------------------------------------------------------
echo
if [ "$FAILURES" -eq 0 ]; then
	echo "$CHECKS check(s) passed."
	exit 0
fi
echo "$FAILURES of $CHECKS check(s) FAILED."
exit 1
