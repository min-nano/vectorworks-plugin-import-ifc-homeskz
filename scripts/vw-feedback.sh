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
# Overridable via environment:
#   VW_REPO                        owner/repo（既定は下記）
#   HOMESKZ_IFC_FEEDBACK_TOKEN     トークン（探索順 1）
#   VW_FEEDBACK_KEYCHAIN_SERVICE   キーチェーンの service 名（既定 HomeskzIfcFeedback）
#
set -uo pipefail

VW_REPO="${VW_REPO:-min-nano/vectorworks-plugin-import-ifc-homeskz}"
VW_API="https://api.github.com"
VW_FEEDBACK_KEYCHAIN_SERVICE="${VW_FEEDBACK_KEYCHAIN_SERVICE:-HomeskzIfcFeedback}"

# gh CLI は GUI アプリの PATH には入っていないのが普通（Vectorworks は
# LaunchServices から起動され、PATH は /usr/bin:/bin:/usr/sbin:/sbin だけ）。
# だから探す場所を明示しておく——ここを省くと「端末では動くのに Vectorworks からは
# 動かない」という一番分かりにくい失敗になる。
VW_GH_CANDIDATES=(/opt/homebrew/bin/gh /usr/local/bin/gh /usr/bin/gh)

# ---------------------------------------------------------------------------
# トークンの取り出し。**標準出力に出すのは呼び出し元の内部だけ**で、機械可読出力へは
# 決して流さない。
# ---------------------------------------------------------------------------

gh_path() {
	local candidate
	for candidate in "${VW_GH_CANDIDATES[@]}"; do
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
	local token; token="$(resolve_token || true)"
	local auth=()
	[ -n "$token" ] && auth=(-H "Authorization: Bearer ${token}")

	local f; f="$(mktemp)"
	if ! curl -fsSL --max-time 20 --retry 2 "${auth[@]}" \
		-H "Accept: application/vnd.github+json" \
		"${VW_API}/repos/${repo}/pulls?state=open&head=${owner}:${branch}" -o "$f"; then
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
		*)            echo "error=不明なモード: '${mode}'（token-status / login / logout / find-pr / post）。" ;;
	esac
}

# Run main only when executed directly, NOT when sourced — the unit tests
# (tests/vw-feedback.test.sh) source this file to drive the modes with curl /
# plutil / security stubbed out, exactly as tests/vw-update.test.sh does.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
