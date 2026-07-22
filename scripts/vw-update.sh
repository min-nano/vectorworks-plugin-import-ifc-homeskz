#!/usr/bin/env bash
#
# vw-update.sh — download the latest CI build of the SamplePlugin Vectorworks plug-in
# and install it into your Vectorworks 2026 Plug-Ins folder.
#
# Two channels, two separately-named plug-ins that can be installed at once:
#
#   stable  -> "SamplePlugin.vwlibrary"     from the rolling "stable" release (main).
#   dev     -> "SamplePluginDev.vwlibrary"  from a per-branch "dev-<branch>" prerelease;
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
#   q-dev               Print installed=<commit|none> then one TSV line per dev
#                       build: "build<TAB>commit<TAB>name<TAB>url"
#                       (or error=<message>).
#   do-install <url> <name>   Download+install <name>.vwlibrary; print "ok" or
#                             error=<message>. No dialogs.
#
# The interactive stable/dev modes below are the manual, run-from-a-terminal
# fallback and keep using macOS (osascript) dialogs.
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

VW_REPO="${VW_REPO:-min-nano/vectorworks-plugin-native-template}"
VW_PLUGINS_DIR="${VW_PLUGINS_DIR:-$HOME/Library/Application Support/Vectorworks/2026/Plug-Ins}"
VW_API="https://api.github.com/repos/${VW_REPO}"

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
	alert "SamplePlugin アップデート" "エラー: $1"
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
# GitHub REST helpers (public repo -> unauthenticated). JSON is parsed with
# plutil, which ships with macOS and reads JSON natively.
# ---------------------------------------------------------------------------
api_get() { # api-subpath -> path to a temp file holding the JSON, or fail
	# --max-time bounds the request so the plug-in's start-up check can never
	# hang Vectorworks on a slow/unreachable network.
	local f; f="$(mktemp)"
	if curl -fsSL --max-time 20 --retry 2 -H "Accept: application/vnd.github+json" "${VW_API}/$1" -o "$f"; then
		printf '%s' "$f"
	else
		rm -f "$f"; return 1
	fi
}

jval() { # json-file, keypath -> raw scalar value (empty if missing)
	plutil -extract "$2" raw -o - "$1" 2>/dev/null || true
}

# asset_url: find the browser_download_url of an asset by name.
#   file   the JSON file
#   prefix keypath of the assets array ("assets" for a single release object,
#          "<i>.assets" for element i of a releases array)
#   want   the asset file name to match
asset_url() { # file, prefix, want
	local f="$1" pfx="$2" want="$3" j=0 nm
	while [ "$j" -lt 30 ]; do
		nm="$(jval "$f" "${pfx}.${j}.name")"
		[ -n "$nm" ] || break
		if [ "$nm" = "$want" ]; then
			jval "$f" "${pfx}.${j}.browser_download_url"
			return 0
		fi
		j=$((j + 1))
	done
	return 1
}

download() { # url, out-file
	curl -fL --retry 3 "$1" -o "$2"
}

# ---------------------------------------------------------------------------
# Plug-in / Vectorworks helpers.
# ---------------------------------------------------------------------------
installed_commit() { # bundle-path -> stamped VWBuildCommit or "none"
	local plist="$1/Contents/Info.plist"
	if [ -f "$plist" ]; then
		/usr/libexec/PlistBuddy -c "Print :VWBuildCommit" "$plist" 2>/dev/null || echo "none"
	else
		echo "none"
	fi
}

# install_zip: unzip a "<name>.vwlibrary.zip", de-quarantine, ad-hoc re-sign and
# atomically swap it into the Plug-Ins folder.
install_zip() { # zip, name
	local zip="$1" name="$2"
	local work; work="$(mktemp -d)"
	unzip -q "$zip" -d "$work"
	local src="$work/$name.vwlibrary"
	[ -d "$src" ] || { rm -rf "$work"; die "$name.vwlibrary が zip 内に見つかりません。"; }

	# Gatekeeper: clear the download quarantine flag, then re-apply an ad-hoc
	# signature so Apple Silicon will load it even after unzip.
	xattr -dr com.apple.quarantine "$src" 2>/dev/null || true
	codesign --force --deep --sign - "$src" >/dev/null 2>&1 || true

	mkdir -p "$VW_PLUGINS_DIR"
	local dst="$VW_PLUGINS_DIR/$name.vwlibrary"
	rm -rf "$dst.new"
	cp -R "$src" "$dst.new"
	rm -rf "$dst"
	mv "$dst.new" "$dst"
	rm -rf "$work"
	echo "installed: $dst"
}

# apply_choice: run the chosen action (skip / update only).
apply_choice() { # choice, zip, name
	local choice="$1" zip="$2" name="$3"
	case "$choice" in
		"更新だけ")
			install_zip "$zip" "$name"
			notify "SamplePlugin アップデート" "更新しました。反映するには Vectorworks を再起動してください。"
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
	local f; f="$(api_get "releases/tags/stable")" \
		|| die "安定版リリース (stable) が見つかりません。main のビルドが完了しているか確認してください。"
	local latest_full; latest_full="$(jval "$f" target_commitish)"
	local url; url="$(asset_url "$f" "assets" "SamplePlugin.vwlibrary.zip" || true)"
	rm -f "$f"
	[ -n "$latest_full" ] || die "安定版リリースの情報を取得できませんでした。"
	[ -n "$url" ] || die "安定版のアセット (SamplePlugin.vwlibrary.zip) が見つかりません。"

	local latest="${latest_full:0:7}"
	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/SamplePlugin.vwlibrary")"

	if [ "$installed" = "$latest" ]; then
		alert "SamplePlugin (stable)" "既に最新です（build ${installed}）。"
		return
	fi

	local choice; choice="$(ask2 "SamplePlugin (stable)" "新しい安定版ビルドがあります。
