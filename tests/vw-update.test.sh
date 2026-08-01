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
#	                short_of / q_stable / q_dev logic runs against JSON fixtures
#	                on a plain Linux runner. (This mirrors the C++ tests, which
#	                also run SDK-free on Linux — see tests/README.md.)
#	  * fetch_json  returns a fixture instead of fetching the R2 manifest.
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
# The distribution base URL is injected at BUILD time (CMake substitutes
# @VW_UPDATE_BASE_URL@), so the repository copy of the script has none and every
# mode would short-circuit with "配布先 URL が設定されていません". Provide one
# through the environment BEFORE sourcing, exactly as a manual run would; the
# "not configured" branch is covered by its own test below.
export VW_BASE_URL="https://dist.example.test"

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

# fetch_json: copy the fixture selected for this bucket key into a fresh temp file
# and echo its path — the real fetch_json returns a temp file the caller then
# `rm`s, so we must NOT hand back the fixture itself. VW_TEST_API_FAIL simulates
# an unreachable bucket.
fetch_json() { # bucket key
	[ -n "${VW_TEST_API_FAIL:-}" ] && return 1
	local fixture=""
	case "$1" in
		stable/manifest.json) fixture="${VW_TEST_STABLE_JSON:-}" ;;
		dev/index.json) fixture="${VW_TEST_INDEX_JSON:-}" ;;
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

# codesign / xattr are macOS Gatekeeper tools do_install calls; no-op them so the
# install path runs cleanly (and quietly) on Linux.
codesign() { return 0; }
xattr() { return 0; }

# RUN: invoke a script function in a faithful `set -euo pipefail` subshell and
# capture its stdout. The subshell inherits the fakes defined above.
RUN() { ( set -euo pipefail; "$@" ); }

# --- Fixtures -------------------------------------------------------------
# The two JSON objects the bucket serves (see scripts/r2-publish.sh for the
# schema): the stable manifest, and the dev index listing one entry per branch.
STABLE_JSON="$WORK/stable.json"
cat >"$STABLE_JSON" <<'JSON'
{
  "schema": 1,
  "channel": "stable",
  "branch": "main",
  "commit": "abc1234def5678",
  "short": "abc1234",
  "built": "2026-08-01T00:00:00Z",
  "mac": "https://dist.example.test/stable/abc1234/HomeskzIfcImport.vwlibrary.zip",
  "win": "https://dist.example.test/stable/abc1234/HomeskzIfcImport.vlb.zip"
}
JSON

# feature/y deliberately has NO "short" (the commit-prefix fallback must kick
# in), feature/z has no download URLs (must be skipped), and the last entry has
# no "branch" (the slug must stand in as the display name).
INDEX_JSON="$WORK/index.json"
cat >"$INDEX_JSON" <<'JSON'
{
  "schema": 1,
  "generated": "2026-08-01T00:00:00Z",
  "builds": [
    { "branch": "feature/x", "slug": "feature-x",
      "commit": "aaa1111ccc", "short": "aaa1111",
      "mac": "https://dist.example.test/dev/feature-x/aaa1111/HomeskzIfcImportDev.vwlibrary.zip",
      "win": "https://dist.example.test/dev/feature-x/aaa1111/HomeskzIfcImportDev.vlb.zip" },
    { "branch": "feature/y", "slug": "feature-y",
      "commit": "bbb2222ddd",
      "mac": "https://dist.example.test/dev/feature-y/bbb2222/HomeskzIfcImportDev.vwlibrary.zip",
      "win": "https://dist.example.test/dev/feature-y/bbb2222/HomeskzIfcImportDev.vlb.zip" },
    { "branch": "feature/z", "slug": "feature-z",
      "commit": "ccc3333eee", "short": "ccc3333" },
    { "slug": "no-branch-field",
      "commit": "ddd4444fff", "short": "ddd4444",
      "mac": "https://dist.example.test/dev/no-branch-field/ddd4444/HomeskzIfcImportDev.vwlibrary.zip",
      "win": "https://dist.example.test/dev/no-branch-field/ddd4444/HomeskzIfcImportDev.vlb.zip" }
  ]
}
JSON

export VW_TEST_STABLE_JSON="$STABLE_JSON"
export VW_TEST_INDEX_JSON="$INDEX_JSON"

