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
#   loop-control <repo> <n> [since]  PR が open か・以後に「止めろ」の合図が付いたか
#                                    （モードレスの往復が周期的に呼ぶ。M24）
#
# **ダイアログはここには無い。** 尋ねるのは全部プラグイン側で、しかも**取り込みが始まる
# 前**に済ませる（src/draw/Feedback.h）。ここでダイアログを出すと、プラグインは popen の
# 出力を読み終わるまでメインスレッドを止めるので、そのあいだ図面が固まる。
#
# **トークンをコマンドラインに乗せない。** `login` が受け取るのは*ファイルのパス*で、
# 中身は読んだ直後に消す——引数はプロセス一覧（ps）から誰にでも見えるので、そこへ
# 秘密を置いてはならない。表示・ログにもトークンは一切出さない。
#
# トークンの探索順（最初に見つかったものを使う）。**実装は同梱の vw-token.sh**で、
# リリース一覧を読む vw-update.sh と共有する:
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

# **トークンの在り処は同梱の vw-token.sh ただ 1 つ**（vw-update.sh と共有する。
# CLAUDE.md「重複を作らない置き場所」）。キーチェーンの service 名・探索順・gh の
# 探し場所はそちらにある。**隣に置かれる前提**——同じ zip で一緒に配られ、同じ
# フォルダ（mac は Contents/Resources、Windows はモジュールの隣）に並ぶ。
# shellcheck source-path=SCRIPTDIR
# shellcheck source=vw-token.sh
. "$(dirname "${BASH_SOURCE[0]}")/vw-token.sh"

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
# GET 1 回（あればトークンを付ける）。**配列を使わない**ため、付ける／付けないで
# curl の呼び出しを 2 つ書く（冒頭「配列を使わない」）。取れたら 0。
# ---------------------------------------------------------------------------
api_get() { # token, url, out-file
	if [ -n "$1" ]; then
		curl -fsSL --max-time 20 --retry 2 \
			-H "Authorization: Bearer ${1}" \
			-H "Accept: application/vnd.github+json" "$2" -o "$3"
	else
		curl -fsSL --max-time 20 --retry 2 \
			-H "Accept: application/vnd.github+json" "$2" -o "$3"
	fi
}

