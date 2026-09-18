//
//	ParseRegressionTests.cpp
//
//	回帰テストの結果・基準・突き合わせ（src/parse/Regression.h）の単体テスト。
//	**Vectorworks も実物の物件も要らない**——ここが押さえるのは「基準を書いて読み戻せるか」
//	「何を『動いた』と見るか」「基準に依らない異常を別に数えているか」だけである。
//	フォルダを走査して 1 件ずつ取り込む運転（draw/Regression）は SDK 側なので、ここには
//	入らない（実機で確かめる）。
//

#include "TestFramework.h"
#include "core/Document.h"
#include "parse/Regression.h"
#include "parse/Summary.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::DrawCounts;
using HomeskzIfcImport::core::GridCommand;
using HomeskzIfcImport::parse::BuildInfo;
using HomeskzIfcImport::parse::compareRegression;
using HomeskzIfcImport::parse::formatRegressionBaseline;
using HomeskzIfcImport::parse::formatRegressionLog;
using HomeskzIfcImport::parse::formatRegressionPrompt;
using HomeskzIfcImport::parse::formatRegressionResult;
using HomeskzIfcImport::parse::parseRegressionBaseline;
using HomeskzIfcImport::parse::readRegressionBaseline;
using HomeskzIfcImport::parse::regressionBaselineOrigin;
using HomeskzIfcImport::parse::regressionBaselinePath;
using HomeskzIfcImport::parse::RegressionCompare;
using HomeskzIfcImport::parse::RegressionEntry;
using HomeskzIfcImport::parse::regressionEntry;
using HomeskzIfcImport::parse::regressionErrorEntry;
using HomeskzIfcImport::parse::RegressionSummary;
using HomeskzIfcImport::parse::RegressionVerdict;
using HomeskzIfcImport::parse::summarizeRegression;
using HomeskzIfcImport::parse::writeRegressionBaseline;

namespace
{
	// 通り芯を n 本だけ持つ最小の命令セット（要素は何でもよく、数が動くことだけが要る）。
	Document GridsOnly(std::size_t count)
	{
		Document document;
		for (std::size_t i = 0; i < count; ++i)
			document.grids.push_back(GridCommand{});
		return document;
	}

	DrawCounts DrewGrids(std::size_t count)
	{
		DrawCounts counts;
		counts.valid = true;
		counts.grids = count;
		return counts;
	}

	BuildInfo SampleBuild()
	{
		BuildInfo build;
		build.plugin = "min-nano_structureDev";
		build.channel = "dev";
		build.commit = "abc1234";
		build.branch = "feature/x";
		build.platform = "macOS";
		return build;
	}

	bool Contains(const std::string& text, const std::string& needle)
	{
		return text.find(needle) != std::string::npos;
	}

	// テスト 1 件ぶんの作業ディレクトリ（作って、抜けるときに消す）。書き先はビルドツリー
	// ——TMPDIR 由来のパスがファイル操作へ届くと CodeQL が cpp/path-injection として報告する
	// ので、CoreTraceTests / CoreBridgeTests と同じ作法にする。
	class TempDir
	{
	public:
		explicit TempDir(const std::string& tag)
		{
			fPath = std::filesystem::path(HOMESKZ_REGRESSION_TEST_DIR) / ("vw-reg-" + tag);
			std::error_code ec;
			std::filesystem::remove_all(fPath, ec);
			std::filesystem::create_directories(fPath, ec);
		}
		~TempDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(fPath, ec);
		}
		TempDir(const TempDir&) = delete;
		TempDir& operator=(const TempDir&) = delete;

		std::string utf8() const
		{
			const std::u8string text = fPath.u8string();
			return {text.begin(), text.end()};
		}

	private:
		std::filesystem::path fPath;
	};
} // namespace

