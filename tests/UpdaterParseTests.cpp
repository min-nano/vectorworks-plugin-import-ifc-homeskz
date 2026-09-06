//
//	UpdaterParseTests.cpp
//
//	Unit tests for the SDK-independent updater helpers in src/UpdaterParse.h:
//	the script-output parsers (Trim / ValueOf / ParseDevBuilds), the two
//	command-line quoters (ShellQuote / CmdQuote), and the path derivations
//	(Mac*/Win* FromBinary/FromPath/FromDir).
//
//	These are the parts of Updater.cpp that carry real branching logic yet do NOT
//	touch the Vectorworks SDK, so they can be exercised exhaustively on a plain
//	toolchain — which is what the coverage job measures.
//

#include "TestFramework.h"
#include "UpdaterParse.h"

#include <string>
#include <vector>

using namespace HomeskzIfcImport::UpdaterParse;

// ---------------------------------------------------------------------------
// Trim
// ---------------------------------------------------------------------------

TEST(trim_removes_surrounding_whitespace)
{
	CHECK_EQ(Trim("  hello  "), "hello");
	CHECK_EQ(Trim("\t\r\n value \n\t"), "value");
	CHECK_EQ(Trim("no-padding"), "no-padding");
}

TEST(trim_all_whitespace_or_empty_is_empty)
{
	CHECK_EQ(Trim(""), "");
	CHECK_EQ(Trim("   \t\r\n  "), "");
}

TEST(trim_preserves_interior_whitespace)
{
	CHECK_EQ(Trim("  a b\tc  "), "a b\tc");
}

// ---------------------------------------------------------------------------
// ValueOf
// ---------------------------------------------------------------------------

TEST(valueof_finds_key_and_trims_value)
{
	const std::string out = "installed=abc123\n"
							"latest=def456\n"
							"url=https://example.com/x.zip\n";
	CHECK_EQ(ValueOf(out, "installed"), "abc123");
	CHECK_EQ(ValueOf(out, "latest"), "def456");
	CHECK_EQ(ValueOf(out, "url"), "https://example.com/x.zip");
}

TEST(valueof_missing_key_is_empty)
{
	const std::string out = "installed=abc123\nlatest=def456\n";
	CHECK_EQ(ValueOf(out, "error"), "");
	CHECK_EQ(ValueOf(out, "nope"), "");
}

TEST(valueof_returns_first_match)
{
	const std::string out = "key=first\nkey=second\n";
	CHECK_EQ(ValueOf(out, "key"), "first");
}

TEST(valueof_trims_value_whitespace)
{
	const std::string out = "key=   spaced value   \n";
	CHECK_EQ(ValueOf(out, "key"), "spaced value");
}

TEST(valueof_handles_last_line_without_newline)
{
	// No trailing '\n' on the final line: the loop must still match it.
	const std::string out = "a=1\nlatest=v9";
	CHECK_EQ(ValueOf(out, "latest"), "v9");
}

TEST(valueof_empty_value)
{
	const std::string out = "error=\ninstalled=abc\n";
	// error is present but blank -> empty string (distinct from "missing", but
	// both render as "").
	CHECK_EQ(ValueOf(out, "error"), "");
	CHECK_EQ(ValueOf(out, "installed"), "abc");
}

TEST(valueof_does_not_match_key_as_substring)
{
	// "installed" must not be returned when asking for "install".
	const std::string out = "installed=abc\n";
	CHECK_EQ(ValueOf(out, "install"), "");
}

TEST(valueof_empty_input)
{
	CHECK_EQ(ValueOf("", "key"), "");
}

// ---------------------------------------------------------------------------
// ParseDevBuilds
// ---------------------------------------------------------------------------

TEST(parsedevbuilds_parses_multiple_rows)
{
	const std::string out = "channel=dev\n"
							"build\tc0ffee1\tfeature/one\thttps://ex.com/one.zip\n"
							"build\tdeadbee\tfeature/two\thttps://ex.com/two.zip\n";

	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(2));
	if (builds.size() == 2)
	{
		CHECK_EQ(builds[0].commit, "c0ffee1");
		CHECK_EQ(builds[0].name, "feature/one");
		CHECK_EQ(builds[0].url, "https://ex.com/one.zip");
		CHECK_EQ(builds[1].commit, "deadbee");
		CHECK_EQ(builds[1].name, "feature/two");
		CHECK_EQ(builds[1].url, "https://ex.com/two.zip");
	}
}

