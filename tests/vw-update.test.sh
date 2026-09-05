#!/usr/bin/env bash
#
#	vw-update.test.sh
#
#	Unit tests for the macOS updater back end (scripts/vw-update.sh). They cover
#	the machine-readable modes the plug-in actually drives — q-stable, q-dev and
#	do-install — plus the helpers they build on (asset_url, installed_commit).
#
#	The script is SOURCED (its `main` is guarded, see the tail of vw-update.sh),
#	so the real functions run in-process and we override just their outermost
#	I/O leaves with fakes:
#
#	  * jval        the JSON boundary. Real jval shells out to `plutil`, which is
#	                macOS-only; here it is emulated with python3 so the REAL
#	                asset_url / q_stable / q_dev logic runs against JSON fixtures
#	                on a plain Linux runner. (This mirrors the C++ tests, which
#	                also run SDK-free on Linux — see tests/README.md.)
#	  * api_get     returns a fixture instead of hitting the GitHub REST API.
#	  * download    copies a local fixture zip instead of downloading one.
#	  * installed_commit  returns a canned commit (real one needs macOS
#	                      PlistBuddy); its "no bundle -> none" branch is still
#	                      exercised directly below.
#
#	This is the shell counterpart to the C++ IUpdaterHost fake in
#	tests/UpdaterFlowTests.cpp: the script is the unit, the stubs are the test
#	doubles. It is NOT an end-to-end test (that would run the real script on a
#	Mac against the live GitHub API).
#
#	The macOS-only surface — osascript dialogs, codesign/xattr re-signing,
#	PlistBuddy — is inherently unrunnable off a Mac and is left to manual /
#	end-to-end testing, exactly as the C++ side leaves the dladdr/gSDK glue.
#

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/../scripts/vw-update.sh"

# ---------------------------------------------------------------------------
# Missing-tool policy. On a developer box a missing helper skips gracefully
# (exit 0) so `ctest` stays green without the macOS back-end deps installed.
# In CI, though, a silent skip would let the suite "pass" without ever running
# a single check — so when VW_REQUIRE_SCRIPT_TESTS is set (the Tests workflow
# sets it), a missing tool is a HARD FAILURE instead. Common falsy spellings
# count as unset so `VW_REQUIRE_SCRIPT_TESTS=0` still means "skip is OK".
# ---------------------------------------------------------------------------
REQUIRE_TOOLS="${VW_REQUIRE_SCRIPT_TESTS:-}"
case "$REQUIRE_TOOLS" in
	'' | 0 | off | OFF | false | FALSE | no | NO) REQUIRE_TOOLS="" ;;
esac

# skip_or_fail <reason> — SKIP (exit 0) locally, ERROR (exit 1) when tests are
# required, so a missing dependency can never masquerade as a passing run in CI.
skip_or_fail() {
	if [ -n "$REQUIRE_TOOLS" ]; then
		echo "ERROR vw-update.test.sh: $1 (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)." >&2
		exit 1
	fi
	echo "SKIP vw-update.test.sh: $1."
	exit 0
}

for tool in python3 unzip zip; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		skip_or_fail "'$tool' not found (macOS updater back-end tests need it)"
	fi
done
if [ ! -f "$SCRIPT" ]; then
	skip_or_fail "$SCRIPT not found"
fi

# ---------------------------------------------------------------------------
# Tiny assertion harness, styled after tests/TestFramework.h (t / check_eq /
# check_contains, a summary line, non-zero exit on any failure).
# ---------------------------------------------------------------------------
TESTS_RUN=0
TESTS_FAILED=0
CURRENT="(none)"

t() { CURRENT="$1"; }

check_eq() { # actual expected [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	if [ "$1" != "$2" ]; then
		TESTS_FAILED=$((TESTS_FAILED + 1))
		printf 'FAIL [%s] %s\n  expected: %s\n  actual:   %s\n' \
			"$CURRENT" "${3:-values differ}" "$2" "$1"
	fi
}