インストール済み: ${installed}
最新: ${latest}

どうしますか？")"
	[ "$choice" != "更新しない" ] || { echo "skipped."; return; }

	local tmp; tmp="$(mktemp -d)"
	download "$url" "$tmp/SamplePlugin.vwlibrary.zip" || die "安定版アセットのダウンロードに失敗しました。"
	apply_choice "$choice" "$tmp/SamplePlugin.vwlibrary.zip" "SamplePlugin"
	rm -rf "$tmp"
}

update_dev() {
	local f; f="$(api_get "releases?per_page=100")" \
		|| die "リリース一覧を取得できませんでした。"

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
				url="$(asset_url "$f" "${i}.assets" "SamplePluginDev.vwlibrary.zip" || true)"
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
	[ -n "$url2" ] || die "選択したビルドのアセット (SamplePluginDev.vwlibrary.zip) が見つかりません。"
	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/SamplePluginDev.vwlibrary")"

	local same_note=""
	[ "$installed" = "$latest" ] && same_note="（このビルドは既にインストール済みです）
"

	local choice; choice="$(ask2 "SamplePlugin (dev)" "${chosen_name}
${same_note}インストール済み: ${installed}
選択したビルド: ${latest}

どうしますか？")"
	[ "$choice" != "更新しない" ] || { echo "skipped."; return; }

	local tmp; tmp="$(mktemp -d)"
	download "$url2" "$tmp/SamplePluginDev.vwlibrary.zip" || die "開発版アセットのダウンロードに失敗しました。"
	apply_choice "$choice" "$tmp/SamplePluginDev.vwlibrary.zip" "SamplePluginDev"
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
	local f; f="$(api_get "releases/tags/stable")" \
		|| { echo "error=stable リリースを取得できませんでした。"; return 0; }
	local latest_full; latest_full="$(jval "$f" target_commitish)"
	local url; url="$(asset_url "$f" "assets" "SamplePlugin.vwlibrary.zip" || true)"
	rm -f "$f"
	if [ -z "$latest_full" ] || [ -z "$url" ]; then
		echo "error=stable リリースの情報が不完全です。"; return 0
	fi
	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/SamplePlugin.vwlibrary")"
	echo "installed=${installed}"
	echo "latest=${latest_full:0:7}"
	echo "url=${url}"
}

# q-dev: installed dev commit, then one line per downloadable dev build.
#   installed=<commit|none>
#   build<TAB>commit<TAB>name<TAB>url
q_dev() {
	local f; f="$(api_get "releases?per_page=100")" \
		|| { echo "error=リリース一覧を取得できませんでした。"; return 0; }
	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/SamplePluginDev.vwlibrary")"
	echo "installed=${installed}"

	local i=0 tag name commit url
	while [ "$i" -lt 100 ]; do
		tag="$(jval "$f" "${i}.tag_name")"
		[ -n "$tag" ] || break
		case "$tag" in
			dev-*)
				name="$(jval "$f" "${i}.name")"
				commit="$(jval "$f" "${i}.target_commitish")"
				url="$(asset_url "$f" "${i}.assets" "SamplePluginDev.vwlibrary.zip" || true)"
				[ -n "$name" ] || name="$tag"
				# Only list builds that actually have a downloadable asset.
				[ -n "$url" ] && printf 'build\t%s\t%s\t%s\n' "${commit:0:7}" "$name" "$url"
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
	[ -n "$url" ] && [ -n "$name" ] || { echo "error=引数が不足しています。"; return 0; }

	local tmp work; tmp="$(mktemp -d)"; work="$(mktemp -d)"
	if ! download "$url" "$tmp/$name.vwlibrary.zip"; then
		rm -rf "$tmp" "$work"; echo "error=ダウンロードに失敗しました。"; return 0
	fi
	if ! unzip -q "$tmp/$name.vwlibrary.zip" -d "$work" >/dev/null 2>&1; then
		rm -rf "$tmp" "$work"; echo "error=アーカイブの展開に失敗しました。"; return 0
	fi
	local src="$work/$name.vwlibrary"
	if [ ! -d "$src" ]; then
		rm -rf "$tmp" "$work"; echo "error=$name.vwlibrary が zip 内に見つかりません。"; return 0
	fi

	# Gatekeeper: clear the download quarantine flag, then re-apply an ad-hoc
	# signature so Apple Silicon will load it even after unzip.
	xattr -dr com.apple.quarantine "$src" 2>/dev/null || true
	codesign --force --deep --sign - "$src" >/dev/null 2>&1 || true

	mkdir -p "$VW_PLUGINS_DIR"
	local dst="$VW_PLUGINS_DIR/$name.vwlibrary"
	rm -rf "$dst.new"
	if ! cp -R "$src" "$dst.new"; then
		rm -rf "$tmp" "$work" "$dst.new"; echo "error=インストール先へのコピーに失敗しました。"; return 0
	fi
	rm -rf "$dst"
	mv "$dst.new" "$dst"
	rm -rf "$tmp" "$work"
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

main "$@"
