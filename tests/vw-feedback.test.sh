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
#	**bash 3.2 で動くこと**も検査する（下の「配列を使っていない」）。このハーネスが走る
#	のは Linux の bash 5 だが、**本番で走るのは macOS の /bin/bash 3.2** で、そちらだけ
#	`set -u` + 空配列の展開で即死する。round 1 でまさにそれが起き（トークン未登録のときだけ
#	`find-pr` が黙って落ちた）、振る舞いのテストでは捕まえられなかった。だから
#	「そもそも配列を書かない」を構文の側で押さえる。
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

# **トークンが無くても引けなければならない。** 登録の前に PR 番号を埋めるのがこの口の
# 目的で、公開リポジトリの open な PR を見るだけなので認証は要らない（round 1 では
# ここが死に、番号を手入力させてしまった）。
rm -f "$KEYCHAIN"
GH_STUB=""
CURL_BODY='[{"number":123,"title":"M23: feedback"}]'
out="$(mode_find_pr "min-nano/vectorworks-plugin-import-ifc-homeskz" "claude/feedback")"
check "find-pr: works with no token at all" "$out" "pr=123
title=M23: feedback
ok"
check_not_contains "find-pr: sends no empty Authorization header" \
	"$(cat "$CURL_URL_FILE")" "Bearer"
printf '%s' "keychain-secret-value" > "$KEYCHAIN"

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
# 本文は診断ログを含むので、引用符もバックスラッシュも日本語も通らなければならない。
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

CURL_BODY='{"html_url":"https://github.com/o/r/pull/123#issuecomment-2","created_at":"2026-09-07T01:02:03Z"}'
out="$(mode_post "o/r" "123" "$BODY_FILE")"
check "post: reports when the comment was created (the next loop-control's since)" "$out" \
	"url=https://github.com/o/r/pull/123#issuecomment-2
created=2026-09-07T01:02:03Z
ok"