// ---------------------------------------------------------------------------
// 1 件ぶんの結果は、取り込みの報告と**同じ結末・同じ内訳**を名乗る。
TEST(entry_uses_the_import_wording)
{
	const RegressionEntry ok = regressionEntry("物件.ifc", GridsOnly(3), DrewGrids(3), 12.5);
	CHECK_EQ(ok.name, "物件.ifc");
	CHECK_EQ(ok.status, "成功");
	CHECK_EQ(ok.tally, "通り芯:3/3");
	CHECK(ok.seconds > 12.0);

	// 描き切れなければ「問題あり」（基準がどうであれ、これは異常である）。
	const RegressionEntry warned = regressionEntry("物件.ifc", GridsOnly(3), DrewGrids(2), 1.0);
	CHECK_EQ(warned.status, "問題あり");
	CHECK_EQ(warned.tally, "通り芯:2/3");

	// **描画側が持ち帰った異常は 1 行に畳んで残す**（行数で切る——UTF-8 の途中で切らない）。
	// **CR も空行も持ち込ませない**（診断は描画側が改行で連ねたもの。CRLF で来ることも
	// あれば、空行が挟まることもある）。
	DrawCounts noisy = DrewGrids(3);
	noisy.diagnostics = "一行目\r\n\r\n二行目\n三行目\n四行目\n";
	const RegressionEntry withNotes = regressionEntry("物件.ifc", GridsOnly(3), noisy, 1.0);
	CHECK_EQ(withNotes.status, "問題あり"); // diagnostics があれば命令を描き切っても異常
	CHECK_EQ(withNotes.detail, "一行目 / 二行目 / 三行目 …");

	const RegressionEntry failed = regressionErrorEntry("物件.ifc", "bad_alloc", 0.5);
	CHECK_EQ(failed.status, "エラー");
	CHECK_EQ(failed.detail, "bad_alloc");
	CHECK(failed.tally.empty());

	// **エラーの説明にも改行を持ち込ませない**（渡されるのは取り込みのエラー本文＝複数行。
	// そのまま入れると、基準ファイルの次の行が鍵として読まれて記録が壊れる）。
	const RegressionEntry multiline =
		regressionErrorEntry("物件.ifc", "取り込みに失敗しました\n詳細: bad_alloc\n", 0.5);
	CHECK_EQ(multiline.detail, "取り込みに失敗しました / 詳細: bad_alloc");
}

// 基準は書いて読み戻すと元に戻る。
TEST(baseline_round_trips)
{
	std::vector<RegressionEntry> entries;
	entries.push_back(regressionEntry("a.ifc", GridsOnly(3), DrewGrids(3), 61.0));
	entries.push_back(regressionErrorEntry("b=c.ifc", "何か", 0.4));

	const std::string text =
		formatRegressionBaseline(entries, SampleBuild(), "2026-09-18 12:34:56");
	const std::vector<RegressionEntry> read = parseRegressionBaseline(text);

	CHECK_EQ(read.size(), std::size_t{2});
	if (read.size() == 2)
	{
		CHECK_EQ(read[0].name, "a.ifc");
		CHECK_EQ(read[0].status, "成功");
		CHECK_EQ(read[0].tally, "通り芯:3/3");
		CHECK(read[0].seconds > 60.0);
		// **名前に `=` が入っていても壊れない**（最初の `=` で割る）。
		CHECK_EQ(read[1].name, "b=c.ifc");
		CHECK_EQ(read[1].status, "エラー");
		CHECK_EQ(read[1].detail, "何か");
	}

	// 基準の素性（いつ・どのビルドで採ったか）。
	const std::string origin = regressionBaselineOrigin(text);
	CHECK(Contains(origin, "2026-09-18 12:34:56"));
	CHECK(Contains(origin, "abc1234"));
	CHECK(Contains(origin, "feature/x"));
}

// 基準ファイルの置き場所と、その読み書き。**無くても異常ではない**（初めて走らせたとき）。
TEST(baseline_file_round_trips_on_disk)
{
	const TempDir temp("io");
	const std::string path = regressionBaselinePath(temp.utf8());
	CHECK(Contains(path, "min-nano_structure-regression.txt"));
	CHECK(regressionBaselinePath("").empty());

	// まだ無いので読めない（呼び出し側は「基準が無い」として 1 回目のように続ける）。
	std::string text = "残っていてはいけない";
	CHECK(!readRegressionBaseline(path, text));
	CHECK(text.empty());
	CHECK(!readRegressionBaseline("", text));

	std::vector<RegressionEntry> entries;
	entries.push_back(regressionEntry("a.ifc", GridsOnly(2), DrewGrids(2), 1.0));
	const std::string written =
		formatRegressionBaseline(entries, SampleBuild(), "2026-09-18 00:00:00");
	CHECK(writeRegressionBaseline(path, written));
	CHECK(readRegressionBaseline(path, text));
	CHECK_EQ(text, written);
	CHECK(!writeRegressionBaseline("", written));

	// 書けない場所は false（呼び出し側は「基準を書けませんでした」と伝える）。
	CHECK(!writeRegressionBaseline(temp.utf8() + "/無いフォルダ/基準.txt", written));
}

