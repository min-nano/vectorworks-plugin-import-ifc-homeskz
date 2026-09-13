#!/usr/bin/env bash
#
# vw-update.sh — download the latest CI build of the min-nano_structure Vectorworks plug-in
# and install it into your Vectorworks 2026 Plug-Ins folder.
#
# Two channels, two separately-named plug-ins that can be installed at once:
#
#   stable  -> "min-nano_structure.vwlibrary"     from the rolling "stable" release (main).
#   dev     -> "min-nano_structureDev.vwlibrary"  from a per-branch "dev-<branch>" prerelease;
#              you pick which branch's build to install.
#
# Flow: check the latest build, tell you whether a newer one is available, then
# let you choose: 更新しない / 更新だけ (skip / update only). The new build is
# loaded the next time you (re)start Vectorworks yourself.
#
# The plug-in itself drives its own updates by invoking this same script (it is
# bundled inside the .vwlibrary, see src/Updater.cpp). The plug-in shows all of
# its own NATIVE Vectorworks dialogs (AlertInform / AlertQuestion), so this
# script exposes a small NON-INTERACTIVE, machine-readable back end for it — no
# dialogs of its own in those modes:
#
#   q-stable            Print the stable channel status as key=value lines:
#                       installed=<commit|none> / latest=<commit> / url=<zip url>
#                       (or error=<message>).
#   q-dev               Print installed=<commit|none> and installed-branch=<branch>,
#                       then one TSV line per dev build:
#                       "build<TAB>commit<TAB>name<TAB>url<TAB>branch"
#                       (or error=<message>).
#   do-install <url> <name>   Download+install <name>.vwlibrary; print "ok" or
#                             error=<message>. No dialogs.
#
# The interactive stable/dev modes below are the manual, run-from-a-terminal
# fallback and keep using macOS (osascript) dialogs.
#
# **ファイルの配置はこのスクリプトが決めない。** 走るのは常に**インストール済みの
# （＝古い）**この 1 本なので、ここに配置手順を持たせると「新しいビルドがどんなファイルで
# できているか」を永遠に知らないままになる。実際 M21 で本体（.vwpayload）が増えたとき、
# 古いアップデータはそれを写さず、利用者は zip を手で落として置き直す羽目になった。
#
# そこで配置は**落とした zip の中の scripts/vw-install.sh**（リリースのアセットとしても
# 公開されている）へ委ねる。委ね先はそのビルドと同じ版なので、ファイル構成や手順が
# 変わっても自動アップデートだけで追随できる。zip にインストーラが無い——この仕組みより
# 前のリリース——ときだけ、下の自前の配置へ落ちる。
#
# Usage:
#   ./scripts/vw-update.sh            # ask which channel (or double-click)
#   ./scripts/vw-update.sh stable
#   ./scripts/vw-update.sh dev
#   ./scripts/vw-update.sh q-stable                 # (used by the plug-in)
#   ./scripts/vw-update.sh q-dev                    # (used by the plug-in)
#   ./scripts/vw-update.sh do-install <url> <name>  # (used by the plug-in)
#
# Requirements: macOS only. Uses tools that ship with macOS (curl, plutil,
# unzip, codesign, xattr, osascript) — no Homebrew, no `gh`, and because the
# repository is public, no authentication.
#
# Overridable via environment:
#   VW_REPO         owner/repo             (default below)
#   VW_PLUGINS_DIR  Vectorworks Plug-Ins   (default: user folder for VW 2026)
#
set -euo pipefail

VW_REPO="${VW_REPO:-min-nano/vectorworks-plugin-import-ifc-homeskz}"
VW_PLUGINS_DIR="${VW_PLUGINS_DIR:-$HOME/Library/Application Support/Vectorworks/2026/Plug-Ins}"
VW_API="https://api.github.com/repos/${VW_REPO}"

# 配布 zip のアセット名の末尾。アセット名を丸ごと決め打ちにせず末尾で拾えるようにして
# あるのは、**将来アセット名（プラグイン名）が変わっても、インストール済みの古いこの
# スクリプトが「見つかりません」で止まらない**ようにするため（plugin_zip_url）。
VW_ZIP_SUFFIX=".vwlibrary.zip"

# 配置を委ねる先（落とした zip の直下にある）。冒頭のコメント参照。
VW_INSTALLER="vw-install.sh"

