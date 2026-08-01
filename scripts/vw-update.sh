#!/usr/bin/env bash
#
# vw-update.sh — download the latest CI build of the HomeskzIfcImport Vectorworks plug-in
# and install it into your Vectorworks 2026 Plug-Ins folder.
#
# Builds are distributed from a Cloudflare R2 bucket, NOT from GitHub Releases:
# reading (pre)release assets through the GitHub API proved unreliable from the
# client side, so the bucket serves the zips plus small JSON manifests over a
# public base URL, and this script reads only those. The bucket layout and the
# manifest schema are documented in scripts/r2-publish.sh (the CI-side writer).
# The GitHub Releases still exist, but only as a human-facing record linking here.
#
# Two channels, two separately-named plug-ins that can be installed at once:
#
#   stable  -> "HomeskzIfcImport.vwlibrary"     from "stable/manifest.json" (main).
#   dev     -> "HomeskzIfcImportDev.vwlibrary"  from "dev/index.json", which lists one
#              entry per branch; you pick which branch's build to install.
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
# bucket is served publicly, no authentication.
#
# Overridable via environment:
#   VW_BASE_URL     distribution base URL   (baked in at build time, see below)
#   VW_PLUGINS_DIR  Vectorworks Plug-Ins    (default: user folder for VW 2026)
#
set -euo pipefail

# The distribution base URL is CONFIGURATION, not source: CMake substitutes it
# into the copy of this script that ships inside the plug-in (see
# VW_UPDATE_BASE_URL in CMakeLists.txt). The copy in the repository still holds
# the unexpanded placeholder — detect that and treat it as "not configured", so
# a manual run can supply the URL through VW_BASE_URL instead.
VW_BASE_URL_DEFAULT='@VW_UPDATE_BASE_URL@'
case "$VW_BASE_URL_DEFAULT" in
	'@'*) VW_BASE_URL_DEFAULT='' ;;
esac
VW_BASE_URL="${VW_BASE_URL:-$VW_BASE_URL_DEFAULT}"
# Strip a trailing slash so joining never produces "…//key".
VW_BASE_URL="${VW_BASE_URL%/}"

VW_PLUGINS_DIR="${VW_PLUGINS_DIR:-$HOME/Library/Application Support/Vectorworks/2026/Plug-Ins}"

# The two manifests this script reads (relative to VW_BASE_URL).
VW_STABLE_MANIFEST="stable/manifest.json"
VW_DEV_INDEX="dev/index.json"

# Shown when VW_BASE_URL is not configured. Kept in one place because both the
# interactive (dialog) and the machine-readable (error=) paths use it.
VW_NO_BASE_MSG="配布先 URL が設定されていません（環境変数 VW_BASE_URL）。"

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
	alert "HomeskzIfcImport アップデート" "エラー: $1"
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
# Distribution helpers. The bucket is public, so these are plain unauthenticated
# GETs. JSON is parsed with plutil, which ships with macOS and reads JSON
# natively.
# ---------------------------------------------------------------------------

# True when a distribution base URL is available at all.
have_base_url() { [ -n "$VW_BASE_URL" ]; }

fetch_json() { # object key -> path to a temp file holding the JSON, or fail
	have_base_url || return 1
	# --max-time bounds the request so the plug-in's start-up check can never
	# hang Vectorworks on a slow/unreachable network.
	#
	# The manifests are uploaded with "Cache-Control: no-cache", but a stale copy
	# handed back by an intermediate cache is exactly the kind of flakiness this
	# move away from the GitHub API was meant to end — so ask for a fresh copy
	# AND append a cache-busting query. (The zips are immutable per commit, so
	# only these small JSON objects need it.)
	local f
	f="$(mktemp)"
	if curl -fsSL --max-time 20 --retry 2 -H "Cache-Control: no-cache" \
		"${VW_BASE_URL}/$1?ts=$(date +%s)" -o "$f"; then
		printf '%s' "$f"
	else
		rm -f "$f"
		return 1
	fi
}

jval() { # json-file, keypath -> raw scalar value (empty if missing)
	plutil -extract "$2" raw -o - "$1" 2>/dev/null || true
}

# short_of: the 7-char build id a manifest entry identifies itself by. Prefer the
# manifest's own "short" field and fall back to truncating "commit", so a
# manifest written by an older publisher still resolves.
short_of() { # json-file, prefix ("" for a manifest, "builds.<i>." for an index entry)
	local s
	s="$(jval "$1" "${2}short")"
	if [ -z "$s" ]; then
		s="$(jval "$1" "${2}commit")"
		s="${s:0:7}"
	fi
	printf '%s' "$s"
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
			notify "HomeskzIfcImport アップデート" "更新しました。反映するには Vectorworks を再起動してください。"
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
	have_base_url || die "$VW_NO_BASE_MSG"
	local f; f="$(fetch_json "$VW_STABLE_MANIFEST")" \
		|| die "安定版のマニフェストを取得できませんでした。main のビルドが完了しているか確認してください。"
	local latest; latest="$(short_of "$f" "")"
	local url; url="$(jval "$f" mac)"
	rm -f "$f"
	[ -n "$latest" ] || die "安定版マニフェストの情報を取得できませんでした。"
	[ -n "$url" ] || die "安定版の macOS アセット (mac) がマニフェストにありません。"

	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/HomeskzIfcImport.vwlibrary")"

	if [ "$installed" = "$latest" ]; then
		alert "HomeskzIfcImport (stable)" "既に最新です（build ${installed}）。"
		return
	fi

	local choice; choice="$(ask2 "HomeskzIfcImport (stable)" "新しい安定版ビルドがあります。
インストール済み: ${installed}
最新: ${latest}

どうしますか？")"
	[ "$choice" != "更新しない" ] || { echo "skipped."; return; }

	local tmp; tmp="$(mktemp -d)"
	download "$url" "$tmp/HomeskzIfcImport.vwlibrary.zip" || die "安定版アセットのダウンロードに失敗しました。"
	apply_choice "$choice" "$tmp/HomeskzIfcImport.vwlibrary.zip" "HomeskzIfcImport"
	rm -rf "$tmp"
}