check_contains() { # haystack needle [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	case "$1" in
		*"$2"*) : ;;
		*)
			TESTS_FAILED=$((TESTS_FAILED + 1))
			printf 'FAIL [%s] %s\n  missing:  %s\n  in:       %s\n' \
				"$CURRENT" "${3:-substring not found}" "$2" "$1"
			;;
	esac
}

check_not_contains() { # haystack needle [label]
	TESTS_RUN=$((TESTS_RUN + 1))
	case "$1" in
		*"$2"*)
			TESTS_FAILED=$((TESTS_FAILED + 1))
			printf 'FAIL [%s] %s\n  unexpected: %s\n  in:         %s\n' \
				"$CURRENT" "${3:-substring present}" "$2" "$1"
			;;
		*) : ;;
	esac
}

# ---------------------------------------------------------------------------
# Load the script under test, then install the fakes. Sourcing turns on the
# script's own `set -euo pipefail`; relax it here so a stubbed non-zero return
# cannot abort the harness. Each function under test is still invoked inside a
# `set -euo pipefail` subshell (RUN, below) so its real behaviour is preserved.
# ---------------------------------------------------------------------------
# shellcheck source=/dev/null
source "$SCRIPT"
set +e +u +o pipefail

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- Fakes ----------------------------------------------------------------
# jval: emulate `plutil -extract <keypath> raw -o - <file>` for the dotted,
# possibly-indexed keypaths the script uses (e.g. "assets.0.name"). Always
# exits 0 (prints nothing when the key is absent), exactly like the real jval's
# trailing `|| true`, so a missing key never trips the caller's `set -e`.
jval() { # json-file, keypath
	python3 - "$1" "$2" <<'PY' || true
import json, sys
path, keypath = sys.argv[1], sys.argv[2]
try:
    with open(path) as fh:
        node = json.load(fh)
    for part in keypath.split('.'):
        node = node[int(part)] if isinstance(node, list) else node[part]
    if isinstance(node, bool):
        print('true' if node else 'false')
    elif node is None:
        sys.exit(1)
    else:
        print(node)
except Exception:
    sys.exit(1)
PY
}

# api_get: copy the fixture selected for this endpoint into a fresh temp file and
# echo its path — the real api_get returns a temp file the caller then `rm`s, so
# we must NOT hand back the fixture itself. VW_TEST_API_FAIL simulates offline.
api_get() { # api-subpath
	[ -n "${VW_TEST_API_FAIL:-}" ] && return 1
	local fixture=""
	case "$1" in
		releases/tags/stable) fixture="${VW_TEST_STABLE_JSON:-}" ;;
		releases*) fixture="${VW_TEST_RELEASES_JSON:-}" ;;
	esac
	[ -n "$fixture" ] && [ -f "$fixture" ] || return 1
	local f
	f="$(mktemp)"
	cp "$fixture" "$f"
	printf '%s' "$f"
}

# download: copy a local fixture zip to the requested output path. VW_TEST_DL_FAIL
# simulates a failed download.
download() { # url, out-file
	[ -n "${VW_TEST_DL_FAIL:-}" ] && return 1
	[ -n "${VW_TEST_DL_ZIP:-}" ] && [ -f "$VW_TEST_DL_ZIP" ] || return 1
	cp "$VW_TEST_DL_ZIP" "$2"
}

# installed_commit: the real one reads Info.plist via macOS PlistBuddy; return a
# canned value here (VW_TEST_INSTALLED, default "none"). The genuine
# "no bundle -> none" branch is unstubbed and tested separately below.
installed_commit() { printf '%s\n' "${VW_TEST_INSTALLED:-none}"; }

# installed_shell_id: 同上（実物はインストール済みバンドルの Info.plist から VWShellId を
# PlistBuddy で読む）。既定は空＝「殻の新旧を判断できない」で、do_install はその行を
# 出さない（src/UpdaterParse.h の NeedsRestartAfterInstall は安全側＝再起動へ倒れる）。
installed_shell_id() { printf '%s\n' "${VW_TEST_SHELL_ID:-}"; }