# ---------------------------------------------------------------------------
# Small AppleScript UI helpers. Values are passed as argv (never interpolated
# into the script text) so titles/messages can't break the AppleScript.
# ---------------------------------------------------------------------------
alert() { # title, message
	osascript - "$1" "$2" <<'APPLESCRIPT' >/dev/null 2>&1 || true
on run argv
	display dialog (item 2 of argv) with title (item 1 of argv) buttons {"OK"} default button "OK"
end run
APPLESCRIPT
}

notify() { # title, message
	osascript - "$1" "$2" <<'APPLESCRIPT' >/dev/null 2>&1 || true
on run argv
	display notification (item 2 of argv) with title (item 1 of argv)
end run
APPLESCRIPT
}

die() { # message
	echo "error: $1" >&2
	alert "みんなの構造設計支援 アップデート" "エラー: $1"
	exit 1
}

# ask2: show the two-way choice. Echoes the chosen button; cancel -> skip.
ask2() { # title, message
	osascript - "$1" "$2" <<'APPLESCRIPT' 2>/dev/null || echo "更新しない"
on run argv
	try
		set r to button returned of (display dialog (item 2 of argv) with title (item 1 of argv) buttons {"更新しない", "更新だけ"} default button "更新だけ" cancel button "更新しない")
		return r
	on error number -128
		return "更新しない"
	end try
end run
APPLESCRIPT
}

# choose_one: prompt + list of items (as argv). Echoes the chosen item, or "".
choose_one() { # prompt, item1, item2, ...
	osascript - "$@" <<'APPLESCRIPT' 2>/dev/null || echo ""
on run argv
	set thePrompt to item 1 of argv
	set theItems to items 2 thru -1 of argv
	set chosen to choose from list theItems with prompt thePrompt without multiple selections allowed
	if chosen is false then return ""
	return item 1 of chosen
end run
APPLESCRIPT
}

# ---------------------------------------------------------------------------
# GitHub REST helpers. JSON is parsed with plutil, which ships with macOS and
# reads JSON natively.
#
# **公開リポジトリなのでトークンは要らない——が、あるなら必ず付ける。** 認証なしの
# GitHub API は **IP ごとに 1 時間 60 回**で、往復のパレット（M24）は 1 分ごとに
# `q-dev` を呼ぶ＝ちょうど上限。取り込みコマンドのついでの確認・「今すぐ確認」・
# 同じ回線のもう 1 台が 1 回でも挟まれば超え、以後その時間内はずっと 403 になる
# （実機 M27。パレットには「リリース一覧を取得できませんでした」とだけ出ていて、
# ネットワークが切れたようにしか見えなかった）。トークンを付ければ 1 時間 5000 回に
# なるので、この経路では事実上当たらない。
#
# トークンの在り処は同梱の vw-token.sh ただ 1 つ（vw-feedback.sh と共有する。
# CLAUDE.md「重複を作らない置き場所」）。**無くても止まらない**——読めなければ従来
# どおり認証なしで続け、上限に当たったときだけその旨を理由に載せる。
# ---------------------------------------------------------------------------

if [ -r "$(dirname "${BASH_SOURCE[0]}")/vw-token.sh" ]; then
	# shellcheck source-path=SCRIPTDIR
	# shellcheck source=vw-token.sh
	. "$(dirname "${BASH_SOURCE[0]}")/vw-token.sh"
fi

# api_get の結果を持ち帰る 2 つ。**戻り値は標準出力ではなくこの変数**にする——理由
# （VW_API_ERROR）を呼び出し元へ渡すには同じシェルで動く必要があり、`$(api_get …)` の
# 形にすると副シェルの中で立てた変数が捨てられる。
VW_API_FILE=""   # 取れた JSON の一時ファイル（呼び出し元が rm する）
VW_API_ERROR=""  # 取れなかった理由（1 行。日本語）

# curl_reason: curl の終了コードを 1 行の日本語にする。**「取得できませんでした」だけで
# 終わらせない**——往復は無人で回るので、パレットに出る 1 行が唯一の手掛かりになる。
curl_reason() { # curl exit code
	case "$1" in
		6)     printf 'GitHub の名前を解決できません（curl 6: DNS）。' ;;
		7)     printf 'GitHub へ接続できません（curl 7）。' ;;
		28)    printf '20 秒以内に応答がありませんでした（curl 28: タイムアウト）。' ;;
		35|60) printf 'TLS の接続に失敗しました（curl %s）。' "$1" ;;
		*)     printf 'ネットワークに届きませんでした（curl 終了コード %s）。' "$1" ;;
	esac
}

# header_value: 応答ヘッダの値（名前は小文字で渡す。無ければ空）。**最後の 1 つ**を採る
# ——リダイレクトを追うとヘッダの塊が複数並ぶので、効いているのは最後のもの。
header_value() { # header-file, lowercase-name
	tr -d '\r' < "$1" |
		awk -F': ' -v want="$2" 'tolower($1) == want { v = $2 } END { if (v != "") print v }'
}