// 壊れた行・知らない鍵は飛ばして読み続ける（古い版が書いたものも読める）。
TEST(baseline_skips_broken_lines)
{
	const std::string text = "# コメント\n"
							 "version=1\n"
							 "これは鍵でも値でもない\n"
							 "file=a.ifc\n"
							 "status=成功\n"
							 "未来の鍵=知らない値\n"
							 "tally=通り芯:1/1\n"
							 "\n"
							 "file=\n" // 名前が無い件は採らない
							 "file=b.ifc\n"
							 "status=問題あり\n";
	const std::vector<RegressionEntry> read = parseRegressionBaseline(text);
	CHECK_EQ(read.size(), std::size_t{2});
	if (read.size() == 2)
	{
		CHECK_EQ(read[0].name, "a.ifc");
		CHECK_EQ(read[0].tally, "通り芯:1/1");
		CHECK_EQ(read[1].name, "b.ifc");
	}
}

// 改行が CRLF でも、秒が数として読めなくても、読むのをやめない（基準ファイルは利用者の
// 機械で作られるので、Windows で書いて mac で読む——その逆も——が普通に起きる）。
TEST(baseline_survives_crlf_and_bad_numbers)
{
	const std::string text = "version=1\r\n"
							 "file=a.ifc\r\n"
							 "status=成功\r\n"
							 "seconds=これは数ではない\r\n"
							 "tally=通り芯:1/1\r\n";
	const std::vector<RegressionEntry> read = parseRegressionBaseline(text);
	CHECK_EQ(read.size(), std::size_t{1});
	if (!read.empty())
	{
		CHECK_EQ(read[0].status, "成功");	   // CR が値に残っていない
		CHECK_EQ(read[0].tally, "通り芯:1/1"); //
		CHECK(read[0].seconds == 0.0);		   // 読めなければ 0。読むのはやめない
	}
}

// 基準の素性が読めないときは空（見出しの無いファイル・壊れた行だけのファイル）。
TEST(baseline_origin_is_empty_without_a_header)
{
	CHECK(regressionBaselineOrigin("").empty());
	CHECK(regressionBaselineOrigin("これは鍵でも値でもない\nfile=a.ifc\nstatus=成功\n").empty());
	// 見出しはあるが記録した日時しか無い、という古い形でも読める。**改行が CRLF でも**
	// （基準ファイルは利用者の機械で作られるので、Windows で書いて mac で読むが起きる）。
	CHECK_EQ(regressionBaselineOrigin("recorded=2026-09-18\r\nfile=a.ifc\r\n"), "2026-09-18");
}

// 突き合わせ: 同じ・動いた・基準に無し・今回は走らず。
TEST(compare_classifies_each_file)
{
	std::vector<RegressionEntry> baseline;
	baseline.push_back(regressionEntry("same.ifc", GridsOnly(3), DrewGrids(3), 1.0));
	baseline.push_back(regressionEntry("moved.ifc", GridsOnly(3), DrewGrids(3), 1.0));
	baseline.push_back(regressionEntry("gone.ifc", GridsOnly(1), DrewGrids(1), 1.0));

	std::vector<RegressionEntry> current;
	current.push_back(regressionEntry("same.ifc", GridsOnly(3), DrewGrids(3), 1.0));
	current.push_back(regressionEntry("moved.ifc", GridsOnly(5), DrewGrids(5), 1.0));
	current.push_back(regressionEntry("new.ifc", GridsOnly(2), DrewGrids(2), 1.0));

	const std::vector<RegressionCompare> compares = compareRegression(baseline, current);
	CHECK_EQ(compares.size(), std::size_t{4});
	if (compares.size() != 4)
		return;

	CHECK_EQ(compares[0].name, "same.ifc");
	CHECK(compares[0].verdict == RegressionVerdict::Same);
	CHECK(compares[0].diff.empty());

	CHECK_EQ(compares[1].name, "moved.ifc");
	CHECK(compares[1].verdict == RegressionVerdict::Changed);
	CHECK(Contains(compares[1].diff, "通り芯: 3/3 → 5/5"));

	CHECK_EQ(compares[2].name, "new.ifc");
	CHECK(compares[2].verdict == RegressionVerdict::Added);

	// **基準にあるのに走らなかったものは最後に必ず出る**（リンク切れ・改名に気付く唯一の場）。
	CHECK_EQ(compares[3].name, "gone.ifc");
	CHECK(compares[3].verdict == RegressionVerdict::Missing);
	CHECK(!compares[3].abnormal); // 走っていないものに異常も正常も無い

	const RegressionSummary summary =
		summarizeRegression(compares, /*baselineKnown*/ true, /*cancelled*/ false);
	CHECK_EQ(summary.total, std::size_t{3});
	CHECK_EQ(summary.same, std::size_t{1});
	CHECK_EQ(summary.changed, std::size_t{1});
	CHECK_EQ(summary.added, std::size_t{1});
	CHECK_EQ(summary.missing, std::size_t{1});
	CHECK_EQ(summary.abnormal, std::size_t{0});
}

