#!/usr/bin/env bash
#
#	vw-install.test.sh
#
#	Unit tests for the macOS INSTALLER (scripts/vw-install.sh) — the script that
#	is shipped inside every release zip (and published as a release asset), and
#	to which the installed updater hands the actual file placement.
#
#	なぜここを厚くテストするか: **このスクリプトが「配置の手順」の唯一の持ち主**に
#	なったから。ここが取りこぼすと、利用者の Plug-Ins に半端なプラグインが残る
#	（M21 で本体 .vwpayload が増えたときに実際に起きた事故で、この仕組みはその
#	再発を止めるためにある）。したがって中心の検査は 1 つ:
#
#	    **zip の直下にあるものが、列挙されていなくても全部入ること。**
#
#	置き先が **`Plug-Ins` 直下ではなくプラグインのフォルダ**（`<Plug-Ins>/<name>/`）に
#	なったので、そこも同じ重さで見る——**入れ子にならないこと**（アップデータは「いま
#	自分が読み込まれたフォルダ」を渡してくる）と、**入れる前に前の版が取り除かれること**。
#
#	The script is SOURCED (its `main` is guarded, see the tail of vw-install.sh),
#	so the real functions run in-process and only their outermost macOS-only
#	leaves are replaced:
#
#	  * jval               the JSON boundary (real one shells out to `plutil`,
#	                       which is macOS-only) — emulated with python3 so the
#	                       REAL release_zip logic runs against JSON fixtures on a
#	                       plain Linux runner.
#	  * codesign / xattr   Gatekeeper tools; no-ops here.
#	  * installed_shell_id reads the installed bundle's Info.plist via macOS
#	                       PlistBuddy; canned here (its "no plist -> empty"
#	                       branch is still exercised for real below).
#
#	This mirrors tests/vw-update.test.sh exactly — same harness, same policy on
#	missing tools. The genuinely macOS-only surface (codesign / xattr /
#	PlistBuddy) is left to manual end-to-end testing on a Mac.
#

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/../scripts/vw-install.sh"

# ---------------------------------------------------------------------------
# Missing-tool policy — identical to vw-update.test.sh: skip locally, hard-fail
# in CI (VW_REQUIRE_SCRIPT_TESTS), so a missing dependency can never masquerade
# as a passing run.
# ---------------------------------------------------------------------------
REQUIRE_TOOLS="${VW_REQUIRE_SCRIPT_TESTS:-}"
case "$REQUIRE_TOOLS" in
	'' | 0 | off | OFF | false | FALSE | no | NO) REQUIRE_TOOLS="" ;;
esac

skip_or_fail() {
	if [ -n "$REQUIRE_TOOLS" ]; then
		echo "ERROR vw-install.test.sh: $1 (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)." >&2
		exit 1
	fi
	echo "SKIP vw-install.test.sh: $1."
	exit 0
}

for tool in python3 unzip zip; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		skip_or_fail "'$tool' not found (installer tests need it)"
	fi
done
if [ ! -f "$SCRIPT" ]; then
	skip_or_fail "$SCRIPT not found"
fi

# ---------------------------------------------------------------------------
# Tiny assertion harness (same shape as tests/vw-update.test.sh).
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

check_file() { # path label
	TESTS_RUN=$((TESTS_RUN + 1))
	if [ ! -e "$1" ]; then
		TESTS_FAILED=$((TESTS_FAILED + 1))
		printf 'FAIL [%s] %s\n  missing path: %s\n' "$CURRENT" "$2" "$1"
	fi
}

check_no_file() { # path label
	TESTS_RUN=$((TESTS_RUN + 1))
	if [ -e "$1" ]; then
		TESTS_FAILED=$((TESTS_FAILED + 1))
		printf 'FAIL [%s] %s\n  unexpected path: %s\n' "$CURRENT" "$2" "$1"
	fi
}

# ---------------------------------------------------------------------------
# Load the script under test, then install the fakes (see the header).
# ---------------------------------------------------------------------------
# shellcheck source=/dev/null
source "$SCRIPT"
set +e +u +o pipefail

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

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

