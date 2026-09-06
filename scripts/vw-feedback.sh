#!/usr/bin/env bash
#
# vw-feedback.sh — post the plug-in's run report back to the pull request it was
# built from, so the local Vectorworks check can answer Claude without anybody
# copying text between windows (docs/DEVELOPMENT.md「実機フィードバックの往復」).
#
# This is the network back end ONLY. Like vw-update.sh it is NON-INTERACTIVE and
# machine-readable: every mode prints "key=value" lines (plus a bare "ok" on
# success) and never shows a dialog — the plug-in parses the output and shows its
# own native Vectorworks dialogs. Transient failures are reported as
# "error=<message>" with exit 0, so the plug-in stays in control of what the user
# sees.
#
#   token-status               ソース（keychain / gh / env / none）と使えるかどうか
#   login <token-file>         ファイルのトークンをキーチェーンへ入れて、ファイルを消す
#   logout                     キーチェーンから消す
#   find-pr <repo> <branch>    そのブランチの open な PR 番号を引く
#   post <repo> <n> <body-file>  PR（= issue）へコメントを 1 通投稿する
#   ask-note <repo> <n> <round> <build> [<url>]
#                              **所見を尋ねて投稿する**。すぐ返り、ダイアログは別プロセスに残る
#
# **ask-note が「待たない」のが肝。** プラグインは同梱スクリプトの出力を読み終わるまで
# Vectorworks のメインスレッドを止める（popen）ので、ここでダイアログを出して待つと
# **図面が固まって見られない**——所見を書くために絵を見たい、という当の目的が果たせない。
# そこで ask-note は**自分自身を detached で起こし直して即座に `ok` を返す**。プラグインは
# すぐ戻り、Vectorworks は完全に操作できる状態になる。利用者は図面を拡大・レイヤ切り替え
# しながら、別プロセスのダイアログに所見を書いて送れる（M23。VW のレイアウトダイアログは
# モーダル前提で、モードレスにするには別の拡張種別が要る——[SDK リファレンス
# 「モードレス（非モーダル）なパレット」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)）。
#
# **戻り先が無いので、投稿までスクリプトがやり切る。** プラグインは既に戻っているから、
# 所見を受け取って投稿するところまでこちらの仕事になる。所見は**独立した 1 通**として
# 投稿する（本文へ差し込まない）——差し込む形にすると、コメントの組み立てが C++ 側
# （parse/Feedback）とここの 2 か所に割れてしまう。
#
# **トークンをコマンドラインに乗せない。** `login` が受け取るのは*ファイルのパス*で、
# 中身は読んだ直後に消す——引数はプロセス一覧（ps）から誰にでも見えるので、そこへ
# 秘密を置いてはならない。表示・ログにもトークンは一切出さない。
#
# トークンの探索順（最初に見つかったものを使う）:
#   1. 環境変数 HOMESKZ_IFC_FEEDBACK_TOKEN（Vectorworks を端末から起動したとき用）
#   2. キーチェーン（login で入れたもの。GUI から使う常用の経路）
#   3. gh CLI の認証（開発機に gh が入っていれば設定は要らない）
# 必要な権限は**その 1 リポジトリの Issues への読み書き**だけ（fine-grained PAT なら
# "Pull requests: Read and write"）。それ以上の権限を要求しない。
#
# Requirements: macOS only. Uses tools that ship with macOS (curl, plutil, awk,
# security) — no Homebrew, no python.
#
# **配列を使わない。** macOS が積んでいる /bin/bash は 3.2 で（プラグインはこれで起動する。
# src/Updater.cpp）、`set -u` の下で**空の配列を展開すると "unbound variable" で即死する**
# （bash 4.4 で直った古い不具合）。実際 round 1 で、トークン未登録のときだけ `find-pr` が
# 何も出さずに落ちた——`auth=()` が空だったためで、Linux の bash 5 で走るテストでは
# 再現しなかった。**配列が 1 つでもあると同じ穴が空くので、この 1 本では使わない**
# （tests/vw-feedback.test.sh がその不在を検査している）。
#
# Overridable via environment:
#   VW_REPO                        owner/repo（既定は下記）
#   HOMESKZ_IFC_FEEDBACK_TOKEN     トークン（探索順 1）
#   VW_FEEDBACK_KEYCHAIN_SERVICE   キーチェーンの service 名（既定 HomeskzIfcFeedback）
#
set -uo pipefail