TEST(parsedevbuilds_reads_the_optional_branch_column)
{
	// 5 列目のブランチ名。取り込みのついでの確認は、これで「いま動いているのと同じ
	// ブランチのビルド」だけを拾う（src/UpdaterFlow.cpp）。
	const std::string out = "build\tc0ffee1\tDev: feature/one (c0ffee1)\t"
							"https://ex.com/one.zip\tfeature/one\n";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
	{
		CHECK_EQ(builds[0].name, "Dev: feature/one (c0ffee1)");
		CHECK_EQ(builds[0].url, "https://ex.com/one.zip");
		CHECK_EQ(builds[0].branch, "feature/one");
	}
}

TEST(parsedevbuilds_without_the_branch_column_leaves_it_empty)
{
	// インストール済みの（＝古い）同梱スクリプトは 4 列しか出さない。**それも読める
	// こと**が要件で、ブランチが空なら黙って何もしない側へ倒れる。
	const std::string out = "build\tc1\tfeature/x\thttps://ex.com/a.zip\n";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
	{
		CHECK_EQ(builds[0].url, "https://ex.com/a.zip");
		CHECK_EQ(builds[0].branch, "");
	}
}

TEST(parsedevbuilds_trims_the_branch_column)
{
	const std::string out = "build\tc1\tn\thttps://ex.com/a.zip\t  feature/x  \n";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
		CHECK_EQ(builds[0].branch, "feature/x");
}

TEST(parsedevbuilds_ignores_non_build_lines)
{
	const std::string out = "error=\n"
							"note: this is not a build line\n"
							"build\tc1\tbranch\thttps://ex.com/a.zip\n";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
		CHECK_EQ(builds[0].commit, "c1");
}

TEST(parsedevbuilds_skips_rows_missing_fields)
{
	const std::string out = "build\tonlyonefield\n"						  // no tabs after -> skip
							"build\tc1\tbranch-no-url\n"				  // only two fields -> skip
							"build\tc2\tbranch\thttps://ex.com/ok.zip\n"; // complete -> keep
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
	{
		CHECK_EQ(builds[0].commit, "c2");
		CHECK_EQ(builds[0].url, "https://ex.com/ok.zip");
	}
}

TEST(parsedevbuilds_skips_row_with_empty_url)
{
	// Three tab-separated fields present, but the url field is blank -> skipped.
	const std::string out = "build\tc1\tbranch\t\n";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(0));
}

TEST(parsedevbuilds_trims_fields)
{
	const std::string out = "build\t  c1  \t  branch  \t  https://ex.com/a.zip  \n";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
	{
		CHECK_EQ(builds[0].commit, "c1");
		CHECK_EQ(builds[0].name, "branch");
		CHECK_EQ(builds[0].url, "https://ex.com/a.zip");
	}
}

TEST(parsedevbuilds_handles_last_row_without_newline)
{
	const std::string out = "build\tc1\tbranch\thttps://ex.com/a.zip";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
		CHECK_EQ(builds[0].url, "https://ex.com/a.zip");
}