# Gatekeeper tools: absent on Linux and irrelevant to what is under test.
codesign() { return 0; }
xattr() { return 0; }

# installed_shell_id: the real one needs macOS PlistBuddy. Canned here; the
# genuine "no plist -> empty" branch is exercised separately below.
installed_shell_id() { printf '%s\n' "${VW_TEST_SHELL_ID:-}"; }

# RUN: invoke a script function in a faithful `set -euo pipefail` subshell.
RUN() { (
	set -euo pipefail
	"$@"
); }

# make_tree: build a realistic unpacked release layout under $1.
#   <name>.vwlibrary/Contents/Info.plist   the shell (a bundle)
#   <name>.vwpayload                       the payload
#   <name>.brand-new                       **a file no caller enumerates** — the
#                                          whole point of this suite
#   vw-install.sh                          the installer itself (must NOT be
#                                          installed into Plug-Ins)
#   vw-uninstall.sh                        the uninstaller — **this one IS installed**
#                                          (the next update re-runs it; see
#                                          scripts/vw-uninstall.sh)
make_tree() { # dir, name
	local dir="$1" name="$2"
	mkdir -p "$dir/$name.vwlibrary/Contents"
	printf 'plist\n' >"$dir/$name.vwlibrary/Contents/Info.plist"
	printf 'payload\n' >"$dir/$name.vwpayload"
	printf 'a file added by a future layout\n' >"$dir/$name.brand-new"
	printf '#!/bin/bash\n' >"$dir/vw-install.sh"
	# 本物のアンインストーラを入れる。**前の版を取り除く段**（remove_installed）は
	# これを写して走らせるので、偽物では意味が無い。
	cp "${HERE}/../scripts/vw-uninstall.sh" "$dir/vw-uninstall.sh"
}

NAME="HomeskzIfcImportDev"

# ===========================================================================
# install_tree — **the core promise: everything at the root goes in, listed or
# not; the installer itself does not.**
# ===========================================================================
t "install_tree installs every entry into the plug-in's own folder"
src="$WORK/tree-all"
dest="$WORK/plugins-all"
mkdir -p "$src"
make_tree "$src" "$NAME"
MACHINE=1 PLUGINS_DIR="$dest" RUN install_tree "$src" "$NAME"
check_eq "$?" "0" "install_tree succeeds"
check_file "$dest/$NAME/$NAME.vwlibrary/Contents/Info.plist" "the shell bundle landed"
check_file "$dest/$NAME/$NAME.vwpayload" "the payload landed"
check_file "$dest/$NAME/$NAME.brand-new" "a file nobody enumerated still landed"
check_no_file "$dest/$NAME.vwlibrary" "nothing is left directly in Plug-Ins"
check_no_file "$dest/$NAME/vw-install.sh" "the installer itself is not installed"
# **アンインストーラは置く。** 次のアップデートが「前の版を、その版自身の知識で
# 取り除く」ために要る（scripts/vw-uninstall.sh の冒頭）。
check_file "$dest/$NAME/vw-uninstall.sh" "the uninstaller travels with the install"

# ===========================================================================
# 入れ子にならないこと — **落とすと更新のたびに深くなる。** 自動アップデートでは
# プラグインが「いま自分が読み込まれたフォルダ」を渡してくるので、そこは既に
# <Plug-Ins>/<name> である。
# ===========================================================================
t "install_tree does not nest when handed the plug-in's own folder"
MACHINE=1 PLUGINS_DIR="$dest/$NAME" RUN install_tree "$src" "$NAME"
check_eq "$?" "0" "install_tree succeeds again"
check_file "$dest/$NAME/$NAME.vwpayload" "still installed in the same place"
check_no_file "$dest/$NAME/$NAME" "no <name>/<name> nesting"