VW_REPO="${VW_REPO:-min-nano/vectorworks-plugin-import-ifc-homeskz}"
VW_API="https://api.github.com"
# キーチェーンの service 名は**識別子なので据え置く**。プラグインの表示名やファイル名が
# 変わっても付け替えない——付け替えた瞬間、既に入っているトークンが行方不明になり、
# 利用者にもう一度貼り付けさせることになる（コマンドの UUID を据え置くのと同じ理由）。
VW_FEEDBACK_KEYCHAIN_SERVICE="${VW_FEEDBACK_KEYCHAIN_SERVICE:-HomeskzIfcFeedback}"

# ---------------------------------------------------------------------------
# トークンの取り出し。**標準出力に出すのは呼び出し元の内部だけ**で、機械可読出力へは
# 決して流さない。
# ---------------------------------------------------------------------------

# gh CLI は GUI アプリの PATH には入っていないのが普通（Vectorworks は LaunchServices
# から起動され、PATH は /usr/bin:/bin:/usr/sbin:/sbin だけ）。だから探す場所を並べておく
# ——ここを省くと「端末では動くのに Vectorworks からは動かない」という一番分かりにくい
# 失敗になる。**配列にしない**（冒頭「配列を使わない」）。
gh_path() {
	local candidate
	for candidate in /opt/homebrew/bin/gh /usr/local/bin/gh /usr/bin/gh; do
		[ -x "$candidate" ] && { printf '%s' "$candidate"; return 0; }
	done
	command -v gh 2>/dev/null || return 1
}

token_from_keychain() {
	security find-generic-password -s "$VW_FEEDBACK_KEYCHAIN_SERVICE" -w 2>/dev/null || return 1
}

token_from_gh() {
	local gh; gh="$(gh_path)" || return 1
	"$gh" auth token 2>/dev/null || return 1
}

# token_source: どこから取れるか（取れなければ "none"）。トークン自体は出さない。
token_source() {
	[ -n "${HOMESKZ_IFC_FEEDBACK_TOKEN:-}" ] && { echo "env"; return 0; }
	[ -n "$(token_from_keychain)" ] && { echo "keychain"; return 0; }
	[ -n "$(token_from_gh)" ] && { echo "gh"; return 0; }
	echo "none"
}

# resolve_token: 実際のトークン（無ければ空文字で 1 を返す）。
resolve_token() {
	local t
	if [ -n "${HOMESKZ_IFC_FEEDBACK_TOKEN:-}" ]; then
		printf '%s' "$HOMESKZ_IFC_FEEDBACK_TOKEN"; return 0
	fi
	t="$(token_from_keychain)" && [ -n "$t" ] && { printf '%s' "$t"; return 0; }
	t="$(token_from_gh)" && [ -n "$t" ] && { printf '%s' "$t"; return 0; }
	return 1
}

# ---------------------------------------------------------------------------
# JSON。読むのは plutil（macOS 同梱で JSON をそのまま読める。vw-update.sh と同じ）、
# 書くのは awk——**本文は任意のテキスト**（診断ログ・利用者の所見）なので、素朴な
# 文字列連結では必ず壊れる。エスケープを 1 か所に閉じ込める。
# ---------------------------------------------------------------------------

jval() { # json-file, keypath -> raw scalar value (empty if missing)
	plutil -extract "$2" raw -o - "$1" 2>/dev/null || true
}