# http_reason: HTTP の失敗を 1 行の日本語にする。**API 制限だけは別扱い**——いつ戻るかと
# 「認証の有無で上限が違う」ことまで言えば、待てばよいのか設定が要るのかが分かる。
http_reason() { # code, header-file, token(空なら認証なし)
	local code="$1" hdr="$2" token="$3" remaining reset now mins limit
	remaining="$(header_value "$hdr" x-ratelimit-remaining)"
	if { [ "$code" = "403" ] || [ "$code" = "429" ]; } && [ "$remaining" = "0" ]; then
		if [ -n "$token" ]; then limit="1 時間 5000 回"; else limit="認証なしは 1 時間 60 回"; fi
		reset="$(header_value "$hdr" x-ratelimit-reset)"
		now="$(date +%s)"
		mins=""
		# **`&&` で終わらせない。** 偽のとき節そのものが失敗扱いになり、`set -e` の下では
		# 呼び出し元（api_get）ごと落ちる。
		case "$reset" in
			'' | *[!0-9]*) ;;
			*)
				mins="$(( (reset - now + 59) / 60 ))"
				if [ "$mins" -lt 0 ]; then mins=0; fi
				;;
		esac
		if [ -n "$mins" ]; then
			printf 'GitHub の API 制限に達しました（%s）。あと %s 分で戻ります。' "$limit" "$mins"
		else
			printf 'GitHub の API 制限に達しました（%s）。' "$limit"
		fi
		return 0
	fi
	printf 'GitHub が HTTP %s を返しました。' "$code"
}

# api_error: error= の 1 行に添える本文。理由が分かっていれば足す。**改行を入れない**
# ——プラグインは key=value の 1 行として読む（src/UpdaterParse.h の ValueOf）。
api_error() { # base message
	if [ -n "${VW_API_ERROR:-}" ]; then
		printf '%s理由: %s' "$1" "$VW_API_ERROR"
	else
		printf '%s' "$1"
	fi
}

# api_get: GitHub REST を 1 回叩く。取れたら 0 を返して JSON を VW_API_FILE へ、
# 取れなかったら 1 を返して理由を VW_API_ERROR へ置く。
api_get() { # api-subpath
	# --max-time bounds the request so the plug-in's periodic check can never
	# hang Vectorworks on a slow/unreachable network.
	VW_API_FILE=""
	VW_API_ERROR=""
	local f hdr token code rc
	f="$(mktemp)"; hdr="$(mktemp)"
	token=""
	if command -v resolve_token >/dev/null 2>&1; then
		token="$(resolve_token || true)"
	fi
	# **配列を使わない**（vw-token.sh 冒頭の理由）ので、付ける／付けないで curl の
	# 呼び出しを 2 つ書く。`-f` は使わない——HTTP の番号を自分で見たいから（`-f` だと
	# 本文もヘッダも捨てられ、403 が「なぜか失敗した」に潰れる）。
	rc=0
	if [ -n "$token" ]; then
		code="$(curl -sSL --max-time 20 --retry 2 -o "$f" -D "$hdr" -w '%{http_code}' \
			-H "Accept: application/vnd.github+json" \
			-H "Authorization: Bearer ${token}" "${VW_API}/$1" 2>/dev/null)" || rc=$?
	else
		code="$(curl -sSL --max-time 20 --retry 2 -o "$f" -D "$hdr" -w '%{http_code}' \
			-H "Accept: application/vnd.github+json" "${VW_API}/$1" 2>/dev/null)" || rc=$?
	fi

	if [ "$rc" -ne 0 ] || [ -z "${code:-}" ]; then
		VW_API_ERROR="$(curl_reason "$rc")"
		rm -f "$f" "$hdr"; return 1
	fi
	case "$code" in
		2*) rm -f "$hdr"; VW_API_FILE="$f"; return 0 ;;
	esac
	VW_API_ERROR="$(http_reason "$code" "$hdr" "$token")"
	rm -f "$f" "$hdr"; return 1
}

jval() { # json-file, keypath -> raw scalar value (empty if missing)
	plutil -extract "$2" raw -o - "$1" 2>/dev/null || true
}