# Build a real "HomeskzIfcImportDev.vwlibrary.zip" for the do-install tests, and a
# malformed one whose top-level dir has the wrong name.
build_zip() { # zip-path, bundle-dir-name
	local dir="$WORK/stage-$$-$RANDOM"
	mkdir -p "$dir/$2/Contents"
	printf 'plist\n' >"$dir/$2/Contents/Info.plist"
	( cd "$dir" && zip -qr "$1" "$2" )
	rm -rf "$dir"
}
GOOD_ZIP="$WORK/good.zip"
BAD_ZIP="$WORK/bad.zip"
build_zip "$GOOD_ZIP" "HomeskzIfcImportDev.vwlibrary"
build_zip "$BAD_ZIP" "WrongName.vwlibrary"

# ===========================================================================
# short_of — the 7-char build id of a manifest entry, with the commit fallback.
# ===========================================================================
t "short_of prefers the manifest's own short field"
out="$(RUN short_of "$STABLE_JSON" "")"
check_eq "$out" "abc1234" "top-level short"

t "short_of reads an indexed entry"
out="$(RUN short_of "$INDEX_JSON" "builds.0.")"
check_eq "$out" "aaa1111" "builds.0.short"

t "short_of falls back to the commit prefix when short is absent"
out="$(RUN short_of "$INDEX_JSON" "builds.1.")"
check_eq "$out" "bbb2222" "first 7 chars of commit"

t "short_of is empty when neither field is present"
out="$(RUN short_of "$INDEX_JSON" "builds.99.")"
check_eq "$out" "" "missing entry -> empty"

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
check_contains "$out" "latest=abc1234" "latest is the 7-char build id"
check_contains "$out" "url=https://dist.example.test/stable/abc1234/HomeskzIfcImport.vwlibrary.zip" "url line (mac asset)"
check_not_contains "$out" "HomeskzIfcImport.vlb.zip" "the Windows asset is not offered to macOS"

t "q_stable reports installed=none when nothing is installed"
out="$(VW_TEST_INSTALLED=none RUN q_stable)"
check_contains "$out" "installed=none" "installed=none"
check_contains "$out" "latest=abc1234" "latest still present"

t "q_stable emits an error line when the manifest is unreachable"
out="$(VW_TEST_API_FAIL=1 RUN q_stable)"
check_contains "$out" "error=" "offline -> error= line"
check_not_contains "$out" "latest=" "no latest when offline"

# ===========================================================================
# q-dev — installed line + one TSV row per indexed build that has a downloadable
# HomeskzIfcImportDev asset.
# ===========================================================================
t "q_dev lists only indexed builds that have a downloadable asset"
out="$(VW_TEST_INSTALLED=run1234 RUN q_dev)"
check_contains "$out" "installed=run1234" "installed line first"
check_contains "$out" $'build\taaa1111\tfeature/x\thttps://dist.example.test/dev/feature-x/aaa1111/HomeskzIfcImportDev.vwlibrary.zip' "feature/x row"
check_contains "$out" $'build\tbbb2222\tfeature/y\thttps://dist.example.test/dev/feature-y/bbb2222/HomeskzIfcImportDev.vwlibrary.zip' "feature/y row (short derived from commit)"
check_not_contains "$out" "feature/z" "asset-less build is skipped"
check_contains "$out" $'build\tddd4444\tno-branch-field\t' "slug stands in when branch is absent"

t "q_dev emits an error line when the index is unreachable"
out="$(VW_TEST_API_FAIL=1 RUN q_dev)"
check_contains "$out" "error=" "offline -> error= line"

# ===========================================================================
# No distribution base URL configured — the state of the repository copy, whose
# @VW_UPDATE_BASE_URL@ placeholder is only substituted when CMake bundles the
# script. Both query modes must say so instead of reaching the network.
# ===========================================================================
t "q_stable / q_dev report a missing base URL"
out="$( set -uo pipefail
	unset VW_BASE_URL
	# shellcheck source=/dev/null
	source "$SCRIPT"
	q_stable
	q_dev )"
check_contains "$out" "error=配布先 URL" "unset VW_BASE_URL -> error= line"
check_not_contains "$out" "installed=" "nothing else is printed"

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
if [ -f "$dest/HomeskzIfcImportDev.vwlibrary/Contents/Info.plist" ]; then installed=yes; else installed=no; fi
check_eq "$installed" "yes" "the .vwlibrary landed in the plug-ins dir"

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
echo "---------------------------------------------------------------"
if [ "$TESTS_FAILED" -eq 0 ]; then
	echo "PASS: all ${TESTS_RUN} checks passed."
	exit 0
fi
echo "FAIL: ${TESTS_FAILED} of ${TESTS_RUN} checks failed."
exit 1