# json_string_from_file: ファイルの中身を JSON 文字列リテラル（引用符つき）にする。
# LC_ALL=C はバイト単位で扱わせるため——UTF-8 の各バイトは 0x80 以上なので、下の
# 制御文字クラスに巻き込まれない（日本語の本文がそのまま通る）。
json_string_from_file() { # file
	LC_ALL=C awk '
		BEGIN { printf "\"" }
		{
			s = $0
			gsub(/\\/, "\\\\", s)
			gsub(/"/, "\\\"", s)
			gsub(/\t/, "\\t", s)
			gsub(/\r/, "", s)
			gsub(/[\001-\010\013\014\016-\037]/, "", s)
			printf "%s\\n", s
		}
		END { printf "\"" }
	' "$1"
}

# ---------------------------------------------------------------------------
# Modes.
# ---------------------------------------------------------------------------

# token-status: 使えるトークンがあるか。**トークンは出さない。**
#   source=<env|keychain|gh|none>
#   ok=<yes|no>
mode_token_status() {
	local src; src="$(token_source)"
	echo "source=${src}"
	if [ "$src" = "none" ]; then
		echo "ok=no"
	else
		echo "ok=yes"
	fi
}

# login <token-file>: ファイルのトークンをキーチェーンへ。**読んだファイルは必ず消す**
# （プラグインが一時ファイルへ書いて渡す。引数に秘密を乗せないための経路）。
mode_login() {
	local file="${1:-}"
	if [ -z "$file" ] || [ ! -f "$file" ]; then
		echo "error=トークンのファイルが見つかりません。"; return 0
	fi
	local token; token="$(tr -d '\r\n' < "$file")"
	rm -f "$file"
	if [ -z "$token" ]; then
		echo "error=トークンが空です。"; return 0
	fi
	# 既存の項目があれば置き換える（-U）。-w で値を渡すのは security の作法。
	# -a（アカウント名）は表示のためだけのもの。**$USER が無い環境でも落とさない**
	# ——Vectorworks は LaunchServices から起動されるので、環境変数は端末より痩せている。
	local account="${USER:-vectorworks}"
	if security add-generic-password -U -s "$VW_FEEDBACK_KEYCHAIN_SERVICE" \
		-a "$account" -w "$token" >/dev/null 2>&1; then
		echo "ok"
	else
		echo "error=キーチェーンへ保存できませんでした。"
	fi
}

mode_logout() {
	security delete-generic-password -s "$VW_FEEDBACK_KEYCHAIN_SERVICE" >/dev/null 2>&1 || true
	echo "ok"
}

# find-pr <repo> <branch>: そのブランチの open な PR。**番号を人に打たせないため**の口で、
# 見つからなければ error= を返す（プラグインは番号の手入力へ落ちる）。
mode_find_pr() {
	local repo="${1:-$VW_REPO}" branch="${2:-}"
	if [ -z "$branch" ]; then
		echo "error=ブランチが指定されていません。"; return 0
	fi
	local owner="${repo%%/*}"
	# **トークンは要らない**（公開リポジトリの open な PR を引くだけ）。あれば付けるが、
	# 無くても引けなければならない——**登録の前に PR 番号を埋めるのがこの口の目的**
	# だからである（round 1 でここが死に、番号を手入力させてしまった）。
	local token; token="$(resolve_token || true)"
	local url="${VW_API}/repos/${repo}/pulls?state=open&head=${owner}:${branch}"

	local f; f="$(mktemp)"
	local got=1
	if [ -n "$token" ]; then
		curl -fsSL --max-time 20 --retry 2 \
			-H "Authorization: Bearer ${token}" \
			-H "Accept: application/vnd.github+json" "$url" -o "$f" && got=0
	else
		curl -fsSL --max-time 20 --retry 2 \
			-H "Accept: application/vnd.github+json" "$url" -o "$f" && got=0
	fi
	if [ "$got" -ne 0 ]; then
		rm -f "$f"; echo "error=PR を検索できませんでした（ネットワークか権限）。"; return 0
	fi
	local number; number="$(jval "$f" "0.number")"
	local title; title="$(jval "$f" "0.title")"
	rm -f "$f"
	if [ -z "$number" ]; then
		echo "error=ブランチ ${branch} に open な PR がありません。"; return 0
	fi
	echo "pr=${number}"
	[ -n "$title" ] && echo "title=${title}"
	echo "ok"
}

