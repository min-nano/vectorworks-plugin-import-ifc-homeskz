#!/usr/bin/env bash
#
#	vw-uninstall.test.sh
#
#	Unit tests for the macOS UNINSTALLER (scripts/vw-uninstall.sh) — the script
#	that ships inside every release zip, is INSTALLED alongside the plug-in, and
#	is re-run by the NEXT release's installer to remove the one it replaces.
#
#	**このスイートが本当に守っているのは削除の安全性である。** ここは本リポジトリで
#	唯一「利用者のディスク上のものを消す」コードなので、中心の検査は 2 つ:
#
#	  * **消してよいものだけを消す** — フォルダ名が一致し、かつ中に殻がある
#	    ときだけ。`Plug-Ins` そのものや無関係なフォルダを名指しされても消さない。
#	  * **入っていなければ成功** — アップデートの入口で無条件に叩けること
#	    （初回インストールでも、この仕組みより前の版からでも止まらない）。
#
#	The script is SOURCED (its `main` is guarded, see its tail), so the real
#	functions run in-process against temp directories. **Nothing is stubbed** —
#	the deletion itself is what is under test, and it uses only `rm`/`basename`,
#	which run the same on Linux as on macOS.
#

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/../scripts/vw-uninstall.sh"

# ---------------------------------------------------------------------------
# Missing-tool policy — identical to the other script harnesses.
# ---------------------------------------------------------------------------
REQUIRE_TOOLS="${VW_REQUIRE_SCRIPT_TESTS:-}"
case "$REQUIRE_TOOLS" in
	'' | 0 | off | OFF | false | FALSE | no | NO) REQUIRE_TOOLS="" ;;
esac

skip_or_fail() {
	if [ -n "$REQUIRE_TOOLS" ]; then
		echo "ERROR vw-uninstall.test.sh: $1 (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)." >&2
		exit 1
	fi
	echo "SKIP vw-uninstall.test.sh: $1."
	exit 0
}

if [ ! -f "$SCRIPT" ]; then
	skip_or_fail "$SCRIPT not found"
fi

# ---------------------------------------------------------------------------
# Tiny assertion harness (same shape as the other script suites).
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
# shellcheck source=/dev/null
source "$SCRIPT"
set +e +u +o pipefail

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

RUN() { (
	set -euo pipefail
	"$@"
); }

NAME="HomeskzIfcImportDev"

# make_install <plugins-dir> [name] — 実際のインストール後と同じ形を作る。
make_install() {
	local root="$1" name="${2:-$NAME}"
	mkdir -p "$root/$name/$name.vwlibrary/Contents"
	printf 'plist\n' >"$root/$name/$name.vwlibrary/Contents/Info.plist"
	printf 'payload\n' >"$root/$name/$name.vwpayload"
	printf '#!/bin/bash\n' >"$root/$name/vw-uninstall.sh"
}

# ===========================================================================
# plugin_dir — **インストーラと同じ規則**でなければならない（片方だけ変えると、
# 入れた場所と消す場所が食い違う）。
# ===========================================================================
t "plugin_dir appends the plug-in's own folder"
check_eq "$(RUN plugin_dir "/x/Plug-Ins" "$NAME")" "/x/Plug-Ins/$NAME" "Plug-Ins -> Plug-Ins/<name>"

t "plugin_dir does not nest when it is already the plug-in's folder"
check_eq "$(RUN plugin_dir "/x/Plug-Ins/$NAME" "$NAME")" "/x/Plug-Ins/$NAME" "already there -> unchanged"

# ===========================================================================
# 取り除く — フォルダごと。**その版が増やしたファイルも一緒に消えること**が肝で、
# ファイル名を列挙していたら取りこぼす。
# ===========================================================================
t "remove_plugin_dir removes the whole plug-in folder"
root="$WORK/plugins-ok"
make_install "$root"
printf 'extra\n' >"$root/$NAME/$NAME.some-future-file"
MACHINE=1 RUN remove_plugin_dir "$root/$NAME" "$NAME"
check_eq "$?" "0" "remove succeeds"
check_no_file "$root/$NAME" "the plug-in folder is gone"
check_file "$root" "Plug-Ins itself is untouched"

