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
#	  * spawn_self / ask_note / note_alert / open_url
#	              所見のダイアログまわり。**別プロセスを起こす口（spawn_self）だけを
#	              差し替える**ので、「すぐ返る」という肝の性質と、その子が実際にやること
#	              （尋ねる → 所見を 1 通として投稿する → ブラウザで開く）の両方を、
#	              本物のコードで確かめられる。
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
# 所見のダイアログのボタンは**対象を名乗る**（実機の指摘。docs/DEV-NOTES.md M23）。
#
# ただの「送る／送らない」だと、結果のダイアログの同名のボタンと区別が付かず、しかも
# 往復が終わるのかどうかも読み取れない。ダイアログは osascript が出すので振る舞いでは
# 試せない——**そう書いてあることを検査する**しかない。
# ---------------------------------------------------------------------------
CHECKS=$((CHECKS + 1))
if grep -q 'buttons {"所見を書かない", "所見を送る"}' "$SCRIPT"; then
	echo "[ PASS ] the note dialog names what its buttons send"
else
	echo "[ FAIL ] the note dialog buttons must name their subject (所見を送る / 所見を書かない)"
	FAILURES=$((FAILURES + 1))
fi

# ---------------------------------------------------------------------------
# ask-note — 所見を別プロセスで訊く（M23）。
#
# **肝は「すぐ返る」こと。** ここで待つと Vectorworks のメインスレッドが止まり、
# 所見を書くために絵を見たい、という当の目的が果たせない。
# ---------------------------------------------------------------------------
SPAWNED_FILE="$WORK/spawned.txt"
spawn_self() { # args...
	printf '%s\n' "$*" > "$SPAWNED_FILE"
}

rm -f "$SPAWNED_FILE"
check "ask-note: returns immediately with ok" \
	"$(mode_ask_note "o/r" "123" "2" "abc1234" "https://example.test/c1")" "ok"
check "ask-note: hands the worker everything it needs" "$(cat "$SPAWNED_FILE")" \
	"ask-note-worker o/r 123 2 abc1234 https://example.test/c1 yes"

rm -f "$SPAWNED_FILE"
check "ask-note: passes on that the round was NOT posted" \
	"$(mode_ask_note "o/r" "123" "5" "abc1234" "" "no"; cat "$SPAWNED_FILE")" \
	"ok
ask-note-worker o/r 123 5 abc1234  no"
check "ask-note: a missing PR is refused" "$(mode_ask_note "o/r" "" "1" "abc" "")" \
	"error=引数が不足しています。"

# --- worker 側 --------------------------------------------------------------
NOTE_ANSWER=""
NOTE_CANCELLED=0
ask_note() { # title, prompt
	printf '%s\n' "$1" > "$WORK/note-title.txt"
	printf '%s\n' "$2" > "$WORK/note-prompt.txt"
	[ "$NOTE_CANCELLED" -eq 0 ] || return 1
	printf '%s' "$NOTE_ANSWER"
}

ALERTED_FILE="$WORK/alerted.txt"
note_alert() { printf '%s\n' "$2" > "$ALERTED_FILE"; }

OPENED_FILE="$WORK/opened.txt"
open_url() { printf '%s\n' "${1:-}" > "$OPENED_FILE"; }

printf '%s' "keychain-secret-value" > "$KEYCHAIN"
CURL_BODY='{"html_url":"https://github.com/o/r/pull/123#issuecomment-2"}'
CURL_CODE="201"

rm -f "$OPENED_FILE" "$ALERTED_FILE"
NOTE_CANCELLED=0
NOTE_ANSWER="3 階の梁が浮いている"
mode_ask_note_worker "o/r" "123" "2" "abc1234" "https://example.test/c1"
decoded="$(python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    print(json.load(handle)["body"], end="")
' "$CURL_PAYLOAD")"
check_contains "ask-note-worker: marks the note as its own kind of comment" "$decoded" \
	"<!-- homeskz-ifc-feedback-note v1 round=2 build=abc1234 posted=yes -->"
check_contains "ask-note-worker: quotes what the person wrote" "$decoded" \
	"> 3 階の梁が浮いている"
check_contains "ask-note-worker: says which round it belongs to" "$decoded" \
	"### 実機を見ての所見（round 2）"
check_contains "ask-note-worker: tells the person the round was posted" \
	"$(cat "$WORK/note-prompt.txt")" "round 2 を投稿しました"