# codesign / xattr are macOS Gatekeeper tools do_install calls; no-op them so the
# install path runs cleanly (and quietly) on Linux.
codesign() { return 0; }
xattr() { return 0; }

# RUN: invoke a script function in a faithful `set -euo pipefail` subshell and
# capture its stdout. The subshell inherits the fakes defined above.
RUN() { ( set -euo pipefail; "$@" ); }

# --- Fixtures -------------------------------------------------------------
STABLE_JSON="$WORK/stable.json"
cat >"$STABLE_JSON" <<'JSON'
{
  "target_commitish": "abc1234def5678",
  "assets": [
    { "name": "HomeskzIfcImport.vwlibrary.zip",
      "browser_download_url": "https://example.test/dl/HomeskzIfcImport.vwlibrary.zip" },
    { "name": "notes.txt",
      "browser_download_url": "https://example.test/dl/notes.txt" }
  ]
}
JSON

RELEASES_JSON="$WORK/releases.json"
cat >"$RELEASES_JSON" <<'JSON'
[
  { "tag_name": "stable", "name": "stable", "target_commitish": "zzz9999",
    "assets": [ { "name": "HomeskzIfcImport.vwlibrary.zip",
                  "browser_download_url": "https://example.test/dl/stable.zip" } ] },
  { "tag_name": "dev-feature-x", "name": "feature/x", "target_commitish": "aaa1111ccc",
    "assets": [ { "name": "HomeskzIfcImportDev.vwlibrary.zip",
                  "browser_download_url": "https://example.test/dl/x.zip" } ] },
  { "tag_name": "dev-feature-y", "name": "feature/y", "target_commitish": "bbb2222ddd",
    "assets": [ { "name": "HomeskzIfcImportDev.vwlibrary.zip",
                  "browser_download_url": "https://example.test/dl/y.zip" } ] },
  { "tag_name": "dev-nobuild", "name": "feature/z", "target_commitish": "ccc3333eee",
    "assets": [ { "name": "unrelated.zip",
                  "browser_download_url": "https://example.test/dl/z.zip" } ] }
]
JSON

export VW_TEST_STABLE_JSON="$STABLE_JSON"
export VW_TEST_RELEASES_JSON="$RELEASES_JSON"

# Build a real "HomeskzIfcImportDev.vwlibrary.zip" for the do-install tests, and a
# malformed one whose top-level dir has the wrong name.
# 本体（"<name>.vwpayload"）も一緒に入れる——**実際のリリース zip と同じ形**にしないと、
# 殻だけ入れて本体を取りこぼす退行を捕まえられない（src/PayloadAbi.h）。
build_zip() { # zip-path, bundle-dir-name
	local dir="$WORK/stage-$$-$RANDOM"
	local name="${2%.vwlibrary}"
	mkdir -p "$dir/$2/Contents"
	printf 'plist\n' >"$dir/$2/Contents/Info.plist"
	printf 'payload\n' >"$dir/$name.vwpayload"
	( cd "$dir" && zip -qr "$1" "$2" "$name.vwpayload" )
	rm -rf "$dir"
}
GOOD_ZIP="$WORK/good.zip"
BAD_ZIP="$WORK/bad.zip"
build_zip "$GOOD_ZIP" "HomeskzIfcImportDev.vwlibrary"
build_zip "$BAD_ZIP" "WrongName.vwlibrary"

# ===========================================================================
# asset_url — pick a browser_download_url out of an assets array by file name.
# ===========================================================================
t "asset_url finds the matching asset"
out="$(RUN asset_url "$STABLE_JSON" "assets" "HomeskzIfcImport.vwlibrary.zip")"
check_eq "$out" "https://example.test/dl/HomeskzIfcImport.vwlibrary.zip" "asset_url returns the URL"