TEST(parsedevbuilds_empty_and_no_matches)
{
	CHECK_EQ(ParseDevBuilds("").size(), static_cast<std::size_t>(0));
	CHECK_EQ(ParseDevBuilds("nothing here\nat all\n").size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// ShellQuote
// ---------------------------------------------------------------------------

TEST(shellquote_wraps_in_single_quotes)
{
	CHECK_EQ(ShellQuote("hello"), "'hello'");
	CHECK_EQ(ShellQuote("/path/to/thing"), "'/path/to/thing'");
	CHECK_EQ(ShellQuote(""), "''");
}

TEST(shellquote_escapes_embedded_single_quote)
{
	// The classic '\'' escape sequence: close, escaped quote, reopen.
	CHECK_EQ(ShellQuote("a'b"), "'a'\\''b'");
	CHECK_EQ(ShellQuote("'"), "''\\'''");
}

TEST(shellquote_passes_through_spaces_and_specials)
{
	// Inside single quotes these are all literal, so nothing extra is escaped.
	CHECK_EQ(ShellQuote("a b c"), "'a b c'");
	CHECK_EQ(ShellQuote("$HOME `x` \"y\""), "'$HOME `x` \"y\"'");
}

// ---------------------------------------------------------------------------
// CmdQuote
// ---------------------------------------------------------------------------

TEST(cmdquote_wraps_in_double_quotes)
{
	CHECK_EQ(CmdQuote("hello"), "\"hello\"");
	CHECK_EQ(CmdQuote("C:\\Plug-Ins\\x.vlb"), "\"C:\\Plug-Ins\\x.vlb\"");
	CHECK_EQ(CmdQuote(""), "\"\"");
}

TEST(cmdquote_drops_embedded_double_quotes)
{
	CHECK_EQ(CmdQuote("a\"b\"c"), "\"abc\"");
	CHECK_EQ(CmdQuote("\"\""), "\"\"");
}

TEST(cmdquote_keeps_spaces)
{
	CHECK_EQ(CmdQuote("a b"), "\"a b\"");
}

// ---------------------------------------------------------------------------
// MacScriptPathFromBinary
// ---------------------------------------------------------------------------

TEST(mac_script_path_from_binary_typical)
{
	const std::string bin = "/Users/me/Vectorworks/Plug-Ins/min-nano_structure.vwlibrary/Contents/"
							"MacOS/min-nano_structure";
	CHECK_EQ(MacScriptPathFromBinary(bin),
			 "/Users/me/Vectorworks/Plug-Ins/min-nano_structure.vwlibrary/Contents/Resources/"
			 "vw-update.sh");
}

TEST(mac_script_path_from_binary_uses_last_marker)
{
	// A stray "/Contents/MacOS/" earlier in the path must not fool rfind.
	const std::string bin = "/Contents/MacOS/weird/Plug-Ins/X.vwlibrary/Contents/MacOS/X";
	CHECK_EQ(MacScriptPathFromBinary(bin),
			 "/Contents/MacOS/weird/Plug-Ins/X.vwlibrary/Contents/Resources/vw-update.sh");
}

TEST(mac_script_path_from_binary_no_marker_is_empty)
{
	CHECK_EQ(MacScriptPathFromBinary("/some/other/path/binary"), "");
	CHECK_EQ(MacScriptPathFromBinary(""), "");
}

// ---------------------------------------------------------------------------
// MacPluginsDirFromBinary
// ---------------------------------------------------------------------------

TEST(mac_plugins_dir_from_binary_typical)
{
	const std::string bin = "/Users/me/Vectorworks/Plug-Ins/min-nano_structure.vwlibrary/Contents/"
							"MacOS/min-nano_structure";
	CHECK_EQ(MacPluginsDirFromBinary(bin), "/Users/me/Vectorworks/Plug-Ins");
}

TEST(mac_plugins_dir_from_binary_no_marker_is_empty)
{
	CHECK_EQ(MacPluginsDirFromBinary("/no/marker/here"), "");
	CHECK_EQ(MacPluginsDirFromBinary(""), "");
}

TEST(mac_plugins_dir_from_binary_bundle_at_root)
{
	// Bundle directly under root: stripping to the containing dir yields "".
	const std::string bin = "/X.vwlibrary/Contents/MacOS/X";
	CHECK_EQ(MacPluginsDirFromBinary(bin), "");
}

TEST(mac_plugins_dir_from_binary_no_leading_slash_is_empty)
{
	// The marker is present but nothing precedes the bundle name (no separator
	// before it), so there is no containing directory to return.
	const std::string bin = "X.vwlibrary/Contents/MacOS/X";
	CHECK_EQ(MacPluginsDirFromBinary(bin), "");
}

// ---------------------------------------------------------------------------
// WinModuleDirFromPath
// ---------------------------------------------------------------------------

TEST(win_module_dir_from_path_backslashes)
{
	CHECK_EQ(WinModuleDirFromPath("C:\\Users\\me\\Plug-Ins\\min-nano_structure.vlb"),
			 "C:\\Users\\me\\Plug-Ins");
}

TEST(win_module_dir_from_path_forward_slashes)
{
	CHECK_EQ(WinModuleDirFromPath("C:/Users/me/Plug-Ins/min-nano_structure.vlb"),
			 "C:/Users/me/Plug-Ins");
}

TEST(win_module_dir_from_path_no_separator_is_empty)
{
	CHECK_EQ(WinModuleDirFromPath("min-nano_structure.vlb"), "");
	CHECK_EQ(WinModuleDirFromPath(""), "");
}

// ---------------------------------------------------------------------------
// WinScriptPathFromDir
// ---------------------------------------------------------------------------

TEST(win_script_path_from_dir_appends_script)
{
	CHECK_EQ(WinScriptPathFromDir("C:\\Users\\me\\Plug-Ins"),
			 "C:\\Users\\me\\Plug-Ins\\vw-update.ps1");
}

TEST(win_script_path_from_dir_empty_is_empty)
{
	CHECK_EQ(WinScriptPathFromDir(""), "");
}

// ---------------------------------------------------------------------------
// EvaluateStable — the stable-channel "is there an update?" decision.
// ---------------------------------------------------------------------------

TEST(evaluate_stable_offers_when_newer)
{
	const std::string out = "installed=abc1234\n"
							"latest=def5678\n"
							"url=https://ex.com/min-nano_structure.vwlibrary.zip\n";
	StableStatus s = EvaluateStable(out);
	CHECK(s.offerUpdate);
	CHECK_EQ(s.installed, "abc1234");
	CHECK_EQ(s.latest, "def5678");
	CHECK_EQ(s.url, "https://ex.com/min-nano_structure.vwlibrary.zip");
}

TEST(evaluate_stable_silent_when_already_current)
{
	const std::string out = "installed=def5678\n"
							"latest=def5678\n"
							"url=https://ex.com/x.zip\n";
	StableStatus s = EvaluateStable(out);
	CHECK(!s.offerUpdate);
	// Fields are still populated even though no update is offered.
	CHECK_EQ(s.latest, "def5678");
}

TEST(evaluate_stable_silent_on_error_line)
{
	// An error= line means offline/transient: never offer, regardless of the
	// other fields (which the script would not have printed anyway).
	const std::string out = "error=stable リリースを取得できませんでした。\n";
	StableStatus s = EvaluateStable(out);
	CHECK(!s.offerUpdate);
	CHECK_EQ(s.latest, "");
	CHECK_EQ(s.url, "");
}

TEST(evaluate_stable_silent_when_incomplete)
{
	// latest present but url missing -> incomplete -> no offer.
	StableStatus a = EvaluateStable("installed=abc\nlatest=def\n");
	CHECK(!a.offerUpdate);
	// url present but latest missing -> incomplete -> no offer.
	StableStatus b = EvaluateStable("installed=abc\nurl=https://ex.com/x.zip\n");
	CHECK(!b.offerUpdate);
}

TEST(evaluate_stable_offers_with_no_installed_build)
{
	// First-ever install: nothing installed yet, but a build is published.
	const std::string out = "installed=none\n"
							"latest=def5678\n"
							"url=https://ex.com/x.zip\n";
	StableStatus s = EvaluateStable(out);
	CHECK(s.offerUpdate);
	CHECK_EQ(s.installed, "none");
}

// ---------------------------------------------------------------------------
// DevSwitchCandidates — parse dev builds and drop the running one.
// ---------------------------------------------------------------------------

TEST(dev_switch_candidates_excludes_running_commit)
{
	const std::string out = "installed=c0ffee1\n"
							"build\tc0ffee1\tmain\thttps://ex.com/a.zip\n" // running -> dropped
							"build\tdeadbee\tfeature/x\thttps://ex.com/b.zip\n"
							"build\tfeed123\tfeature/y\thttps://ex.com/c.zip\n";
	std::vector<DevBuild> others = DevSwitchCandidates(out, "c0ffee1");
	CHECK_EQ(others.size(), static_cast<std::size_t>(2));
	if (others.size() == 2)
	{
		// Order preserved; the running build is gone.
		CHECK_EQ(others[0].commit, "deadbee");
		CHECK_EQ(others[1].commit, "feed123");
	}
}

TEST(dev_switch_candidates_keeps_all_when_running_absent)
{
	const std::string out = "build\tc1\tbranch-a\thttps://ex.com/a.zip\n"
							"build\tc2\tbranch-b\thttps://ex.com/b.zip\n";
	// Running commit not among the builds (e.g. a local build) -> keep both.
	std::vector<DevBuild> others = DevSwitchCandidates(out, "local");
	CHECK_EQ(others.size(), static_cast<std::size_t>(2));
}

TEST(dev_switch_candidates_empty_when_no_builds)
{
	CHECK_EQ(DevSwitchCandidates("installed=none\n", "local").size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// DevBuildBranch / FindDevBuildForBranch — pick the SAME branch's next build.
// 実機フィードバックの往復（docs/DEV-NOTES.md M23）が、修正版のビルドを待つときに使う。
// ---------------------------------------------------------------------------

TEST(dev_build_branch_reads_the_ci_title)
{
	// CI が付ける題は "Dev: <branch> (<short sha>)"（.github/workflows/build.yml）。
	CHECK_EQ(DevBuildBranch("Dev: claude/feedback (a1b2c3d)"), "claude/feedback");
	// ブランチ名に丸括弧やスペースが入っても、**最後の** " (" で切る。
	CHECK_EQ(DevBuildBranch("Dev: feature/x (2) (a1b2c3d)"), "feature/x (2)");
}

TEST(dev_build_branch_is_empty_when_the_title_does_not_match)
{
	// 題の形が違う（古いリリース・題を変えた）ときは空。**これは 5 列目が無いときの
	// 保険なので、当てずっぽうのブランチ名を作らない**——空なら「分からない」として
	// 何も拾わない側へ倒れる（FindDevBuildForBranch）。
	CHECK_EQ(DevBuildBranch("feature/x"), "");
	CHECK_EQ(DevBuildBranch("Dev: "), "");
	CHECK_EQ(DevBuildBranch(""), "");
}

TEST(parse_dev_builds_fills_the_branch_from_the_title_when_the_column_is_missing)
{
	// **古い同梱スクリプト（4 列）でも「同じブランチの新しいビルド」を拾える。**
	// ここを空のまま通すと、古いスクリプトが入っている間だけ、取り込み時の確認も
	// 実機フィードバックの往復も黙って死ぬ（UpdaterParse.h の ParseDevBuilds）。
	const std::string out = "build\tfeed123\tDev: feature/x (feed123)\thttps://ex.com/c.zip\n";
	const std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
		CHECK_EQ(builds[0].branch, "feature/x");
}

TEST(parse_dev_builds_prefers_the_branch_column_over_the_title)
{
	// 5 列目があるならそちらが正。題は表示用でしかない。
	const std::string out = "build\tfeed123\tDev: 題は当てにしない (feed123)\t"
							"https://ex.com/c.zip\treal/branch\n";
	const std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(1));
	if (!builds.empty())
		CHECK_EQ(builds[0].branch, "real/branch");
}

TEST(find_dev_build_for_branch_picks_only_that_branch)
{
	const std::string out = "installed=c0ffee1\n"
							"build\tdeadbee\tDev: feature/y (deadbee)\thttps://ex.com/b.zip\n"
							"build\tfeed123\tDev: feature/x (feed123)\thttps://ex.com/c.zip\n";
	const std::vector<DevBuild> builds = DevSwitchCandidates(out, "c0ffee1");
	const int index = FindDevBuildForBranch(builds, "feature/x");
	CHECK_EQ(index, 1);
	if (index >= 0)
		CHECK_EQ(builds[static_cast<std::size_t>(index)].commit, "feed123");

	// **他のブランチのビルドへ乗り換えない**（絞らないとここが -1 にならない）。
	CHECK_EQ(FindDevBuildForBranch(builds, "no-such-branch"), -1);
	// ブランチが分からないとき（ローカルビルド）は何も選ばない。
	CHECK_EQ(FindDevBuildForBranch(builds, ""), -1);
}

TEST(find_dev_build_for_branch_takes_the_first_match)
{
	// GitHub は新しい順に返すので、同じブランチが 2 つ並んだら先頭が新しい。
	const std::string out = "build\tnewer11\tDev: feature/x (newer11)\thttps://ex.com/n.zip\n"
							"build\tolder22\tDev: feature/x (older22)\thttps://ex.com/o.zip\n";
	const std::vector<DevBuild> builds = DevSwitchCandidates(out, "running");
	CHECK_EQ(FindDevBuildForBranch(builds, "feature/x"), 0);
}

// ---------------------------------------------------------------------------
// ResolveDevSelection — map a picker index back to a candidate.
// ---------------------------------------------------------------------------

TEST(resolve_dev_selection_zero_keeps_current)
{
	// Entry 0 is the installed build -> "keep current" -> -1.
	CHECK_EQ(ResolveDevSelection(0, 3), -1);
}

TEST(resolve_dev_selection_negative_keeps_current)
{
	// No/!invalid selection defensively maps to "keep current".
	CHECK_EQ(ResolveDevSelection(-1, 3), -1);
}

TEST(resolve_dev_selection_maps_to_candidate_index)
{
	// Picker entry 1 -> candidate 0, entry 3 -> candidate 2.
	CHECK_EQ(ResolveDevSelection(1, 3), 0);
	CHECK_EQ(ResolveDevSelection(3, 3), 2);
}

TEST(resolve_dev_selection_out_of_range_keeps_current)
{
	// Selection past the last candidate -> safeguard -> -1.
	CHECK_EQ(ResolveDevSelection(4, 3), -1);
	// No candidates at all: any positive selection is out of range.
	CHECK_EQ(ResolveDevSelection(1, 0), -1);
}

// ---------------------------------------------------------------------------
// InstallReportedOk / InstallErrorText — interpret do-install output.
// ---------------------------------------------------------------------------

TEST(install_reported_ok_true_for_ok)
{
	CHECK(InstallReportedOk("ok"));
	CHECK(InstallReportedOk("  ok \n")); // Trim tolerates surrounding whitespace
}

TEST(install_reported_ok_false_otherwise)
{
	CHECK(!InstallReportedOk(""));
	CHECK(!InstallReportedOk("error=ダウンロードに失敗しました。\n"));
	CHECK(!InstallReportedOk("okay")); // 行として "ok" ちょうどでなければならない
}

TEST(install_error_text_uses_script_error)
{
	const std::string out = "error=ダウンロードに失敗しました。\n";
	CHECK_EQ(InstallErrorText(out, "fallback"), "ダウンロードに失敗しました。");
}

TEST(install_error_text_falls_back_when_no_error)
{
	// No error= line (e.g. garbled output): use the caller's fallback wording.
	CHECK_EQ(InstallErrorText("something unexpected\n", "fallback"), "fallback");
	CHECK_EQ(InstallErrorText("", "fallback"), "fallback");
}

// ---------------------------------------------------------------------------
// ホットリロードの判断材料 — InstalledShellId / NeedsRestartAfterInstall
//
// プラグインは殻と本体に割れていて、Vectorworks が起動時にしか読み込めないのは殻だけ
// （src/PayloadAbi.h）。**入れたビルドの殻が同じなら再起動は要らない**——その 1 点を
// 決めるのがこの 2 つ。
// ---------------------------------------------------------------------------

TEST(install_reported_ok_accepts_extra_lines_before_ok)
{
	// do-install は "ok" の前に installed-shell= を出す。**その行があっても成功と読む**
	// ——出力全体が "ok" であることを求めていた頃の書き方だと、成功が失敗に化ける。
	CHECK(InstallReportedOk("installed-shell=abc123def456\nok\n"));
	CHECK(InstallReportedOk("installed-shell=abc123def456\nok"));
	// それでも error= だけの出力は失敗のまま。
	CHECK(!InstallReportedOk("installed-shell=abc123def456\nerror=だめでした。\n"));
}

TEST(installed_shell_id_reads_the_line)
{
	CHECK_EQ(InstalledShellId("installed-shell=abc123def456\nok\n"), "abc123def456");
	// 行が無ければ空（この行を出さない古いスクリプト）。
	CHECK_EQ(InstalledShellId("ok\n"), "");
	CHECK_EQ(InstalledShellId(""), "");
	// installed= と取り違えない（前方一致ではなくキーの一致で引く）。
	CHECK_EQ(InstalledShellId("installed=abc1234\nok\n"), "");
}

TEST(needs_restart_is_false_only_when_both_shells_are_known_and_equal)
{
	// 殻が同じ＝本体だけが新しい → 読み直すだけでよい。
	CHECK(!NeedsRestartAfterInstall("abc123def456", "abc123def456"));
	// 殻が違う → 次の起動でしか読み込めない。
	CHECK(NeedsRestartAfterInstall("abc123def456", "cccc2222dddd"));
	// **判断できないときは必ず「要る」へ倒す。** 食い違ったまま動かすほうが危ない。
	CHECK(NeedsRestartAfterInstall("abc123def456", ""));
	CHECK(NeedsRestartAfterInstall("", "abc123def456"));
	CHECK(NeedsRestartAfterInstall("", ""));
}

// ---------------------------------------------------------------------------

TEST_MAIN();