# release_branch: リリース本文（notes）の "branch=<name>" 行を読む。公開時に CI が
# 必ず書いている（.github/workflows/build.yml）。
#
# **なぜ表示名では駄目か。** dev プレリリースの name は "Dev: <branch> (<sha>)" なので、
# 「いま動いているのと同じブランチか」の照合には使えない。取り込みコマンドのついでに
# 走る確認は、**同じブランチの新しいビルドだけ**を拾ってブランチ選択のダイアログを
# 出さずに済ませる（src/UpdaterFlow.cpp の RunDevUpdateCheckWith）ので、素のブランチ名が
# 要る。読めなければ空——プラグイン側はそのとき何もしない側へ倒れる。
release_branch() { # file, index-prefix -> branch name or ""
	jval "$1" "${2}.body" | sed -n 's/^branch=//p' | head -n 1 | tr -d '\r'
}

# find_asset_url: walk an assets array and return the browser_download_url of the
# first entry that matches.
#   file   the JSON file
#   prefix keypath of the assets array ("assets" for a single release object,
#          "<i>.assets" for element i of a releases array)
#   mode   "exact" (name equals value) or "suffix" (name ends with value)
#   value  what to match against
find_asset_url() { # file, prefix, mode, value
	local f="$1" pfx="$2" mode="$3" value="$4" j=0 nm hit
	while [ "$j" -lt 30 ]; do
		nm="$(jval "$f" "${pfx}.${j}.name")"
		[ -n "$nm" ] || break
		hit=0
		if [ "$mode" = "exact" ]; then
			[ "$nm" = "$value" ] && hit=1
		else
			case "$nm" in
				*"$value") hit=1 ;;
			esac
		fi
		if [ "$hit" -eq 1 ]; then
			jval "$f" "${pfx}.${j}.browser_download_url"
			return 0
		fi
		j=$((j + 1))
	done
	return 1
}

# asset_url: find the browser_download_url of an asset by exact name.
asset_url() { # file, prefix, want
	find_asset_url "$1" "$2" exact "$3"
}

# plugin_zip_url: the download URL of the plug-in's distribution zip.
#
# **名前が完全一致しなければ、末尾が "<VW_ZIP_SUFFIX>" のアセットで拾い直す。** この
# スクリプトはインストール済みの（＝古い）ものが走るので、アセット名を決め打ちにすると
# 名前が変わった瞬間にアップデートの経路そのものが途切れる（利用者は手で落とすしかなく
# なる）。1 つのリリースが持つ配布 zip はそのチャンネルの 1 つだけなので、末尾での照合で
# 取り違えは起きない。
plugin_zip_url() { # file, prefix, plugin-name
	find_asset_url "$1" "$2" exact "$3${VW_ZIP_SUFFIX}" ||
		find_asset_url "$1" "$2" suffix "${VW_ZIP_SUFFIX}"
}

download() { # url, out-file
	curl -fL --retry 3 "$1" -o "$2"
}

# ---------------------------------------------------------------------------
# Plug-in / Vectorworks helpers.
# ---------------------------------------------------------------------------

# plugin_dir: そのプラグインが持つフォルダ（`<Plug-Ins>/<name>/`）。**インストーラと
# 同じ規則**でなければならない（scripts/vw-install.sh の同名関数。片方だけ変えると、
# 入れた場所と読む場所が食い違う）。渡された先が既にそのフォルダなら足さない——
# プラグインは「いま自分が読み込まれたフォルダ」を渡してくるので、そこが既に
# `<Plug-Ins>/<name>` である。
plugin_dir() { # plugins-dir, name -> the plug-in's own folder
	local root="$1" name="$2"
	if [ "$(basename "$root")" = "$name" ]; then
		printf '%s' "$root"
	else
		printf '%s' "$root/$name"
	fi
}

# installed_bundle: インストール済みの殻のパス（上記のフォルダの中にある）。
installed_bundle() { # name -> path to <name>.vwlibrary
	printf '%s/%s.vwlibrary' "$(plugin_dir "$VW_PLUGINS_DIR" "$1")" "$1"
}

installed_commit() { # bundle-path -> stamped VWBuildCommit or "none"
	local plist="$1/Contents/Info.plist"
	if [ -f "$plist" ]; then
		/usr/libexec/PlistBuddy -c "Print :VWBuildCommit" "$plist" 2>/dev/null || echo "none"
	else
		echo "none"
	fi
}