t "asset_url returns nothing for an unknown asset"
out="$(RUN asset_url "$STABLE_JSON" "assets" "does-not-exist.zip" || true)"
check_eq "$out" "" "asset_url is empty when no asset matches"

# ===========================================================================
# installed_commit — the "no bundle installed -> none" branch (the PlistBuddy
# branch is macOS-only). Run the REAL function, not the canned fake.
# ===========================================================================
t "installed_commit is 'none' when the bundle is absent"
# Re-source the script inside the subshell to restore the REAL installed_commit
# (our fake shadows it in the parent), then call its no-bundle branch — that
# path needs no macOS tools, so it runs on Linux. The parent's fake is intact.
out="$( set -euo pipefail
	# shellcheck source=/dev/null
	source "$SCRIPT"
	installed_commit "$WORK/nope.vwlibrary" )"
check_eq "$out" "none" "absent bundle -> none"

# ===========================================================================
# q-stable — installed / latest / url lines, and the offline / incomplete paths.
# ===========================================================================
t "q_stable reports installed, 7-char latest and the asset url"
out="$(VW_TEST_INSTALLED=abc1234 RUN q_stable)"
check_contains "$out" "installed=abc1234" "installed line"
check_contains "$out" "latest=abc1234" "latest is the 7-char commit prefix"
check_contains "$out" "url=https://example.test/dl/HomeskzIfcImport.vwlibrary.zip" "url line"

t "q_stable reports installed=none when nothing is installed"
out="$(VW_TEST_INSTALLED=none RUN q_stable)"
check_contains "$out" "installed=none" "installed=none"
check_contains "$out" "latest=abc1234" "latest still present"

t "q_stable emits an error line when the API is unreachable"
out="$(VW_TEST_API_FAIL=1 RUN q_stable)"
check_contains "$out" "error=" "offline -> error= line"
check_not_contains "$out" "latest=" "no latest when offline"

# ===========================================================================
# q-dev — installed line + one TSV row per dev-* build that has a downloadable
# HomeskzIfcImportDev asset (the stable release and the asset-less dev build are
# both skipped).
# ===========================================================================
t "q_dev lists only dev-* builds that have a downloadable asset"
out="$(VW_TEST_INSTALLED=run1234 RUN q_dev)"
check_contains "$out" "installed=run1234" "installed line first"
check_contains "$out" $'build\taaa1111\tfeature/x\thttps://example.test/dl/x.zip' "feature/x row"
check_contains "$out" $'build\tbbb2222\tfeature/y\thttps://example.test/dl/y.zip' "feature/y row"
check_not_contains "$out" "feature/z" "asset-less dev build is skipped"
check_not_contains "$out" $'build\tzzz9999' "the stable (non dev-*) release is skipped"

t "q_dev emits an error line when the API is unreachable"
out="$(VW_TEST_API_FAIL=1 RUN q_dev)"
check_contains "$out" "error=" "offline -> error= line"

# ===========================================================================
# do-install — download + unzip + atomic swap into VW_PLUGINS_DIR, and its
# error paths. Uses the real unzip / cp / mv; only download + codesign/xattr are
# faked.
# ===========================================================================
t "do_install installs the bundle and prints ok"
dest="$WORK/plugins-ok"
mkdir -p "$dest"
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_ZIP="$GOOD_ZIP" \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_eq "$out" "ok" "do_install prints ok"
# **プラグインは自分のフォルダを 1 つ持つ**（<Plug-Ins>/<name>/。scripts/vw-install.sh）。
# 予備の配置もそこへ入れる——読む側（installed_bundle）と食い違わせないため。
if [ -f "$dest/HomeskzIfcImportDev/HomeskzIfcImportDev.vwlibrary/Contents/Info.plist" ]; then installed=yes; else installed=no; fi
check_eq "$installed" "yes" "the .vwlibrary landed in the plug-in's own folder"
# **本体も入っていること。** 殻だけ入れて本体を取りこぼすと、次の起動でプラグインは
# 何もできなくなる（src/PayloadHost.cpp が「本体が見つかりません」と言うだけ）。
if [ -f "$dest/HomeskzIfcImportDev/HomeskzIfcImportDev.vwpayload" ]; then installed=yes; else installed=no; fi
check_eq "$installed" "yes" "the .vwpayload landed next to the bundle"