update_dev() {
	have_base_url || die "$VW_NO_BASE_MSG"
	local f; f="$(fetch_json "$VW_DEV_INDEX")" \
		|| die "開発版ビルドの一覧を取得できませんでした。"

	# Collect the per-branch dev builds listed in the index.
	local names=() commits=() urls=()
	local i=0 branch commit url
	while [ "$i" -lt 200 ]; do
		commit="$(jval "$f" "builds.${i}.commit")"
		[ -n "$commit" ] || break
		branch="$(jval "$f" "builds.${i}.branch")"
		url="$(jval "$f" "builds.${i}.mac")"
		[ -n "$branch" ] || branch="$(jval "$f" "builds.${i}.slug")"
		names+=("$branch"); commits+=("$(short_of "$f" "builds.${i}.")"); urls+=("$url")
		i=$((i + 1))
	done
	rm -f "$f"

	[ "${#names[@]}" -gt 0 ] || die "開発版ビルドがまだありません。対象ブランチを push してビルドを走らせてください。"

	local chosen_name; chosen_name="$(choose_one "確認したい開発版ビルド（ブランチ）を選んでください:" "${names[@]}")"
	[ -n "$chosen_name" ] || { echo "cancelled."; return; }

	# Resolve the chosen entry.
	local idx=-1
	for i in "${!names[@]}"; do
		if [ "${names[$i]}" = "$chosen_name" ]; then idx="$i"; break; fi
	done
	[ "$idx" -ge 0 ] || die "選択したビルドを特定できませんでした。"

	local url2="${urls[$idx]}" latest="${commits[$idx]}"
	[ -n "$url2" ] || die "選択したビルドの macOS アセット (mac) がマニフェストにありません。"
	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/HomeskzIfcImportDev.vwlibrary")"

	local same_note=""
	[ "$installed" = "$latest" ] && same_note="（このビルドは既にインストール済みです）
"

	local choice; choice="$(ask2 "HomeskzIfcImport (dev)" "${chosen_name}
${same_note}インストール済み: ${installed}
選択したビルド: ${latest}

どうしますか？")"
	[ "$choice" != "更新しない" ] || { echo "skipped."; return; }

	local tmp; tmp="$(mktemp -d)"
	download "$url2" "$tmp/HomeskzIfcImportDev.vwlibrary.zip" || die "開発版アセットのダウンロードに失敗しました。"
	apply_choice "$choice" "$tmp/HomeskzIfcImportDev.vwlibrary.zip" "HomeskzIfcImportDev"
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
	if ! have_base_url; then
		echo "error=${VW_NO_BASE_MSG}"; return 0
	fi
	local f; f="$(fetch_json "$VW_STABLE_MANIFEST")" \
		|| { echo "error=stable のマニフェストを取得できませんでした。"; return 0; }
	local latest; latest="$(short_of "$f" "")"
	local url; url="$(jval "$f" mac)"
	rm -f "$f"
	if [ -z "$latest" ] || [ -z "$url" ]; then
		echo "error=stable のマニフェストの内容が不完全です。"; return 0
	fi
	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/HomeskzIfcImport.vwlibrary")"
	echo "installed=${installed}"
	echo "latest=${latest}"
	echo "url=${url}"
}

# q-dev: installed dev commit, then one line per downloadable dev build.
#   installed=<commit|none>
#   build<TAB>commit<TAB>name<TAB>url
q_dev() {
	if ! have_base_url; then
		echo "error=${VW_NO_BASE_MSG}"; return 0
	fi
	local f; f="$(fetch_json "$VW_DEV_INDEX")" \
		|| { echo "error=開発版ビルドの一覧を取得できませんでした。"; return 0; }
	local installed; installed="$(installed_commit "$VW_PLUGINS_DIR/HomeskzIfcImportDev.vwlibrary")"
	echo "installed=${installed}"

	local i=0 branch commit short url
	while [ "$i" -lt 200 ]; do
		commit="$(jval "$f" "builds.${i}.commit")"
		[ -n "$commit" ] || break
		branch="$(jval "$f" "builds.${i}.branch")"
		short="$(short_of "$f" "builds.${i}.")"
		url="$(jval "$f" "builds.${i}.mac")"
		[ -n "$branch" ] || branch="$(jval "$f" "builds.${i}.slug")"
		# Only list builds that actually have a downloadable asset.
		if [ -n "$url" ] && [ -n "$short" ]; then
			printf 'build\t%s\t%s\t%s\n' "$short" "$branch" "$url"
		fi
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

# Run main only when executed directly (./vw-update.sh …), NOT when this file is
# sourced. The plug-in and the manual/terminal use both EXECUTE the script, so
# they are unaffected: run directly, $0 equals BASH_SOURCE[0] and main runs. The
# unit tests (tests/vw-update.test.sh) SOURCE the file instead, to call the pure
# back-end functions (short_of / q_stable / q_dev / do_install) with curl/plutil
# stubbed out — there BASH_SOURCE[0] != $0, so main does not run. This is the
# shell analogue of the IUpdaterHost seam that makes UpdaterFlow.cpp testable.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