// **基準がその異常ごと記録されていても、異常は異常として数える。** 「基準どおり」だけを
// 見ていると、ずっと壊れたままのものが永久に見えなくなる。
TEST(compare_counts_abnormal_even_when_unchanged)
{
	std::vector<RegressionEntry> baseline;
	baseline.push_back(regressionEntry("warn.ifc", GridsOnly(3), DrewGrids(2), 1.0));
	std::vector<RegressionEntry> current;
	current.push_back(regressionEntry("warn.ifc", GridsOnly(3), DrewGrids(2), 1.0));

	const std::vector<RegressionCompare> compares = compareRegression(baseline, current);
	CHECK_EQ(compares.size(), std::size_t{1});
	if (compares.empty())
		return;
	CHECK(compares[0].verdict == RegressionVerdict::Same);
	CHECK(compares[0].abnormal);

	const RegressionSummary summary = summarizeRegression(compares, true, false);
	CHECK_EQ(summary.same, std::size_t{1});
	CHECK_EQ(summary.abnormal, std::size_t{1});
}

// 結末だけが変わった（内訳は同じ）ときも「動いた」。
TEST(compare_notices_a_status_change)
{
	std::vector<RegressionEntry> baseline;
	baseline.push_back(regressionErrorEntry("x.ifc", "落ちた", 0.1));
	std::vector<RegressionEntry> current;
	current.push_back(regressionEntry("x.ifc", GridsOnly(2), DrewGrids(2), 1.0));

	const std::vector<RegressionCompare> compares = compareRegression(baseline, current);
	CHECK_EQ(compares.size(), std::size_t{1});
	if (compares.empty())
		return;
	CHECK(compares[0].verdict == RegressionVerdict::Changed);
	CHECK_EQ(compares[0].statusBefore, "エラー");
	CHECK_EQ(compares[0].status, "成功");
	// **基準が空でも黙らない**（formatTallyDiff は前回が空なら空を返すので、ここで補う）。
	CHECK(Contains(compares[0].diff, "（基準は空）"));
}

// 走らせる前の問い: 基準の有無で言うことが変わり、待ち時間を必ず先に言う。
TEST(prompt_says_what_will_happen)
{
	const std::string first = formatRegressionPrompt(12, 0, "");
	CHECK(Contains(first, "12 件"));
	CHECK(Contains(first, "基準として記録します"));
	CHECK(Contains(first, "時間がかかります"));
	CHECK(Contains(first, "もとのファイルは変わりません"));

	const std::string again = formatRegressionPrompt(12, 10, "2026-09-18 / abc1234 (main)");
	CHECK(Contains(again, "引き比べます"));
	CHECK(Contains(again, "abc1234"));
}