t "do_install reports the installed shell id so the plug-in can skip the restart"
dest="$WORK/plugins-shellid"
mkdir -p "$dest"
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_ZIP="$GOOD_ZIP" VW_TEST_SHELL_ID="abc123def456" \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_contains "$out" "installed-shell=abc123def456" "prints the installed shell id"
check_contains "$out" "ok" "still prints ok"

t "do_install omits the shell id line when it cannot be read"
dest="$WORK/plugins-noshellid"
mkdir -p "$dest"
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_ZIP="$GOOD_ZIP" \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_eq "$out" "ok" "no shell id -> just ok (the plug-in then asks to restart)"

t "do_install reports a download failure"
dest="$WORK/plugins-dlfail"
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_FAIL=1 \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_contains "$out" "error=" "download failure -> error= line"

t "do_install reports a zip missing the expected bundle"
dest="$WORK/plugins-badzip"
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_ZIP="$BAD_ZIP" \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_contains "$out" "error=" "wrong bundle name -> error= line"

t "do_install rejects missing arguments"
out="$(RUN do_install "" "")"
check_contains "$out" "error=" "empty args -> error= line"

# ===========================================================================
# plugin_dir / installed_bundle — **プラグインは自分のフォルダを 1 つ持つ**。ここが
# インストーラ（scripts/vw-install.sh の同名関数）とずれると、入れた場所と読む場所が
# 食い違い、「更新したのに古いままに見える」事故になる。**渡された先が既にその
# フォルダなら足さない**のが肝で、これを落とすと更新のたびに入れ子が深くなる。
# ===========================================================================
t "plugin_dir appends the plug-in's own folder"
check_eq "$(RUN plugin_dir "/x/Plug-Ins" "HomeskzIfcImport")" "/x/Plug-Ins/HomeskzIfcImport" \
	"Plug-Ins -> Plug-Ins/<name>"

t "plugin_dir does not nest when it is already the plug-in's folder"
check_eq "$(RUN plugin_dir "/x/Plug-Ins/HomeskzIfcImport" "HomeskzIfcImport")" \
	"/x/Plug-Ins/HomeskzIfcImport" "already there -> unchanged"

t "installed_bundle points inside the plug-in's own folder"
check_eq "$(VW_PLUGINS_DIR=/x/Plug-Ins RUN installed_bundle "HomeskzIfcImport")" \
	"/x/Plug-Ins/HomeskzIfcImport/HomeskzIfcImport.vwlibrary" "bundle path"

# ===========================================================================
# plugin_zip_url — the distribution zip is found by exact name, and STILL found
# after the asset is renamed. **これが効かないと、アセット名を変えた瞬間に
# インストール済みの古いアップデータからは何も落とせなくなる**（利用者は手で
# 落とすしかなくなる）。
# ===========================================================================
RENAMED_JSON="$WORK/renamed.json"
cat >"$RENAMED_JSON" <<'JSON'
{
  "target_commitish": "abc1234def5678",
  "assets": [
    { "name": "notes.txt", "browser_download_url": "https://example.test/dl/notes.txt" },
    { "name": "SomethingElse.vwlibrary.zip",
      "browser_download_url": "https://example.test/dl/renamed.zip" }
  ]
}
JSON

t "plugin_zip_url prefers the exact asset name"
out="$(RUN plugin_zip_url "$STABLE_JSON" "assets" "HomeskzIfcImport")"
check_eq "$out" "https://example.test/dl/HomeskzIfcImport.vwlibrary.zip" "exact match wins"

t "plugin_zip_url still finds the zip after the asset was renamed"
out="$(RUN plugin_zip_url "$RENAMED_JSON" "assets" "HomeskzIfcImport")"
check_eq "$out" "https://example.test/dl/renamed.zip" "falls back to any *.vwlibrary.zip"