# ===========================================================================
# 前の版を取り除いてから入れること — その版にしか無かったファイルが残らない
# （残ると、消えたはずのものが Vectorworks から見え続ける）。
# ===========================================================================
# 直接呼ぶので TMP_ROOT は空のまま——**取り除く段が自分で一時ディレクトリを作れる
# こと**（呼び出し側の状態に頼っていないこと）も、ここで一緒に押さえている。
t "install_tree removes the previously installed release first"
printf 'only in the old release\n' >"$dest/$NAME/$NAME.only-in-old"
MACHINE=1 PLUGINS_DIR="$dest/$NAME" RUN install_tree "$src" "$NAME"
check_eq "$?" "0" "install_tree succeeds"
check_no_file "$dest/$NAME/$NAME.only-in-old" "the old release's leftovers are gone"
check_file "$dest/$NAME/$NAME.vwpayload" "the new release is in place"

t "install_tree refuses an archive without the expected shell"
src="$WORK/tree-wrong"
dest="$WORK/plugins-wrong"
mkdir -p "$src"
make_tree "$src" "SomethingElse"
out="$(MACHINE=1 PLUGINS_DIR="$dest" RUN install_tree "$src" "$NAME" 2>&1)"
check_eq "$?" "1" "install_tree fails on a mismatched archive"
check_no_file "$dest/$NAME" "nothing is placed when the check fails"

t "install_tree replaces what is already installed"
src="$WORK/tree-replace"
dest="$WORK/plugins-replace"
mkdir -p "$src" "$dest/$NAME/$NAME.vwlibrary/Contents"
printf 'OLD\n' >"$dest/$NAME/$NAME.vwlibrary/Contents/Info.plist"
printf 'STALE\n' >"$dest/$NAME/$NAME.vwlibrary/Contents/Stale.txt"
printf 'OLD\n' >"$dest/$NAME/$NAME.vwpayload"
make_tree "$src" "$NAME"
MACHINE=1 PLUGINS_DIR="$dest" RUN install_tree "$src" "$NAME"
check_eq "$(cat "$dest/$NAME/$NAME.vwlibrary/Contents/Info.plist")" "plist" "the bundle was replaced"
check_no_file "$dest/$NAME/$NAME.vwlibrary/Contents/Stale.txt" "the old bundle's leftovers are gone"
check_eq "$(cat "$dest/$NAME/$NAME.vwpayload")" "payload" "the payload was replaced"

# ===========================================================================
# plugin_dir — **インストーラとアンインストーラで同じ規則**でなければならない
# （scripts/vw-uninstall.sh の同名関数）。
# ===========================================================================
t "plugin_dir appends the plug-in's own folder"
check_eq "$(RUN plugin_dir "/x/Plug-Ins" "$NAME")" "/x/Plug-Ins/$NAME" "Plug-Ins -> Plug-Ins/<name>"

t "plugin_dir does not nest when it is already the plug-in's folder"
check_eq "$(RUN plugin_dir "/x/Plug-Ins/$NAME" "$NAME")" "/x/Plug-Ins/$NAME" "already there -> unchanged"

# ===========================================================================
# guess_name — the plug-in name is read from the archive, so `--name` is
# optional for a manual install.
# ===========================================================================
t "guess_name reads the plug-in name off the shell bundle"
check_eq "$(RUN guess_name "$WORK/tree-all")" "$NAME" "name comes from <name>.vwlibrary"

t "guess_name is empty when there is no shell in the directory"
mkdir -p "$WORK/empty-dir"
check_eq "$(RUN guess_name "$WORK/empty-dir")" "" "no bundle -> empty"

# ===========================================================================
# installed_shell_id — the "no bundle installed -> empty" branch (the PlistBuddy
# branch is macOS-only). Runs the REAL function, not the canned fake.
# ===========================================================================
t "installed_shell_id is empty when nothing is installed"
out="$( set -euo pipefail
	# shellcheck source=/dev/null
	source "$SCRIPT"
	PLUGINS_DIR="$WORK/nowhere" installed_shell_id "$NAME" )"
check_eq "$out" "" "absent bundle -> empty"