# installed_branch: インストール済みのビルドが出たブランチ（Info.plist の VWBuildBranch）。
# **乗り換え先のブランチを覚えている唯一の場所**で、殻にコンパイルされた VW_BUILD_BRANCH は
# 本体だけを入れ替えたあと前のブランチを名乗ったままになる（src/UpdaterParse.h の
# ResolveCurrentDevBuild）。読めなければ空文字（プラグイン側はビルド一覧の sha 照合か、
# 最後に殻の値へ落ちる）。
installed_branch() { # bundle-path -> stamped VWBuildBranch or ""
	local plist="$1/Contents/Info.plist"
	if [ -f "$plist" ]; then
		/usr/libexec/PlistBuddy -c "Print :VWBuildBranch" "$plist" 2>/dev/null || echo ""
	else
		echo ""
	fi
}

# 殻（バンドル）の ID。**「アップデートに Vectorworks の再起動が要るか」を決める鍵**で、
# プラグイン側は自分にコンパイルされた VW_SHELL_ID と突き合わせる——一致するなら本体
# （.vwpayload）を読み直すだけで反映される（src/PayloadAbi.h / src/UpdaterParse.h）。
# 読めなければ空文字（＝判断できないので、プラグイン側は安全側＝「再起動が要る」へ倒す）。
installed_shell_id() { # bundle-path -> stamped VWShellId or ""
	local plist="$1/Contents/Info.plist"
	if [ -f "$plist" ]; then
		/usr/libexec/PlistBuddy -c "Print :VWShellId" "$plist" 2>/dev/null || echo ""
	else
		echo ""
	fi
}

# install_payload: 本体（"<name>.vwpayload"）を殻の隣へ入れる。**予備の配置の一部**で、
# 通常は zip 同梱の vw-install.sh がまとめて置く（下の run_zip_installer）。**殻とは別の
# ファイルで、これが入れ替わると Vectorworks を再起動しなくても次の操作から新しいコードが動く**
# （src/PayloadAbi.h）。zip に入っていなければ**何もしないで成功扱い**にする——古い形の
# リリース（本体を持たない）へ当たったときに、殻の入れ替えまで巻き添えで失敗させないため。
#
# 書き込みは必ず「別名へ書いてから mv」にする。走っている Vectorworks は入口ごとに
# このファイルを見て読み直すので、**途中まで書かれたファイルを掴ませない**ことが要る
# （src/PayloadSession.cpp）。
install_payload() { # work-dir, name -> 0 on success (or nothing to do)
	local work="$1" name="$2"
	local src="$work/$name.vwpayload"
	[ -f "$src" ] || return 0
	local dir
	dir="$(plugin_dir "$VW_PLUGINS_DIR" "$name")"

	# Gatekeeper: 殻と同じ手当て。dlopen する側なので、隔離属性が残っていると
	# Apple Silicon では読み込めない。
	xattr -d com.apple.quarantine "$src" 2>/dev/null || true
	codesign --force --sign - "$src" >/dev/null 2>&1 || true

	local dst="$dir/$name.vwpayload"
	rm -f "$dst.new"
	cp "$src" "$dst.new" || return 1
	mv -f "$dst.new" "$dst" || return 1
	return 0
}

# ---------------------------------------------------------------------------
# 配置は zip の中のインストーラへ委ねる（冒頭のコメント参照）。ここから下の自前の配置は
# **この仕組みより前のリリースへ当たったときだけ**使う予備。
# ---------------------------------------------------------------------------

# run_zip_installer: 展開済みの zip に入っている vw-install.sh へ配置を委ねる。委ねられた
# ら、その機械可読な出力（installed-shell= / ok / error=）をそのまま stdout へ流して 0 を
# 返す。委ねられなければ 1（呼び出し側は自前の配置へ落ちる）。
#
# 「委ねられた」の判定は**結末の行が返ってきたか**で行う——インストーラが古い／壊れて
# いて何も言わないときに、成功したと取り違えないため。
run_zip_installer() { # work-dir, name -> the installer's machine-readable output
	local work="$1" name="$2"
	local inst="$work/$VW_INSTALLER"
	[ -f "$inst" ] || return 1

	local out
	out="$(/bin/bash "$inst" --machine --from "$work" --name "$name" \
		--plugins-dir "$VW_PLUGINS_DIR" 2>/dev/null)" || return 1

	printf '%s\n' "$out" | grep -qE '^(ok$|error=)' || return 1
	printf '%s\n' "$out"
}

# installer_error_text: 機械可読な出力から error= の中身を取り出す（無ければ空）。
installer_error_text() { # machine-output
	printf '%s\n' "$1" | sed -n 's/^error=//p' | head -n 1
}