// 結果ダイアログの本文は短く、読むのは 3 つだけ（何件・動いたか・異常）。
TEST(result_body_is_short)
{
	RegressionSummary summary;
	summary.total = 5;
	summary.same = 3;
	summary.changed = 1;
	summary.added = 1;
	summary.abnormal = 2;
	summary.baselineKnown = true;

	const std::string body = formatRegressionResult(summary, /*baselineWritten*/ false);
	CHECK(Contains(body, "5 件"));
	CHECK(Contains(body, "動いた 1 件"));
	CHECK(Contains(body, "「成功」でなかった"));
	CHECK(!Contains(body, "基準を今回の結果で更新"));

	RegressionSummary fresh;
	fresh.total = 2;
	fresh.added = 2;
	fresh.baselineKnown = false;
	const std::string firstRun = formatRegressionResult(fresh, /*baselineWritten*/ true);
	CHECK(Contains(firstRun, "基準として記録しました"));

	// 基準を書けなかったことを黙らない（次回また「基準がありません」になるので）。
	const std::string unwritten = formatRegressionResult(fresh, /*baselineWritten*/ false);
	CHECK(Contains(unwritten, "基準を書けませんでした"));
}

// ログには件ごとの結末・差分・走査で気付いたことが載る。
TEST(log_carries_the_details)
{
	std::vector<RegressionEntry> baseline;
	baseline.push_back(regressionEntry("moved.ifc", GridsOnly(3), DrewGrids(3), 1.0));
	std::vector<RegressionEntry> current;
	current.push_back(regressionEntry("moved.ifc", GridsOnly(4), DrewGrids(4), 65.0));

	const std::vector<RegressionCompare> compares = compareRegression(baseline, current);
	const RegressionSummary summary = summarizeRegression(compares, true, false);
	const std::vector<std::string> notes = {"ショートカットを辿れませんでした: 切れ.lnk"};

	const std::string log = formatRegressionLog(compares, summary, notes);
	CHECK(Contains(log, "moved.ifc"));
	CHECK(Contains(log, "動いた"));
	CHECK(Contains(log, "通り芯: 3/3 → 4/4"));
	CHECK(Contains(log, "1 分 5 秒")); // 所要の言い方は parse/Summary と同じもの
	CHECK(Contains(log, "切れ.lnk"));
}

// 中止した回・基準を書いた回の言い方（走らせた人が最初に読む 1 枚）。
TEST(result_body_names_the_unusual_runs)
{
	RegressionSummary stopped;
	stopped.total = 2;
	stopped.same = 2;
	stopped.baselineKnown = true;
	stopped.cancelled = true;
	const std::string body = formatRegressionResult(stopped, /*baselineWritten*/ false);
	CHECK(Contains(body, "途中で中止した"));

	RegressionSummary updated;
	updated.total = 3;
	updated.same = 3;
	updated.baselineKnown = true;
	const std::string refreshed = formatRegressionResult(updated, /*baselineWritten*/ true);
	CHECK(Contains(refreshed, "基準を今回の結果で更新しました"));
}

// ログは 4 つの分類すべてに言葉を持ち、結末が変わった件は基準の結末も並べる。
TEST(log_names_every_verdict)
{
	std::vector<RegressionEntry> baseline;
	baseline.push_back(regressionEntry("same.ifc", GridsOnly(2), DrewGrids(2), 1.0));
	baseline.push_back(regressionEntry("gone.ifc", GridsOnly(2), DrewGrids(2), 1.0));
	baseline.push_back(regressionEntry("worse.ifc", GridsOnly(2), DrewGrids(2), 1.0));

	std::vector<RegressionEntry> current;
	current.push_back(regressionEntry("same.ifc", GridsOnly(2), DrewGrids(2), 1.0));
	current.push_back(regressionEntry("new.ifc", GridsOnly(1), DrewGrids(1), 1.0));
	current.push_back(regressionErrorEntry("worse.ifc", "落ちた", 0.2));

	const std::vector<RegressionCompare> compares = compareRegression(baseline, current);
	const RegressionSummary summary =
		summarizeRegression(compares, /*baselineKnown*/ true, /*cancelled*/ true);
	const std::string log = formatRegressionLog(compares, summary, {});

	CHECK(Contains(log, "基準どおり"));
	CHECK(Contains(log, "基準に無し"));
	CHECK(Contains(log, "今回は走らず"));
	CHECK(Contains(log, "途中で中止"));
	// **結末が変わった件は、基準の結末も並べる**（「問題あり → 成功」も動きである）。
	CHECK(Contains(log, "（基準は 成功）"));
	CHECK(Contains(log, "詳細: 落ちた"));
}

TEST_MAIN();