# post <repo> <issue-number> <body-file>: PR へコメントを 1 通。
#   url=<コメントの URL>
#   ok
mode_post() {
	local repo="${1:-$VW_REPO}" number="${2:-}" body="${3:-}"
	if [ -z "$number" ] || [ -z "$body" ] || [ ! -f "$body" ]; then
		echo "error=引数が不足しています。"; return 0
	fi
	local token; token="$(resolve_token || true)"
	if [ -z "$token" ]; then
		echo "error=GitHub のトークンがありません（先に login してください）。"; return 0
	fi

	local payload; payload="$(mktemp)"
	{ printf '{"body":'; json_string_from_file "$body"; printf '}'; } > "$payload"

	local out; out="$(mktemp)"
	local code
	code="$(curl -sS --max-time 60 --retry 2 -o "$out" -w '%{http_code}' \
		-X POST \
		-H "Authorization: Bearer ${token}" \
		-H "Accept: application/vnd.github+json" \
		-H "Content-Type: application/json" \
		--data-binary "@${payload}" \
		"${VW_API}/repos/${repo}/issues/${number}/comments" 2>/dev/null)"
	rm -f "$payload"

	if [ "$code" != "201" ]; then
		# GitHub の言い分をそのまま渡す（権限不足か PR 違いかが、これで切り分けられる）。
		local message; message="$(jval "$out" "message")"
		rm -f "$out"
		[ -n "$message" ] || message="HTTP ${code}"
		echo "error=コメントを投稿できませんでした（${message}）。"
		return 0
	fi
	local url; url="$(jval "$out" "html_url")"
	rm -f "$out"
	[ -n "$url" ] && echo "url=${url}"
	echo "ok"
}

# ---------------------------------------------------------------------------
# 所見を尋ねる（ask-note）。**別プロセスのダイアログ**なので、開いている間も
# Vectorworks は動かせる（冒頭「ask-note が『待たない』のが肝」）。
#
# UI は macOS 同梱の osascript。値は argv で渡し、スクリプト本文へ埋め込まない
# （scripts/vw-update.sh の同じヘルパーと同じ作法——題名や本文で AppleScript を
# 壊せないようにするため）。
# ---------------------------------------------------------------------------

# ask_note: 所見を 1 つ尋ねる。「送る」なら入力（空のこともある）を echo して 0、
# 「送らない」／キャンセルなら何も出さずに 1。
ask_note() { # title, prompt
	osascript - "$1" "$2" <<'APPLESCRIPT' 2>/dev/null || return 1
on run argv
	set r to display dialog (item 2 of argv) with title (item 1 of argv) default answer "" buttons {"送らない", "送る"} default button "送る" cancel button "送らない"
	return text returned of r
end run
APPLESCRIPT
}

# note_alert: 伝えないと黙って消えてしまうことだけを出す（投稿の失敗）。
note_alert() { # title, message
	osascript - "$1" "$2" <<'APPLESCRIPT' >/dev/null 2>&1 || true
on run argv
	display dialog (item 2 of argv) with title (item 1 of argv) buttons {"OK"} default button "OK"
end run
APPLESCRIPT
}

# open_url: 投稿したコメントをブラウザで開く（失敗しても黙って続ける）。
open_url() { # url
	[ -n "$1" ] || return 0
	open "$1" >/dev/null 2>&1 || true
}