# install_zip: unzip a "<name>.vwlibrary.zip" and install it — first by handing the
# unpacked tree to the installer that came WITH it, otherwise (older releases) by
# the built-in de-quarantine / ad-hoc re-sign / atomic swap below.
install_zip() { # zip, name
	local zip="$1" name="$2"
	local work; work="$(mktemp -d)"
	unzip -q "$zip" -d "$work"

	local out
	if out="$(run_zip_installer "$work" "$name")"; then
		rm -rf "$work"
		local err; err="$(installer_error_text "$out")"
		[ -z "$err" ] || die "$err"
		echo "installed: $(installed_bundle "$name")"
		return 0
	fi

	local src="$work/$name.vwlibrary"
	[ -d "$src" ] || { rm -rf "$work"; die "$name.vwlibrary が zip 内に見つかりません。"; }

	# Gatekeeper: clear the download quarantine flag, then re-apply an ad-hoc
	# signature so Apple Silicon will load it even after unzip.
	xattr -dr com.apple.quarantine "$src" 2>/dev/null || true
	codesign --force --deep --sign - "$src" >/dev/null 2>&1 || true

	local dir
	dir="$(plugin_dir "$VW_PLUGINS_DIR" "$name")"
	mkdir -p "$dir"
	local dst="$dir/$name.vwlibrary"
	rm -rf "$dst.new"
	cp -R "$src" "$dst.new"
	rm -rf "$dst"
	mv "$dst.new" "$dst"
	install_payload "$work" "$name" || { rm -rf "$work"; die "本体（$name.vwpayload）のインストールに失敗しました。"; }
	rm -rf "$work"
	echo "installed: $dst"
}

# apply_choice: run the chosen action (skip / update only).
apply_choice() { # choice, zip, name
	local choice="$1" zip="$2" name="$3"
	case "$choice" in
		"更新だけ")
			install_zip "$zip" "$name"
			notify "みんなの構造設計支援 アップデート" "更新しました。反映するには Vectorworks を再起動してください。"
			;;
		*)
			echo "skipped."
			;;
	esac
}

# ---------------------------------------------------------------------------
# Channel flows.
# ---------------------------------------------------------------------------
update_stable() {
	api_get "releases/tags/stable" \
		|| die "$(api_error "安定版リリース (stable) を取得できませんでした。")"
	local f="$VW_API_FILE"
	local latest_full; latest_full="$(jval "$f" target_commitish)"
	local url; url="$(plugin_zip_url "$f" "assets" "min-nano_structure" || true)"
	rm -f "$f"
	[ -n "$latest_full" ] || die "安定版リリースの情報を取得できませんでした。"
	[ -n "$url" ] || die "安定版リリースに配布 zip (*.vwlibrary.zip) が見つかりません。"

	local latest="${latest_full:0:7}"
	local installed; installed="$(installed_commit "$(installed_bundle min-nano_structure)")"

	if [ "$installed" = "$latest" ]; then
		alert "みんなの構造設計支援 (stable)" "既に最新です（build ${installed}）。"
		return
	fi

	local choice; choice="$(ask2 "みんなの構造設計支援 (stable)" "新しい安定版ビルドがあります。
インストール済み: ${installed}
最新: ${latest}

どうしますか？")"
	[ "$choice" != "更新しない" ] || { echo "skipped."; return; }

	local tmp; tmp="$(mktemp -d)"
	download "$url" "$tmp/min-nano_structure.vwlibrary.zip" || die "安定版アセットのダウンロードに失敗しました。"
	apply_choice "$choice" "$tmp/min-nano_structure.vwlibrary.zip" "min-nano_structure"
	rm -rf "$tmp"
}

update_dev() {
	api_get "releases?per_page=100" \
		|| die "$(api_error "リリース一覧を取得できませんでした。")"
	local f="$VW_API_FILE"

	# Collect the per-branch dev prereleases.
	local names=() tags=() commits=() urls=()
	local i=0 tag name commit url
	while [ "$i" -lt 100 ]; do
		tag="$(jval "$f" "${i}.tag_name")"
		[ -n "$tag" ] || break
		case "$tag" in
			dev-*)
				name="$(jval "$f" "${i}.name")"
				commit="$(jval "$f" "${i}.target_commitish")"
				url="$(plugin_zip_url "$f" "${i}.assets" "min-nano_structureDev" || true)"
				[ -n "$name" ] || name="$tag"
				names+=("$name"); tags+=("$tag"); commits+=("$commit"); urls+=("$url")
				;;
		esac
		i=$((i + 1))
	done
	rm -f "$f"

	[ "${#tags[@]}" -gt 0 ] || die "開発版ビルド (dev-*) がまだありません。対象ブランチを push してビルドを走らせてください。"

	local chosen_name; chosen_name="$(choose_one "確認したい開発版ビルド（ブランチ）を選んでください:" "${names[@]}")"
	[ -n "$chosen_name" ] || { echo "cancelled."; return; }

	# Resolve the chosen entry.
	local idx=-1
	for i in "${!names[@]}"; do
		if [ "${names[$i]}" = "$chosen_name" ]; then idx="$i"; break; fi
	done
	[ "$idx" -ge 0 ] || die "選択したビルドを特定できませんでした。"

	local url2="${urls[$idx]}" latest="${commits[$idx]:0:7}"
	[ -n "$url2" ] || die "選択したビルドに配布 zip (*.vwlibrary.zip) が見つかりません。"
	local installed; installed="$(installed_commit "$(installed_bundle min-nano_structureDev)")"

	local same_note=""
	[ "$installed" = "$latest" ] && same_note="（このビルドは既にインストール済みです）