# ---------------------------------------------------------------------------
# loop-control — 往復を続けてよいか（M24）。PR の状態と「止めろ」の合図。
#
# curl の応答を **URL で出し分ける**: /pulls/<n> には PR の JSON、/issues/<n>/comments には
# コメントの配列。stub は 1 つの CURL_BODY しか返せないので、ここだけ URL を見る版に替える。
# ---------------------------------------------------------------------------
PULL_BODY='{"state":"open","merged":false}'
COMMENTS_BODY='[]'
COMMENTS_URL_FILE="$WORK/comments-url.txt"
curl() {
	local out="" arg url=""
	while [ "$#" -gt 0 ]; do
		arg="$1"
		case "$arg" in
			-o) out="${2:-}"; shift ;;
			https://*) url="$arg" ;;
		esac
		shift
	done
	[ "$CURL_FAIL" -eq 1 ] && return 1
	case "$url" in
		*/issues/*/comments*)
			printf '%s' "$url" > "$COMMENTS_URL_FILE"
			[ -n "$out" ] && printf '%s' "$COMMENTS_BODY" > "$out"
			;;
		*/pulls/*)
			[ -n "$out" ] && printf '%s' "$PULL_BODY" > "$out"
			;;
	esac
	return 0
}

check "loop-control: open, no signal" "$(mode_loop_control "o/r" "123" "")" "state=open
control=none
ok"
check_contains "loop-control: without since it reads the recent comments" \
	"$(cat "$COMMENTS_URL_FILE")" "/repos/o/r/issues/123/comments?per_page=100&page=1"

COMMENTS_BODY='[{"id":1,"body":"<!-- homeskz-ifc-feedback v1 round=3 build=abc1234 branch=x -->\n## round 3"},{"id":2,"body":"直しました。\n\n<!-- homeskz-ifc-feedback v1 control=stop -->\n往復はもう要りません。"}]'
out="$(mode_loop_control "o/r" "123" "2026-09-07T01:02:03Z")"
check "loop-control: Claude's stop signal is found" "$out" "state=open
control=stop
ok"
check_contains "loop-control: since narrows the comments to those after our own post" \
	"$(cat "$COMMENTS_URL_FILE")" "&since=2026-09-07T01:02:03Z"

# **プラグイン自身の「終えました」（control=ended）を「止めろ」と読まない。** また、
# 目印の無いコメントに control=stop と書いてあっても合図ではない。
COMMENTS_BODY='[{"id":3,"body":"<!-- homeskz-ifc-feedback v1 control=ended reason=user -->\n往復を終えました。"},{"id":4,"body":"control=stop と書いただけ"}]'
check "loop-control: ended / unmarked comments are not a signal" \
	"$(mode_loop_control "o/r" "123" "")" "state=open
control=none
ok"

PULL_BODY='{"state":"closed","merged":true}'
COMMENTS_BODY='[]'
check "loop-control: a merged PR reads as merged" "$(mode_loop_control "o/r" "123" "")" \
	"state=merged
control=none
ok"
PULL_BODY='{"state":"closed","merged":false}'
check "loop-control: a closed PR reads as closed" "$(mode_loop_control "o/r" "123" "")" \
	"state=closed
control=none
ok"

check "loop-control: a missing PR number is refused" "$(mode_loop_control "o/r" "" "")" \
	"error=PR 番号が指定されていません。"
CURL_FAIL=1
check "loop-control: a network failure is reported, not fatal" \
	"$(mode_loop_control "o/r" "123" "")" \
	"error=PR の状態を取得できませんでした（ネットワークか権限）。"
CURL_FAIL=0

check "control_of_comment: stop at the end of the marker" \
	"$(control_of_comment "<!-- homeskz-ifc-feedback v1 control=stop -->")" "stop"
check "control_of_comment: ended is not stop" \
	"$(control_of_comment "<!-- homeskz-ifc-feedback v1 control=ended reason=user -->")" "none"
check "control_of_comment: the signal may share the comment with human text" \
	"$(control_of_comment "直りました。もう往復は要りません。

<!-- homeskz-ifc-feedback v1 control=stop -->")" "stop"
check "control_of_comment: leading whitespace on the signal line is allowed" \
	"$(control_of_comment "  <!-- homeskz-ifc-feedback v1 control=stop -->  ")" "stop"

# ---------------------------------------------------------------------------
# **「書いてある」と「合図である」は違う**（実機 round 1 の回帰。docs/DEV-NOTES.md M24）。
#
# 本文のどこかで `control=stop` を拾う作りだったころ、**プラグイン自身の投稿**が末尾で
# 「合図はこう書きます」と案内しているせいで、その 1 通目を読んだ時点で往復が止まった。
# 説明のために引用された目印を合図と読まないこと——ここが崩れると、この仕組みは
# 「始めた瞬間に自分で止まる」に戻る。
# ---------------------------------------------------------------------------
check "control_of_comment: quoted inside a sentence is not a signal" \
	"$(control_of_comment "往復がもう要らなくなったら、\`<!-- homeskz-ifc-feedback v1 control=stop -->\` を含むコメントを投稿してください。")" \
	"none"
check "control_of_comment: quoted inside a fenced block is not a signal" \
	"$(control_of_comment "合図の書き方:

\`\`\`
<!-- homeskz-ifc-feedback v1 control=stop -->
\`\`\`

以上です。")" "none"

# プラグイン自身の周の投稿（1 行目が round= の目印で、末尾に上の案内文を含む）。
PLUGIN_ROUND_BODY="<!-- homeskz-ifc-feedback v1 round=1 build=a618af6 branch=x -->
## 実機フィードバック round 1

往復のパレットが開いていれば、push のあとは何もしなくても次の周が自動で走ります。
往復がもう要らなくなったら、\`<!-- homeskz-ifc-feedback v1 control=stop -->\` を含む
コメントをこの PR へ投稿してください——次の確認でパレットが止まります。"
check "control_of_comment: the plug-in's own round post never stops the loop" \
	"$(control_of_comment "$PLUGIN_ROUND_BODY")" "none"

# 同じ本文を loop-control の経路でも通す（since は自分の投稿を含んで返りうる）。
PULL_BODY='{"state":"open","merged":false}'
COMMENTS_BODY="$(python3 - <<'JSON'
import json
body = (
    "<!-- homeskz-ifc-feedback v1 round=1 build=a618af6 branch=x -->\n"
    "## 実機フィードバック round 1\n\n"
    "往復がもう要らなくなったら、`<!-- homeskz-ifc-feedback v1 control=stop -->` を含む\n"
    "コメントをこの PR へ投稿してください。"
)
print(json.dumps([{"id": 9, "body": body}], ensure_ascii=False))
JSON
)"
check "loop-control: the round we just posted is not read as our own stop signal" \
	"$(mode_loop_control "o/r" "123" "2026-09-08T00:42:20Z")" "state=open
control=none
ok"
COMMENTS_BODY='[]'

# ---------------------------------------------------------------------------
# bash 3.2（macOS の /bin/bash）で動くこと。
#
# **配列を 1 つも書いていないこと**を構文の側で押さえる。`set -u` の下で空の配列を
# 展開すると bash 3.2 は "unbound variable" で即死するが、このハーネスが走る bash 5 では
# 起きない——だから「振る舞いを試す」では守れず、**書かないことを検査する**しかない
# （round 1 の実機で、トークン未登録のときだけ find-pr が黙って落ちた原因がこれ）。
# ---------------------------------------------------------------------------
CHECKS=$((CHECKS + 1))
if grep -nE '\$\{[A-Za-z_][A-Za-z0-9_]*\[@\]\}' "$SCRIPT" >/dev/null 2>&1; then
	echo "[ FAIL ] the script must not expand arrays (macOS bash 3.2 dies on an empty one)"
	grep -nE '\$\{[A-Za-z_][A-Za-z0-9_]*\[@\]\}' "$SCRIPT" | sed 's/^/         /'
	FAILURES=$((FAILURES + 1))
else
	echo "[ PASS ] the script expands no arrays (safe on macOS bash 3.2)"
fi

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