# コメント本文から**往復への合図**を読む。合図は
#   <!-- homeskz-ifc-feedback v1 control=stop -->
# **ただ 1 行**で、その行に他の文字があってはならない。合図なら "stop"、無ければ "none"。
#
# **「書いてある」と「合図である」は違う。** 本文のどこかに現れる `control=stop` を拾う作り
# では、**この目印を説明した文章が合図になってしまう**——実機 round 1 でまさにそれが起きた
# （プラグイン自身の投稿が末尾で「合図はこう書きます」と案内しており、その 1 通目を読んだ
# 時点で往復が止まった。docs/DEV-NOTES.md M24「合図は行、文中の引用ではない」）。
# だから 3 つで縛る:
#
#   1. **行がまるごと目印であること**（前後は空白だけ）。文中に引用したものは合図でない。
#   2. **``` で囲まれた中は読まない**（例として貼ったものを合図にしない）。
#   3. **プラグイン自身の投稿は合図にしない**（`round=` / `control=ended` の目印を持つ本文）。
#      1 と 2 で足りているが、ここは「自分の言葉で自分が止まる」を二度と起こさないための
#      歯止めなので、判定を重ねておく。
control_of_comment() { # body-text
	printf '%s\n' "$1" | LC_ALL=C awk '
		/^[[:space:]]*```/ { fence = !fence; next }
		fence { next }
		{
			line = $0
			sub(/^[[:space:]]+/, "", line)
			sub(/[[:space:]]+$/, "", line)
			if (line ~ /^<!--[[:space:]]*homeskz-ifc-feedback[[:space:]]+v1[[:space:]]+round=/) { self = 1 }
			if (line ~ /^<!--[[:space:]]*homeskz-ifc-feedback[[:space:]]+v1[[:space:]]+control=ended/) { self = 1 }
			if (line ~ /^<!--[[:space:]]*homeskz-ifc-feedback[[:space:]]+v1[[:space:]]+control=stop[[:space:]]*-->$/) { stop = 1 }
		}
		END { print (stop && !self) ? "stop" : "none" }
	'
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
	# **投稿した時刻（ISO 8601, UTC）も返す。** 往復の駆動はこれを次の loop-control の
	# since に使い、「自分の投稿より後に付いた合図」だけを読む（古い合図を何度も読まない）。
	local created; created="$(jval "$out" "created_at")"
	rm -f "$out"
	[ -n "$url" ] && echo "url=${url}"
	[ -n "$created" ] && echo "created=${created}"
	echo "ok"
}

# loop-control <repo> <issue-number> [since]: **往復を続けてよいか**（M24。モードレスの
# 往復が周期的に呼ぶ）。答えは 2 つ:
#   state=<open|closed|merged>   PR の状態（閉じたら続ける相手がいない）
#   control=<stop|none>          since 以降のコメントに「止めろ」の合図があるか
#   ok
# since は ISO 8601（post が返した created=）。**無ければ最近のコメントから読む**
# （古い記憶で走っているとき用。上限 5 ページ＝500 通）。合図が複数あれば最後のものが
# 勝つ——が、合図は stop しか無いので、1 つでもあれば stop。
mode_loop_control() {
	local repo="${1:-$VW_REPO}" number="${2:-}" since="${3:-}"
	if [ -z "$number" ]; then
		echo "error=PR 番号が指定されていません。"; return 0
	fi
	local token; token="$(resolve_token || true)"

	local f; f="$(mktemp)"
	if ! api_get "$token" "${VW_API}/repos/${repo}/pulls/${number}" "$f"; then
		rm -f "$f"; echo "error=PR の状態を取得できませんでした（ネットワークか権限）。"; return 0
	fi
	local state; state="$(jval "$f" "state")"
	local merged; merged="$(jval "$f" "merged")"
	rm -f "$f"
	if [ -z "$state" ]; then
		echo "error=PR の状態を読めませんでした。"; return 0
	fi
	# 真偽の綴りは読み手（plutil / 試験の python 代替）で揺れるので寛容に読む。
	case "$merged" in
		true | True | TRUE | 1) state="merged" ;;
	esac

	local control="none" page=1 maxpage=5 url i body count
	# since があれば「自分の投稿より後」だけなので 1〜2 ページで足りる。
	[ -n "$since" ] && maxpage=2
	while [ "$page" -le "$maxpage" ]; do
		url="${VW_API}/repos/${repo}/issues/${number}/comments?per_page=100&page=${page}"
		[ -n "$since" ] && url="${url}&since=${since}"
		f="$(mktemp)"
		if ! api_get "$token" "$url" "$f"; then
			rm -f "$f"; echo "error=PR のコメントを取得できませんでした（ネットワークか権限）。"; return 0
		fi
		count=0
		i=0
		while [ "$i" -lt 100 ]; do
			# 項目の有無は id で見る（本文は空でもよい）。
			[ -n "$(jval "$f" "${i}.id")" ] || break
			count=$((count + 1))
			body="$(jval "$f" "${i}.body")"
			[ "$(control_of_comment "$body")" = "stop" ] && control="stop"
			i=$((i + 1))
		done
		rm -f "$f"
		[ "$count" -lt 100 ] && break
		page=$((page + 1))
	done

	echo "state=${state}"
	echo "control=${control}"
	echo "ok"
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
		loop-control) mode_loop_control "${1:-}" "${2:-}" "${3:-}" ;;
		*)            echo "error=不明なモード: '${mode}'（token-status / login / logout / find-pr / post / loop-control）。" ;;
	esac
}

# Run main only when executed directly, NOT when sourced — the unit tests
# (tests/vw-feedback.test.sh) source this file to drive the modes with curl /
# plutil / security stubbed out, exactly as tests/vw-update.test.sh does.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