"

	local choice; choice="$(ask2 "みんなの構造設計支援 (dev)" "${chosen_name}
${same_note}インストール済み: ${installed}
選択したビルド: ${latest}

どうしますか？")"
	[ "$choice" != "更新しない" ] || { echo "skipped."; return; }

	local tmp; tmp="$(mktemp -d)"
	download "$url2" "$tmp/min-nano_structureDev.vwlibrary.zip" || die "開発版アセットのダウンロードに失敗しました。"
	apply_choice "$choice" "$tmp/min-nano_structureDev.vwlibrary.zip" "min-nano_structureDev"
	rm -rf "$tmp"
}

# ---------------------------------------------------------------------------
# Non-interactive, machine-readable back end for the plug-in. These print
# simple key=value / TSV lines to stdout and NEVER show a dialog — the plug-in
# parses them and shows its own native Vectorworks dialogs. Transient failures
# are reported as an "error=<message>" line (exit 0), so the plug-in stays in
# control of what (if anything) the user sees.
# ---------------------------------------------------------------------------

# q-stable: stable channel status.
#   installed=<commit|none>
#   latest=<commit>
#   url=<zip download url>
q_stable() {
	api_get "releases/tags/stable" \
		|| { echo "error=$(api_error "stable リリースを取得できませんでした。")"; return 0; }
	local f="$VW_API_FILE"
	local latest_full; latest_full="$(jval "$f" target_commitish)"
	local url; url="$(plugin_zip_url "$f" "assets" "min-nano_structure" || true)"
	rm -f "$f"
	if [ -z "$latest_full" ] || [ -z "$url" ]; then
		echo "error=stable リリースの情報が不完全です。"; return 0
	fi
	local installed; installed="$(installed_commit "$(installed_bundle min-nano_structure)")"
	echo "installed=${installed}"
	echo "latest=${latest_full:0:7}"
	echo "url=${url}"
}

# q-dev: installed dev build (commit + branch), then one line per downloadable
# dev build.
#   installed=<commit|none>
#   installed-branch=<branch>   （刻印が読めなければ空。プラグイン側は落としどころを持つ）
#   build<TAB>commit<TAB>name<TAB>url<TAB>branch
# branch は空のことがある（リリース本文に branch= が無い古いリリース）。プラグイン側の
# パーサはこの列が無い出力も読める（src/UpdaterParse.h の ParseDevBuilds）。
q_dev() {
	api_get "releases?per_page=100" \
		|| { echo "error=$(api_error "リリース一覧を取得できませんでした。")"; return 0; }
	local f="$VW_API_FILE"
	local bundle; bundle="$(installed_bundle min-nano_structureDev)"
	echo "installed=$(installed_commit "$bundle")"
	echo "installed-branch=$(installed_branch "$bundle")"

	local i=0 tag name commit url branch
	while [ "$i" -lt 100 ]; do
		tag="$(jval "$f" "${i}.tag_name")"
		[ -n "$tag" ] || break
		case "$tag" in
			dev-*)
				name="$(jval "$f" "${i}.name")"
				commit="$(jval "$f" "${i}.target_commitish")"
				url="$(plugin_zip_url "$f" "${i}.assets" "min-nano_structureDev" || true)"
				branch="$(release_branch "$f" "$i")"
				[ -n "$name" ] || name="$tag"
				# Only list builds that actually have a downloadable asset.
				[ -n "$url" ] && printf 'build\t%s\t%s\t%s\t%s\n' "${commit:0:7}" "$name" \
					"$url" "$branch"
				;;
		esac
		i=$((i + 1))
	done
	rm -f "$f"
}

