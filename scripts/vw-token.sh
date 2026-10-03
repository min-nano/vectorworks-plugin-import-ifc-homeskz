#!/usr/bin/env bash
#
# vw-token.sh — GitHub のトークンの在り処。**同梱スクリプトが共有する唯一の実装**で、
# キーチェーンの service 名も探索順もここ 1 か所にある（CLAUDE.md「重複を作らない
# 置き場所」）。
#
# 使う側は vw-update.sh（リリース一覧・stable リリースを読む）。**トークンは要らないが、
# 認証なしの GitHub API は IP ごとに 1 時間 60 回**で、M24〜M37 の往復のパレットが 1 分
# ごとに見に行ってちょうど上限に当たった（実機で「リリース一覧を取得できませんでした」と
# して出た。M27）。M37 まではもう 1 つの使い手（vw-feedback.sh。PR への投稿）がいて、
# login でキーチェーンへ保存していたので、そこを読む口は残してある。
#
# **このファイルは source される**（実行しない）。トークンを標準出力へ出すモードは
# 持たない——出せるようにした瞬間、`ps` やログに載る道ができる。呼び出し元が内部で
# `resolve_token` を呼び、curl のヘッダへ直接渡すこと。
#
# **配列を使わない。** macOS が積んでいる /bin/bash は 3.2 で、`set -u` の下で空の配列を
# 展開すると即死する（M23 の実機 round 1 で vw-feedback.sh が踏んだ）。
#
# トークンの探索順（最初に見つかったものを使う）:
#   1. 環境変数 HOMESKZ_IFC_FEEDBACK_TOKEN（Vectorworks を端末から起動したとき用）
#   2. キーチェーン（M37 までの vw-feedback.sh の login で入れたもの）
#   3. gh CLI の認証（開発機に gh が入っていれば設定は要らない）
#

# キーチェーンの service 名は**識別子なので据え置く**。プラグインの表示名やファイル名が
# 変わっても付け替えない——付け替えた瞬間、既に入っているトークンが行方不明になり、
# 利用者にもう一度貼り付けさせることになる（コマンドの UUID を据え置くのと同じ理由）。
VW_FEEDBACK_KEYCHAIN_SERVICE="${VW_FEEDBACK_KEYCHAIN_SERVICE:-HomeskzIfcFeedback}"

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

# resolve_token: 実際のトークン（無ければ空文字で 1 を返す）。
#
# **`set -e` の下でも呼び出し元を落とさない形に保つ**——vw-update.sh は `set -e` で走る
# ので、途中の探索が失敗しただけでスクリプトごと終わってはならない（呼び出し側も
# `$(resolve_token || true)` で受ける）。
resolve_token() {
	local t
	if [ -n "${HOMESKZ_IFC_FEEDBACK_TOKEN:-}" ]; then
		printf '%s' "$HOMESKZ_IFC_FEEDBACK_TOKEN"
		return 0
	fi
	if t="$(token_from_keychain)"; then
		if [ -n "$t" ]; then printf '%s' "$t"; return 0; fi
	fi
	if t="$(token_from_gh)"; then
		if [ -n "$t" ]; then printf '%s' "$t"; return 0; fi
	fi
	return 1
}