# ===========================================================================
# **安全弁** — ここが本スイートの中心。消してよい形でなければ、名指しされても消さない。
# ===========================================================================
t "remove_plugin_dir refuses a folder whose name is not the plug-in's"
root="$WORK/plugins-name"
mkdir -p "$root/SomethingElse"
printf 'keep me\n' >"$root/SomethingElse/keep.txt"
out="$(MACHINE=1 RUN remove_plugin_dir "$root/SomethingElse" "$NAME" 2>&1)"
check_eq "$?" "1" "refused"
check_file "$root/SomethingElse/keep.txt" "nothing was deleted"

t "remove_plugin_dir refuses a folder that holds no shell"
# **Plug-Ins そのものを名指しされた形**（名前は一致しうるが、中に殻が無い）。これを
# 消してしまうと利用者の他のプラグインごと消える。
root="$WORK/Plug-Ins"
mkdir -p "$root/SomeoneElsePlugin"
printf 'keep me\n' >"$root/important.txt"
out="$(MACHINE=1 RUN remove_plugin_dir "$root" "Plug-Ins" 2>&1)"
check_eq "$?" "1" "refused"
check_file "$root/important.txt" "the other plug-ins are untouched"
check_file "$root/SomeoneElsePlugin" "the other plug-ins are untouched"

# ===========================================================================
# 入っていなければ成功 — アップデートの入口で無条件に叩けること。
# ===========================================================================
t "remove_plugin_dir succeeds when there is nothing installed"
MACHINE=1 RUN remove_plugin_dir "$WORK/nowhere/$NAME" "$NAME"
check_eq "$?" "0" "absent -> success"

# ===========================================================================
# guess_name — 名前を渡されなくても、置かれているものから割り出す（手動実行）。
# ===========================================================================
t "guess_name finds the plug-in inside a Plug-Ins folder"
root="$WORK/plugins-guess"
make_install "$root"
check_eq "$(RUN guess_name "$root")" "$NAME" "found from <Plug-Ins>/<name>/<name>.vwlibrary"

t "guess_name accepts the plug-in's own folder"
check_eq "$(RUN guess_name "$root/$NAME")" "$NAME" "found from the folder itself"

t "guess_name is empty when nothing is installed"
mkdir -p "$WORK/plugins-empty"
check_eq "$(RUN guess_name "$WORK/plugins-empty")" "" "nothing -> empty"

# ===========================================================================
# main --machine — the contract the installer reads: "removed=<path>" then "ok",
# or a single "error=<message>". Always exits 0.
# ===========================================================================
t "main --machine removes the install and reports where"
root="$WORK/plugins-machine"
make_install "$root"
out="$(RUN main --machine --plugins-dir "$root")"
check_eq "$?" "0" "machine mode always exits 0"
check_contains "$out" "removed=$root/$NAME" "reports the removed path"
check_contains "$out" "ok" "ok line"
check_no_file "$root/$NAME" "the plug-in folder is gone"

t "main --machine is a no-op success when nothing is installed"
out="$(RUN main --machine --plugins-dir "$WORK/plugins-empty")"
check_eq "$out" "ok" "just ok — nothing to remove"

t "main --machine reports a refusal as error= and exits 0"
root="$WORK/plugins-refuse"
mkdir -p "$root/$NAME"
printf 'no shell here\n' >"$root/$NAME/stray.txt"
out="$(RUN main --machine --plugins-dir "$root" --name "$NAME")"
check_eq "$?" "0" "machine mode always exits 0"
check_contains "$out" "error=" "refusal is reported on stdout"
check_file "$root/$NAME/stray.txt" "nothing was deleted"

t "main --machine tolerates options it does not know"
root="$WORK/plugins-future"
make_install "$root"
out="$(RUN main --machine --plugins-dir "$root" --a-future-option 42 --a-future-flag)"
check_contains "$out" "ok" "unknown options do not stop the removal"
check_no_file "$root/$NAME" "the plug-in folder is gone"

t "main removes the plug-in when handed its own folder"
# アップデータ経由の呼ばれ方（プラグインは「いま自分が読み込まれたフォルダ」を渡す）。
root="$WORK/plugins-own"
make_install "$root"
out="$(RUN main --machine --plugins-dir "$root/$NAME" --name "$NAME")"
check_contains "$out" "ok" "ok"
check_no_file "$root/$NAME" "the plug-in folder is gone"

# ===========================================================================
echo "---------------------------------------------------------------"
if [ "$TESTS_FAILED" -eq 0 ]; then
	echo "PASS: all ${TESTS_RUN} checks passed."
	exit 0
fi
echo "FAIL: ${TESTS_FAILED} of ${TESTS_RUN} checks failed."
exit 1