# do-install <url> <name>: download and install "<name>.vwlibrary". Prints "ok"
# or "error=<message>". Self-contained (does not use install_zip's die(), which
# would show an osascript dialog) so the plug-in owns all UI.
do_install() {
	local url="$1" name="$2"
	if [ -z "$url" ] || [ -z "$name" ]; then
		echo "error=引数が不足しています。"
		return 0
	fi

	local tmp work; tmp="$(mktemp -d)"; work="$(mktemp -d)"
	if ! download "$url" "$tmp/$name.vwlibrary.zip"; then
		rm -rf "$tmp" "$work"; echo "error=ダウンロードに失敗しました。"; return 0
	fi
	if ! unzip -q "$tmp/$name.vwlibrary.zip" -d "$work" >/dev/null 2>&1; then
		rm -rf "$tmp" "$work"; echo "error=アーカイブの展開に失敗しました。"; return 0
	fi

	# **配置は落とした zip の中のインストーラへ委ねる**（冒頭のコメント／
	# run_zip_installer）。プラグインが読む契約（installed-shell= / ok / error=）は
	# インストーラ側が満たすので、その出力をそのまま流す。
	local out
	if out="$(run_zip_installer "$work" "$name")"; then
		rm -rf "$tmp" "$work"
		printf '%s\n' "$out"
		return 0
	fi

	# ここから下は予備——zip にインストーラが無い（この仕組みより前のリリース）とき
	# だけ通る。
	local src="$work/$name.vwlibrary"
	if [ ! -d "$src" ]; then
		rm -rf "$tmp" "$work"; echo "error=$name.vwlibrary が zip 内に見つかりません。"; return 0
	fi

	# Gatekeeper: clear the download quarantine flag, then re-apply an ad-hoc
	# signature so Apple Silicon will load it even after unzip.
	xattr -dr com.apple.quarantine "$src" 2>/dev/null || true
	codesign --force --deep --sign - "$src" >/dev/null 2>&1 || true

	local dir
	dir="$(plugin_dir "$VW_PLUGINS_DIR" "$name")"
	mkdir -p "$dir"
	local dst="$dir/$name.vwlibrary"
	rm -rf "$dst.new"
	if ! cp -R "$src" "$dst.new"; then
		rm -rf "$tmp" "$work" "$dst.new"; echo "error=インストール先へのコピーに失敗しました。"; return 0
	fi
	rm -rf "$dst"
	mv "$dst.new" "$dst"
	if ! install_payload "$work" "$name"; then
		rm -rf "$tmp" "$work"; echo "error=本体（$name.vwpayload）のインストールに失敗しました。"; return 0
	fi
	rm -rf "$tmp" "$work"
	# **いま入れた殻の ID を先に出す。** プラグインはこれを自分の VW_SHELL_ID と突き合わせて
	# 「再起動が要るか／本体の読み直しで済むか」を決める（src/UpdaterParse.h の
	# NeedsRestartAfterInstall）。読めなければ行を出さない＝プラグインは安全側へ倒す。
	local shell_id; shell_id="$(installed_shell_id "$dst")"
	if [ -n "$shell_id" ]; then
		echo "installed-shell=$shell_id"
	fi
	echo "ok"
}

# ---------------------------------------------------------------------------
main() {
	command -v curl >/dev/null 2>&1 || die "curl が見つかりません（macOS で実行してください）。"

	local channel="${1:-}"
	if [ -z "$channel" ]; then
		channel="$(choose_one "どのビルドを確認しますか？" "stable（安定版 / main）" "dev（開発版 / ブランチ選択）")"
		case "$channel" in
			stable*) channel="stable" ;;
			dev*)    channel="dev" ;;
			*)       echo "cancelled."; exit 0 ;;
		esac
	fi

	case "$channel" in
		stable)     update_stable ;;
		dev)        update_dev ;;
		q-stable)   q_stable ;;
		q-dev)      q_dev ;;
		do-install) do_install "${2:-}" "${3:-}" ;;
		*)      die "不明なチャンネル: '$channel'（stable / dev / q-stable / q-dev / do-install）。" ;;
	esac
}

# Run main only when executed directly (./vw-update.sh …), NOT when this file is
# sourced. The plug-in and the manual/terminal use both EXECUTE the script, so
# they are unaffected: run directly, $0 equals BASH_SOURCE[0] and main runs. The
# unit tests (tests/vw-update.test.sh) SOURCE the file instead, to call the pure
# back-end functions (asset_url / q_stable / q_dev / do_install) with curl/plutil
# stubbed out — there BASH_SOURCE[0] != $0, so main does not run. This is the
# shell analogue of the IUpdaterHost seam that makes UpdaterFlow.cpp testable.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