# spawn_self: 自分自身を detached で起こす。**出力を捨てるのが肝**——繋いだままだと、
# 呼び出し元（プラグインの popen）が子の終了まで EOF を見られず、結局待つことになる。
# $0 は実行されたこのスクリプト。**テストが差し替える唯一の口**でもある。
spawn_self() { # args...
	nohup /bin/bash "$0" "$@" >/dev/null 2>&1 &
}

# ask-note: **すぐ返る。** 自分自身を detached で起こし直し、`ok` を出して終わる。
# ダイアログと投稿はその子（ask-note-worker）が引き受ける。
mode_ask_note() {
	local repo="${1:-}" number="${2:-}" round="${3:-}" build="${4:-}" url="${5:-}"
	if [ -z "$number" ]; then
		echo "error=引数が不足しています。"; return 0
	fi
	spawn_self ask-note-worker "$repo" "$number" "$round" "$build" "$url"
	echo "ok"
}

# ask-note-worker: 別プロセス側の本体。**プラグインはもう戻っている**ので、尋ねてから
# 投稿するところまでここでやり切る。
#
# 所見は**独立した 1 通**として投稿する（取り込み結果の本文へ差し込まない）。差し込む形に
# すると、コメントの組み立てが C++ 側（parse/Feedback）とここの 2 か所に割れる。
mode_ask_note_worker() {
	local repo="${1:-}" number="${2:-}" round="${3:-}" build="${4:-}" url="${5:-}"
	local prompt="round ${round} を投稿しました。
図面を確かめて、気付いたことがあれば書いてください（空のまま送れば所見なしで終わります）。"

	local note
	if ! note="$(ask_note "実機フィードバック round ${round}" "$prompt")"; then
		open_url "$url" # 「送らない」でも、投稿そのものは済んでいるので開いて見せる
		return 0
	fi
	note="$(printf '%s' "$note" | tr -d '\r')"
	if [ -z "$note" ]; then
		open_url "$url"
		return 0
	fi

	local body; body="$(mktemp)"
	{
		# 機械可読の目印。取り込み結果のコメント（parse/Feedback）とは別の種別にして、
		# **人が書いた所見だと読む側が判別できる**ようにする。
		printf '<!-- homeskz-ifc-feedback-note v1 round=%s build=%s -->\n' "$round" "$build"
		printf '### 実機を見ての所見（round %s）\n\n' "$round"
		printf '%s\n' "$note" | awk '{ print "> " $0 }'
	} > "$body"

	local out; out="$(mode_post "$repo" "$number" "$body")"
	rm -f "$body"
	local reason; reason="$(printf '%s\n' "$out" | sed -n 's/^error=//p' | head -n 1)"
	if [ -n "$reason" ]; then
		note_alert "実機フィードバック" "所見を投稿できませんでした（${reason}）"
		return 0
	fi
	open_url "$url"
}

# ---------------------------------------------------------------------------
main() {
	command -v curl >/dev/null 2>&1 || { echo "error=curl が見つかりません。"; exit 0; }

	local mode="${1:-}"
	shift || true
	case "$mode" in
		token-status) mode_token_status ;;
		login)        mode_login "${1:-}" ;;
		logout)       mode_logout ;;
		find-pr)      mode_find_pr "${1:-}" "${2:-}" ;;
		post)         mode_post "${1:-}" "${2:-}" "${3:-}" ;;
		ask-note)     mode_ask_note "${1:-}" "${2:-}" "${3:-}" "${4:-}" "${5:-}" ;;
		# 内部用（ask-note が自分を起こし直すときの入口。人が直接呼ぶものではない）。
		ask-note-worker)
			mode_ask_note_worker "${1:-}" "${2:-}" "${3:-}" "${4:-}" "${5:-}" ;;
		*)            echo "error=不明なモード: '${mode}'（token-status / login / logout / find-pr / post / ask-note）。" ;;
	esac
}

# Run main only when executed directly, NOT when sourced — the unit tests
# (tests/vw-feedback.test.sh) source this file to drive the modes with curl /
# plutil / security stubbed out, exactly as tests/vw-update.test.sh does.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