# ===========================================================================
# release_zip — resolve the distribution zip out of a release. Exact name first,
# then the "*.vwlibrary.zip" fallback that keeps an OLD installed updater working
# after the asset is renamed.
# ===========================================================================
REL_JSON="$WORK/release.json"
cat >"$REL_JSON" <<'JSON'
{
  "target_commitish": "abc1234def5678",
  "assets": [
    { "name": "notes.txt",
      "browser_download_url": "https://example.test/dl/notes.txt" },
    { "name": "HomeskzIfcImport.vwlibrary.zip",
      "browser_download_url": "https://example.test/dl/HomeskzIfcImport.vwlibrary.zip" }
  ]
}
JSON

t "release_zip finds the asset by exact plug-in name"
check_eq "$(RUN release_zip "$REL_JSON" "HomeskzIfcImport")" \
	"$(printf 'https://example.test/dl/HomeskzIfcImport.vwlibrary.zip\tHomeskzIfcImport')" \
	"exact match wins"

t "release_zip falls back to any *.vwlibrary.zip and reports its plug-in name"
check_eq "$(RUN release_zip "$REL_JSON" "")" \
	"$(printf 'https://example.test/dl/HomeskzIfcImport.vwlibrary.zip\tHomeskzIfcImport')" \
	"suffix match yields url + name"

t "release_zip returns nothing when the release carries no distribution zip"
NOZIP_JSON="$WORK/nozip.json"
cat >"$NOZIP_JSON" <<'JSON'
{ "assets": [ { "name": "notes.txt", "browser_download_url": "https://example.test/n" } ] }
JSON
check_eq "$(RUN release_zip "$NOZIP_JSON" "" || true)" "" "no candidate -> empty"

# ===========================================================================
# parse_args — **unknown options must be ignored, not fatal.** A NEWER updater
# may hand an OLDER in-zip installer an option it has never heard of; refusing
# it would break exactly the update path this whole design exists to keep open.
# ===========================================================================
t "parse_args ignores unknown options and their values"
out="$( set -euo pipefail
	# shellcheck source=/dev/null
	source "$SCRIPT"
	parse_args --future-flag --future-option value --name X --machine
	printf '%s|%s' "$OPT_NAME" "$MACHINE" )"
check_eq "$out" "X|1" "known options still bind after unknown ones"

# ===========================================================================
# main --machine — the contract the plug-in reads (src/UpdaterParse.h):
# "installed-shell=<id>" (optional) then "ok", or a single "error=<message>".
# ===========================================================================
ZIP="$WORK/dist.zip"
(cd "$WORK/tree-all" && zip -qr "$ZIP" .)

t "main --machine installs from a zip and prints ok"
dest="$WORK/plugins-machine"
out="$(VW_TEST_SHELL_ID="" RUN main --machine --zip "$ZIP" --plugins-dir "$dest")"
check_eq "$out" "ok" "machine mode prints just ok"
check_file "$dest/$NAME/$NAME.vwpayload" "the payload landed"
check_file "$dest/$NAME/$NAME.brand-new" "the unlisted file landed"

t "main --machine reports the installed shell id when it can be read"
dest="$WORK/plugins-machine-id"
out="$(VW_TEST_SHELL_ID="abc123def456" RUN main --machine --zip "$ZIP" --plugins-dir "$dest")"
check_contains "$out" "installed-shell=abc123def456" "shell id line"
check_contains "$out" "ok" "still prints ok"

t "main --machine reports a broken archive as error= and exits 0"
dest="$WORK/plugins-machine-bad"
out="$(RUN main --machine --zip "$WORK/does-not-exist.zip" --plugins-dir "$dest")"
check_eq "$?" "0" "machine mode always exits 0"
check_contains "$out" "error=" "failure is reported on stdout"
check_not_contains "$out" "ok" "no ok line on failure"

t "main --machine tolerates options it does not know"
dest="$WORK/plugins-machine-future"
out="$(RUN main --machine --zip "$ZIP" --plugins-dir "$dest" --a-future-option 42 --a-future-flag)"
check_eq "$out" "ok" "unknown options do not stop the install"

# ===========================================================================
echo "---------------------------------------------------------------"
if [ "$TESTS_FAILED" -eq 0 ]; then
	echo "PASS: all ${TESTS_RUN} checks passed."
	exit 0
fi
echo "FAIL: ${TESTS_FAILED} of ${TESTS_RUN} checks failed."
exit 1