# ===========================================================================
# do-install の委譲 — **この変更の要**。落とした zip に vw-install.sh が入っていたら、
# 配置はそちらへ渡し、その機械可読な出力をそのまま流す。自前の配置（下の予備）は
# 使わない。
#
# 偽インストーラは「自分が呼ばれた証拠」を残して ok を出すだけ。**自前の配置なら必ず
# 置かれるはずの .vwlibrary が置かれていないこと**を見て、委譲が起きたと判定する。
# ===========================================================================
# build_zip_with_installer <zip> <bundle-dir-name> <installer-body>
build_zip_with_installer() {
	local dir="$WORK/stage-inst-$$-$RANDOM"
	local name="${2%.vwlibrary}"
	mkdir -p "$dir/$2/Contents"
	printf 'plist\n' >"$dir/$2/Contents/Info.plist"
	printf 'payload\n' >"$dir/$name.vwpayload"
	printf '%s\n' "$3" >"$dir/vw-install.sh"
	( cd "$dir" && zip -qr "$1" "$2" "$name.vwpayload" vw-install.sh )
	rm -rf "$dir"
}

t "do_install hands the placement to the installer that came with the zip"
dest="$WORK/plugins-delegated"
mkdir -p "$dest"
DELEGATED_ZIP="$WORK/delegated.zip"
build_zip_with_installer "$DELEGATED_ZIP" "HomeskzIfcImportDev.vwlibrary" \
	'printf "installed-shell=from-installer\nok\n"; : >"$VW_TEST_MARKER"'
marker="$WORK/installer-ran"
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_ZIP="$DELEGATED_ZIP" VW_TEST_MARKER="$marker" \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_contains "$out" "installed-shell=from-installer" "the installer's lines are passed through"
check_contains "$out" "ok" "ok is passed through"
if [ -f "$marker" ]; then ran=yes; else ran=no; fi
check_eq "$ran" "yes" "the bundled installer actually ran"
if [ -e "$dest/HomeskzIfcImportDev/HomeskzIfcImportDev.vwlibrary" ]; then fellback=yes; else fellback=no; fi
check_eq "$fellback" "no" "the built-in placement was NOT used"

t "do_install passes an installer error through unchanged"
dest="$WORK/plugins-delegated-err"
mkdir -p "$dest"
ERR_ZIP="$WORK/delegated-err.zip"
build_zip_with_installer "$ERR_ZIP" "HomeskzIfcImportDev.vwlibrary" \
	'printf "error=インストーラからの理由\n"'
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_ZIP="$ERR_ZIP" \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_contains "$out" "error=インストーラからの理由" "the installer's error reaches the plug-in"
check_not_contains "$out" "ok" "no ok line"

t "do_install falls back to its own placement when the installer says nothing"
dest="$WORK/plugins-mute"
mkdir -p "$dest"
MUTE_ZIP="$WORK/delegated-mute.zip"
build_zip_with_installer "$MUTE_ZIP" "HomeskzIfcImportDev.vwlibrary" 'exit 3'
out="$(VW_PLUGINS_DIR="$dest" VW_TEST_DL_ZIP="$MUTE_ZIP" \
	RUN do_install "https://example.test/dl/x.zip" "HomeskzIfcImportDev")"
check_eq "$out" "ok" "the built-in placement reported success"
if [ -f "$dest/HomeskzIfcImportDev/HomeskzIfcImportDev.vwpayload" ]; then fellback=yes; else fellback=no; fi
check_eq "$fellback" "yes" "a mute/broken installer never counts as done"

# ===========================================================================
echo "---------------------------------------------------------------"
if [ "$TESTS_FAILED" -eq 0 ]; then
	echo "PASS: all ${TESTS_RUN} checks passed."
	exit 0
fi
echo "FAIL: ${TESTS_FAILED} of ${TESTS_RUN} checks failed."
exit 1