check "ask-note-worker: opens the posted comment" "$(cat "$OPENED_FILE")" \
	"https://example.test/c1"

# --- 結果を投稿しなかった周（「送らない」を選んだとき） ----------------------
# **その周について PR に載るのはこの所見だけ**になるので、そう伝えたうえで訊く。
# 黙って終わると、読む側からはその周が丸ごと消える（実機 round 4 の所見）。
rm -f "$OPENED_FILE" "$ALERTED_FILE"
NOTE_CANCELLED=0
NOTE_ANSWER="今回は絵を見たかっただけ"
mode_ask_note_worker "o/r" "123" "5" "abc1234" "" "no"
decoded="$(python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    print(json.load(handle)["body"], end="")
' "$CURL_PAYLOAD")"
check_contains "ask-note-worker (unposted): records that the round was not posted" "$decoded" \
	"<!-- homeskz-ifc-feedback-note v1 round=5 build=abc1234 posted=no -->"
check_contains "ask-note-worker (unposted): says so in the heading" "$decoded" \
	"### 実機を見ての所見（round 5・結果は未投稿）"
check_contains "ask-note-worker (unposted): tells the person it is the only record" \
	"$(cat "$WORK/note-prompt.txt")" "唯一の記録"
check "ask-note-worker (unposted): opens nothing" "$(cat "$OPENED_FILE" 2>/dev/null)" ""

# 空のまま送れば、投稿しなかった周には**何も残らない**（本当に何も言いたくない人の逃げ道）。
CURL_PAYLOAD_BEFORE="$(cat "$CURL_PAYLOAD")"
NOTE_ANSWER=""
mode_ask_note_worker "o/r" "123" "6" "abc1234" "" "no"
check "ask-note-worker (unposted): an empty note posts nothing" "$(cat "$CURL_PAYLOAD")" \
	"$CURL_PAYLOAD_BEFORE"

# 複数行を書かれても引用が崩れない。
rm -f "$OPENED_FILE"
NOTE_ANSWER="1 行目
2 行目"
mode_ask_note_worker "o/r" "123" "3" "abc1234" ""
decoded="$(python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    print(json.load(handle)["body"], end="")
' "$CURL_PAYLOAD")"
check_contains "ask-note-worker: quotes every line" "$decoded" "> 1 行目
> 2 行目"

# 「送らない」と空欄は、どちらも投稿しない（が、投稿済みのコメントは開いて見せる）。
rm -f "$CURL_PAYLOAD" "$OPENED_FILE"
NOTE_CANCELLED=1
mode_ask_note_worker "o/r" "123" "2" "abc1234" "https://example.test/c1"
CHECKS=$((CHECKS + 1))
if [ -f "$CURL_PAYLOAD" ]; then
	echo "[ FAIL ] ask-note-worker: 「送らない」で投稿してはいけない"
	FAILURES=$((FAILURES + 1))
else
	echo "[ PASS ] ask-note-worker: posts nothing when the person declines"
fi
check "ask-note-worker: still opens the posted comment after declining" \
	"$(cat "$OPENED_FILE")" "https://example.test/c1"

rm -f "$CURL_PAYLOAD"
NOTE_CANCELLED=0
NOTE_ANSWER=""
mode_ask_note_worker "o/r" "123" "2" "abc1234" ""
CHECKS=$((CHECKS + 1))
if [ -f "$CURL_PAYLOAD" ]; then
	echo "[ FAIL ] ask-note-worker: 空欄で投稿してはいけない"
	FAILURES=$((FAILURES + 1))
else
	echo "[ PASS ] ask-note-worker: posts nothing for an empty note"
fi

# 投稿に失敗したら**必ず伝える**——プラグインはもう戻っていて、ここで黙ると
# 書いた所見がどこにも残らないまま消える。
rm -f "$ALERTED_FILE"
NOTE_ANSWER="なにか"
CURL_CODE="403"
CURL_BODY='{"message":"Resource not accessible by integration"}'
mode_ask_note_worker "o/r" "123" "2" "abc1234" ""
check_contains "ask-note-worker: says so when the note could not be posted" \
	"$(cat "$ALERTED_FILE")" "所見を投稿できませんでした"
CURL_CODE="201"
CURL_BODY='{"html_url":"https://github.com/o/r/pull/123#issuecomment-2"}'

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
