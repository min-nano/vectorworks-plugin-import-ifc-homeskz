//
//	CoreFeedbackSessionTests.cpp
//
//	実機テストの記憶（src/core/FeedbackSession）の単体テスト。VectorWorks SDK を
//	一切 include せず、無 SDK のテストハーネスで実行する（CLAUDE.md「テスト方針」）。
//
//	検証項目（docs/DEV-NOTES.md M23）: 既定は「何もしない」・書いて読んで元に戻る・
//	壊れた行を読み飛ばして読み続ける・ファイルへの読み書き。ここを壊さないこと——
//	**2 周目が実行されるかどうかはこの記憶だけで決まる**（MCP の vw_run_test は
//	ダイアログを出せない）。
//

#include "TestFramework.h"

#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using HomeskzIfcImport::core::clearFeedbackSession;
using HomeskzIfcImport::core::defaultFeedbackSessionPath;
using HomeskzIfcImport::core::FeedbackRoundKind;
using HomeskzIfcImport::core::feedbackRoundKind;
using HomeskzIfcImport::core::FeedbackSession;
using HomeskzIfcImport::core::feedbackSessionRemembered;
using HomeskzIfcImport::core::formatFeedbackSession;
using HomeskzIfcImport::core::isOwnedTestDocument;
using HomeskzIfcImport::core::kSymbolRoleCount;
using HomeskzIfcImport::core::parseFeedbackSession;
using HomeskzIfcImport::core::readFeedbackSession;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::testReportPathFor;
using HomeskzIfcImport::core::writeFeedbackSession;

namespace
{
	// 一通り埋めた記憶（周をまたいで実際に引き継ぐ値のすべて）。
	FeedbackSession sample()
	{
		FeedbackSession session;
		session.ifcPath = "/Users/someone/Documents/物件A.ifc";
		session.templatePath = "/tmp/homeskz-test/main/template-1.sta";
		session.ownedDocuments = {"/tmp/homeskz-test/main/template-1.sta",
								  "/tmp/homeskz-test/main/round-3-1.vwx"};
		session.round = 3;
		session.lastCommit = "a1b2c3d";
		session.lastTally = "ストーリ:3/3,通り芯:44/44";
		session.baselineRecorded = true;
		session.baselineLayers = {"共通", "デザイン レイヤ-1"};
		session.options.setSymbol(SymbolRole::FloorPost, "床束（特注）");
		session.options.setEnabled(SymbolRole::FireBrace, false);
		session.options.setTitleBlockStyle("図面枠 A3（構造）");
		session.options.setDimensionStandard("構造図 寸法");
		session.options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}, true);
		session.options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{2, 6374}, true);
		// M34 軸組図から除外する通り（重なった図番の "(2)" も含めて）。
		session.options.setSkippedSections({"X1", "1(2)", "又い"});
		// 垂木の断面（端数付きの寸法も引き継げること）。
		session.options.setRafterSize(60.5, 105.0);
		return session;
	}

	// テスト用の書き出し先（同じ名前を使い回さない）。
	std::string tempPath(const char* name)
	{
		std::error_code ec;
		const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
		return (dir / name).string();
	}

	// 環境変数の設定と解除。**置き場所は環境変数だけで決まる**ので、これが無いと
	// その分岐（Windows 流・macOS 流・どちらも取得できない）を確認できない。綴りが
	// 処理系で異なるのでここに閉じ込める（core/Trace が getenv を 1 か所へ閉じ込めて
	// いるのと同じ）。
	void setEnv(const char* name, const char* value)
	{
#if defined(_WIN32)
		_putenv_s(name, value != nullptr ? value : "");
#else
		if (value != nullptr)
			setenv(name, value, 1);
		else
			unsetenv(name);
#endif
	}

	// 環境変数を 1 つ保存し、スコープを抜けるときに元へ戻す（他のテストへ影響させない）。
	class ScopedEnv
	{
	public:
		ScopedEnv(const char* name, const char* value) : fName(name)
		{
			const char* const previous = std::getenv(name);
			fHad = previous != nullptr;
			if (fHad)
				fPrevious = previous;
			setEnv(name, value);
		}
		~ScopedEnv()
		{
			setEnv(fName, fHad ? fPrevious.c_str() : nullptr);
		}
		ScopedEnv(const ScopedEnv&) = delete;
		ScopedEnv& operator=(const ScopedEnv&) = delete;

	private:
		const char* fName;
		std::string fPrevious;
		bool fHad = false;
	};
} // namespace

TEST(feedback_session_defaults_do_nothing)
{
	// 記憶が無いとき（＝1 周目）にそのまま使っても、従来どおりの手動の取り込みになる。
	const FeedbackSession session;
	CHECK_EQ(session.round, 0);
	CHECK(session.ifcPath.empty());
	CHECK(!feedbackSessionRemembered(session));
}

TEST(feedback_session_without_a_baseline_reads_as_not_recorded)
{
	// 古い版が書いた記憶（baseline の行が無い）。**空の基準と区別する**——
	// 混同すると、基準を採る前の周を「戻っています」と報告してしまう。
	const FeedbackSession session = parseFeedbackSession("round=2\nbuild=a1b2c3d\n");
	CHECK(!session.baselineRecorded);
	CHECK(session.baselineLayers.empty());
	// テンプレートの行（M39）も無い。**空＝基準が無い**ので、MCP の周は実行されず、
	// メニューの周はいま開いている図面から採り直す。閉じる相手も無い。
	CHECK(session.templatePath.empty());
	CHECK(session.ownedDocuments.empty());
}

TEST(feedback_session_drops_the_m38_rollback_lines)
{
	// M38 までの記憶（作業ファイル・前の周が作ったレイヤ）。**警告せずに読み飛ばす**——作業
	// ファイル（.vwx）は開くとそのファイル自体が開くので、テンプレートの代わりにしない。
	const FeedbackSession session =
		parseFeedbackSession("round=4\nifc=/tmp/a.ifc\nwork=/tmp/homeskz-test/main/work-1.vwx\n"
							 "created.layer=1-伏図\ncreated.sheet=A-1\n");
	CHECK_EQ(session.round, 4);
	CHECK(session.templatePath.empty());
	CHECK(session.ownedDocuments.empty());
	const std::string text = formatFeedbackSession(session);
	CHECK(text.find("work=") == std::string::npos);
	CHECK(text.find("created.") == std::string::npos);
}

TEST(feedback_session_keeps_an_empty_baseline_distinct_from_none)
{
	// 空の図面で始めた 1 周目は「基準は採ったが 0 枚」。真偽が無いと区別できない。
	FeedbackSession empty;
	empty.baselineRecorded = true;
	const FeedbackSession after = parseFeedbackSession(formatFeedbackSession(empty));
	CHECK(after.baselineRecorded);
	CHECK(after.baselineLayers.empty());
}

TEST(feedback_session_round_trips_through_text)
{
	const FeedbackSession before = sample();
	const FeedbackSession after = parseFeedbackSession(formatFeedbackSession(before));

	CHECK_EQ(after.ifcPath, before.ifcPath);
	// **毎周開くテンプレート**（M39）。欠けると MCP の周が実行されなくなる。
	CHECK_EQ(after.templatePath, before.templatePath);
	// **自分で保存した図面**（M39）。欠けると次の周で閉じられず、周の数だけ増えていく
	// （再起動のときに保存の確認が並ぶ）。
	CHECK_EQ(after.ownedDocuments.size(), before.ownedDocuments.size());
	for (std::size_t i = 0; i < before.ownedDocuments.size(); ++i)
		CHECK_EQ(after.ownedDocuments[i], before.ownedDocuments[i]);
	CHECK_EQ(after.round, before.round);
	CHECK_EQ(after.lastCommit, before.lastCommit);
	CHECK_EQ(after.lastTally, before.lastTally);
	// 1 周目に採った基準（レイヤの構成）。**ここが欠けると次の周で図面が戻っているかを
	// 判定できなくなる**（テンプレートのレイヤと前の周の残りを区別できない）。
	CHECK_EQ(after.baselineRecorded, before.baselineRecorded);
	CHECK_EQ(after.baselineLayers.size(), before.baselineLayers.size());
	for (std::size_t i = 0; i < before.baselineLayers.size(); ++i)
		CHECK_EQ(after.baselineLayers[i], before.baselineLayers[i]);
	// 取り込み設定も 1 周目のまま引き継がれる（ここが欠けると 2 周目が別の条件で実行される）。
	for (std::size_t i = 0; i < kSymbolRoleCount; ++i)
	{
		const auto role = static_cast<SymbolRole>(i);
		CHECK_EQ(after.options.symbol(role), before.options.symbol(role));
		CHECK_EQ(after.options.isEnabled(role), before.options.isEnabled(role));
	}
	// 役割の表の外にある設定（M28 図面枠のスタイル）も引き継がれる。ここが欠けると 2 周目
	// 以降は図面枠が 1 枚も置かれない（PR #133 の round 2 で実際に起きた）。
	CHECK_EQ(after.options.titleBlockStyle(), before.options.titleBlockStyle());
	CHECK(after.options.hasTitleBlock());
	// M31 寸法規格も引き継がれる（欠けると 2 周目以降は寸法が 1 つも入らない）。
	CHECK_EQ(after.options.dimensionStandard(), before.options.dimensionStandard());
	CHECK(after.options.hasDimensions());
	// 伏図のまとめ方も引き継がれる（欠けると 2 周目以降は伏図の枚数が 1 周目と変わる）。
	CHECK(after.options.mergedPlanLevels == before.options.mergedPlanLevels);
	CHECK_EQ(after.options.mergedPlanLevels.size(), std::size_t(2));
	// M34 除外した通りも引き継がれる（欠けると 2 周目以降は除外したはずの通りまで描画する）。
	CHECK(after.options.skippedSections == before.options.skippedSections);
	CHECK_EQ(after.options.skippedSections.size(), std::size_t(3));
	// 垂木の断面も引き継がれる（欠けると 2 周目以降は既定の 45×45 で描画する）。
	CHECK(std::abs(after.options.rafterWidth - 60.5) < 1e-9);
	CHECK(std::abs(after.options.rafterHeight - 105.0) < 1e-9);
}

TEST(feedback_session_without_a_title_block_line_places_none)
{
	// M28 より前の記憶には titleblock の行が無い。**置かない**（従来どおり）と読む。
	const FeedbackSession session = parseFeedbackSession("round=2\nbuild=a1b2c3d\n");
	CHECK(!session.options.hasTitleBlock());
	CHECK(session.options.titleBlockStyle().empty());
	// M31 より前の記憶には dimension の行も無い。**入れない**と読む。
	CHECK(!session.options.hasDimensions());
	// M34 より前の記憶には section.skip の行も無い。**全部描画する**と読む。
	CHECK(session.options.skippedSections.empty());
	// 伏図のまとめ方の行が無い記憶は**まとめない**と読む。
	CHECK(session.options.mergedPlanLevels.empty());
	// 垂木の断面の行が無い記憶は**既定の 45×45** と読む。
	CHECK(std::abs(session.options.rafterWidth - 45.0) < 1e-9);
	CHECK(std::abs(session.options.rafterHeight - 45.0) < 1e-9);
}

TEST(feedback_session_keeps_the_default_for_unreadable_rafter_lines)
{
	// 読めない値の寸法だけ既定のまま（もう一方は読んだ値）。
	const FeedbackSession session = parseFeedbackSession("rafter.width=abc\nrafter.height=90\n");
	CHECK(std::abs(session.options.rafterWidth - 45.0) < 1e-9);
	CHECK(std::abs(session.options.rafterHeight - 90.0) < 1e-9);
}

TEST(feedback_session_skips_unreadable_merge_lines)
{
	const FeedbackSession session =
		parseFeedbackSession("merge.level=1:3531\nmerge.level=x:1\nmerge.level=2\n"
							 "merge.level=1:-5\n");
	CHECK_EQ(session.options.mergedPlanLevels.size(), std::size_t(1));
	CHECK(session.options.mergesWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}));
}

TEST(feedback_session_reads_an_m37_memory_without_the_pull_request_lines)
{
	// M37 までの記憶には PR へ投稿していた頃の行（send / repo / pr / branch / anon /
	// posted / loop）が並ぶ。**警告せずに読み飛ばし、IFC と設定はそのまま続きの周に使う**
	// ——更新しただけで 1 周目からやり直しになると、MCP から続きの周を開始できない。
	const FeedbackSession session = parseFeedbackSession(
		"send=1\nrepo=owner/repo\npr=137\nbranch=feature/x\nifc=/tmp/model.ifc\nanon=0\n"
		"round=4\nbuild=a1b2c3d\nposted=2026-09-07T01:02:03Z\nloop=1\n"
		"titleblock=図面枠 A3\n");
	CHECK_EQ(session.ifcPath, std::string("/tmp/model.ifc"));
	CHECK_EQ(session.round, 4);
	CHECK_EQ(session.options.titleBlockStyle(), std::string("図面枠 A3"));
	CHECK(feedbackSessionRemembered(session));
	// 書き直すと古い行は残らない。
	const std::string text = formatFeedbackSession(session);
	CHECK(text.find("pr=") == std::string::npos);
	CHECK(text.find("send=") == std::string::npos);
	CHECK(text.find("loop=") == std::string::npos);
}

TEST(feedback_session_parse_skips_broken_lines)
{
	// 見出し・空行・"=" の無い行・知らないキー・番号にならない／範囲外の役割は警告せずに
	// 読み飛ばし、読める行だけを取得する（古い版が書いたファイルで往復を止めない）。
	const std::string text = "# コメント\n"
							 "\n"
							 "イコールがまったく無い行\n"
							 "unknown=なにか\n"
							 "role.99.symbol=存在しない役割\n"
							 "role.x.on=1\n"
							 "roleでもドットが続かない=1\n"
							 "round=7\n"
							 "send=yes\n"
							 "auto=off\n"; // 昔の版が書いた行。知らない鍵は警告せずに読み飛ばす
	const FeedbackSession session = parseFeedbackSession(text);
	CHECK_EQ(session.round, 7);
}

TEST(feedback_session_parse_keeps_defaults_for_unreadable_values)
{
	// 真偽にならない綴り・桁あふれは**既定のまま**（0 にしない・例外を投げない）。
	const FeedbackSession session = parseFeedbackSession("baseline=たぶん\n"
														 "round=99999999999\n");
	CHECK(!session.baselineRecorded); // 既定（false）のまま
	CHECK_EQ(session.round, 0);
}

TEST(feedback_session_parse_trims_blank_values)
{
	// 値が空白だけの行は「空」として読む（前後の空白を除去するので何も残らない）。
	const FeedbackSession session = parseFeedbackSession("template=   \nifc= /tmp/a.ifc \n");
	CHECK(session.templatePath.empty());
	CHECK_EQ(session.ifcPath, std::string("/tmp/a.ifc"));
}

TEST(feedback_session_parse_ignores_bad_numbers)
{
	// 数字でない周回数は既定のまま（例外を投げない）。
	const FeedbackSession session = parseFeedbackSession("round=abc\n");
	CHECK_EQ(session.round, 0);
	const FeedbackSession negative = parseFeedbackSession("round=-1\n");
	CHECK_EQ(negative.round, 0);
}

TEST(feedback_session_reads_crlf)
{
	// Windows で手直しされたファイル（CRLF）も読める。
	const FeedbackSession session = parseFeedbackSession("round=5\r\nifc=/tmp/a.ifc\r\n");
	CHECK_EQ(session.round, 5);
	CHECK_EQ(session.ifcPath, std::string("/tmp/a.ifc"));
}

TEST(feedback_session_file_round_trip)
{
	const std::string path = tempPath("homeskz-feedback-test.txt");
	clearFeedbackSession(path);

	FeedbackSession missing;
	CHECK(!readFeedbackSession(path, missing)); // 無ければ false（＝1 周目）

	CHECK(writeFeedbackSession(path, sample()));
	FeedbackSession loaded;
	CHECK(readFeedbackSession(path, loaded));
	CHECK_EQ(loaded.round, 3);
	CHECK_EQ(loaded.ifcPath, std::string("/Users/someone/Documents/物件A.ifc"));

	clearFeedbackSession(path);
	FeedbackSession gone;
	CHECK(!readFeedbackSession(path, gone));
	// 二度削除しても異常終了しない。
	clearFeedbackSession(path);
}

TEST(feedback_session_write_reports_a_place_it_cannot_write)
{
	// 書けなくても取り込みは続けられる（2 周目が実行されないだけ）ので、**例外ではなく false**。
	// ファイルの下のパスは、ディレクトリとしても作れないので確実に失敗する。
	const std::string file = tempPath("homeskz-feedback-not-a-dir.txt");
	{
		std::ofstream out(file, std::ios::trunc);
		out << "これはファイルであってディレクトリではない\n";
	}
	CHECK(!writeFeedbackSession(file + "/child/feedback.txt", sample()));
	std::error_code ec;
	std::filesystem::remove(file, ec);
}

TEST(feedback_session_default_path_follows_the_platform)
{
	// **置き場所は環境変数だけで決まる。** ここが誤ると往復が通知なく 1 周で終わる。
	// 一時ディレクトリには置かない（消えると 2 周目が実行されない）。
	{
		// 差し替え（試験用）が最優先。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", "/tmp/custom-feedback.txt");
		CHECK_EQ(defaultFeedbackSessionPath(), std::string("/tmp/custom-feedback.txt"));
	}
	{
		// Windows は %LOCALAPPDATA% の下。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", nullptr);
		const ScopedEnv local("LOCALAPPDATA", "C:\\Users\\Taro\\AppData\\Local");
		CHECK_EQ(defaultFeedbackSessionPath(),
				 std::string("C:\\Users\\Taro\\AppData\\Local\\HomeskzIfcImport\\feedback.txt"));
	}
	{
		// macOS は $HOME/Library/Application Support の下。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", nullptr);
		const ScopedEnv local("LOCALAPPDATA", nullptr);
		const ScopedEnv home("HOME", "/Users/hanako");
		CHECK_EQ(defaultFeedbackSessionPath(),
				 std::string("/Users/hanako/Library/Application Support/HomeskzIfcImport/"
							 "feedback.txt"));
	}
	{
		// どれも取れない環境では諦める（呼び出し側は記憶を持たずに 1 周で終わる）。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", nullptr);
		const ScopedEnv local("LOCALAPPDATA", nullptr);
		const ScopedEnv home("HOME", nullptr);
		CHECK(defaultFeedbackSessionPath().empty());
	}
}

TEST(feedback_session_empty_path_is_refused)
{
	// 置き場所が決まらない環境では、通知なく諦める（記憶を持たずに 1 周で終わる）。
	FeedbackSession session;
	CHECK(!readFeedbackSession("", session));
	CHECK(!writeFeedbackSession("", session));
	clearFeedbackSession("");
}

TEST(test_report_sits_next_to_the_memory)
{
	// 報告は記憶と同じフォルダに置く（MCP の vw_test_report が本体を入れ替えたあとも読める）。
	const std::string report = testReportPathFor("/Users/hanako/Library/Application Support/"
												 "HomeskzIfcImport/feedback.txt");
	CHECK_EQ(std::filesystem::path(report).filename().string(), std::string("last-round.md"));
	CHECK_EQ(std::filesystem::path(report).parent_path().filename().string(),
			 std::string("HomeskzIfcImport"));
	// 置き場所が分からなければ空（呼び出し側は報告を書かずに結末へ添える）。
	CHECK(testReportPathFor("").empty());
	// 区切りの無いパス（環境変数でファイル名だけを渡された）なら、同じ場所に置く。
	CHECK_EQ(testReportPathFor("feedback.txt"), std::string("last-round.md"));
}

// ---------------------------------------------------------------------------
// **実機テストの周がどれになるか**（M25 / M38。core/FeedbackSession.h の feedbackRoundKind）。
// この場合分けを描画側に分散させず、無 SDK でここに固定する。

namespace
{
	// 「1 周済んだ」記憶（この 2 つが揃って初めて続きの周を組み立てられる）。
	FeedbackSession ranOnce()
	{
		FeedbackSession session;
		session.round = 1;
		session.ifcPath = "/tmp/model.ifc";
		session.lastCommit = "aaaaaaa";
		session.templatePath = "/tmp/homeskz-test/main/template-1.sta";
		return session;
	}
} // namespace

TEST(feedback_round_kind_continues_with_memory)
{
	// メニューからでも MCP からでも、記憶（とテンプレート）があれば続きの周になる。
	CHECK(feedbackRoundKind(ranOnce(), /*allowDialogs*/ true) == FeedbackRoundKind::ContinueRound);
	CHECK(feedbackRoundKind(ranOnce(), /*allowDialogs*/ false) == FeedbackRoundKind::ContinueRound);
}

TEST(feedback_round_kind_imports_again_on_the_same_build)
{
	// **同じビルドでも取り込む**（M38）。取り込み直すかは頼んだ側が決め、判断にビルドの
	// sha を参照しない。M37 までは RearmOnly で取り込まなかったが、投稿をやめたため。
	FeedbackSession session = ranOnce();
	session.lastCommit = "bbbbbbb";
	CHECK(feedbackRoundKind(session, true) == FeedbackRoundKind::ContinueRound);
}

TEST(feedback_round_kind_starts_a_first_round_without_memory)
{
	CHECK(feedbackRoundKind(FeedbackSession{}, /*allowDialogs*/ true) ==
		  FeedbackRoundKind::FirstRound);
	// 1 周も済んでいない・IFC が分からない記憶も 1 周目。
	FeedbackSession neverRan = ranOnce();
	neverRan.round = 0;
	CHECK(feedbackRoundKind(neverRan, true) == FeedbackRoundKind::FirstRound);
	FeedbackSession noFile = ranOnce();
	noFile.ifcPath.clear();
	CHECK(feedbackRoundKind(noFile, true) == FeedbackRoundKind::FirstRound);
}

TEST(feedback_round_kind_refuses_when_it_would_have_to_ask)
{
	// MCP（ダイアログ無し）は 1 周目を始められない——IFC と設定はダイアログでしか決まらない。
	CHECK(feedbackRoundKind(FeedbackSession{}, /*allowDialogs*/ false) ==
		  FeedbackRoundKind::Refuse);
}

TEST(feedback_round_kind_refuses_mcp_without_a_template)
{
	// **テンプレートが無いと MCP の周は実行されない**（M39）。人の居ない周に「いま開いている
	// 図面」を基準に採らせない——前の周の描画結果が載った図面がそのまま基準になりうる。
	FeedbackSession session = ranOnce();
	session.templatePath.clear();
	CHECK(feedbackRoundKind(session, /*allowDialogs*/ false) == FeedbackRoundKind::Refuse);
	// メニューから押した周は続きの周のまま（いま開いている図面から採り直す）。
	CHECK(feedbackRoundKind(session, /*allowDialogs*/ true) == FeedbackRoundKind::ContinueRound);
}

TEST(feedback_round_kind_starts_an_unattended_first_round_when_an_ifc_is_named)
{
	// **IFC を名指しした MCP の周は、尋ねずに 1 周目を始める**（M40）。記憶が無くても、
	// テンプレートさえあればよい（テンプレートは同じ要求で渡され、先に記憶へ入る）。
	FeedbackSession fresh;
	fresh.templatePath = "/tmp/homeskz-test/main/template-1.sta";
	CHECK(feedbackRoundKind(fresh, /*allowDialogs*/ false, /*ifcRequested*/ true) ==
		  FeedbackRoundKind::AutoFirstRound);
	// 記憶があっても、名指しされたら新しい 1 周目（別の IFC で試し直す）。
	CHECK(feedbackRoundKind(ranOnce(), false, true) == FeedbackRoundKind::AutoFirstRound);
	// 名指しが無ければ従来どおり。
	CHECK(feedbackRoundKind(fresh, false, false) == FeedbackRoundKind::Refuse);
}

TEST(feedback_round_kind_refuses_a_named_ifc_without_a_template)
{
	// **テンプレートが無ければ、名指しされても実行されない**——描画先をいま開いている図面に
	// 求めない（M39 の Refuse と同じ理由）。
	CHECK(feedbackRoundKind(FeedbackSession{}, /*allowDialogs*/ false, /*ifcRequested*/ true) ==
		  FeedbackRoundKind::Refuse);
	FeedbackSession session = ranOnce();
	session.templatePath.clear();
	CHECK(feedbackRoundKind(session, false, true) == FeedbackRoundKind::Refuse);
	// メニューの周では名指しを参照しない（人が選ぶ）。
	CHECK(feedbackRoundKind(FeedbackSession{}, /*allowDialogs*/ true, /*ifcRequested*/ true) ==
		  FeedbackRoundKind::FirstRound);
}

// ---------------------------------------------------------------------------
// **閉じてよい図面か**（M39。core/FeedbackSession.h の isOwnedTestDocument）。
// `CloseDocument()` は確認なしに変更を捨てるので、ここが利用者の図面を守る安全弁になる。

TEST(owned_test_document_is_the_one_saved_in_the_scratch_root)
{
	FeedbackSession session;
	session.ownedDocuments = {"/tmp/homeskz-test/main/round-2-1.vwx"};
	CHECK(
		isOwnedTestDocument(session, "/tmp/homeskz-test/main/round-2-1.vwx", "/tmp/homeskz-test"));
	// 名指しに無い図面は、置き場の中でも閉じない。
	CHECK(
		!isOwnedTestDocument(session, "/tmp/homeskz-test/main/round-3-1.vwx", "/tmp/homeskz-test"));
	// 利用者の図面は閉じない。
	CHECK(!isOwnedTestDocument(session, "/Users/someone/物件A.vwx", "/tmp/homeskz-test"));
	// 空のパス・置き場が分からないときは閉じない。
	CHECK(!isOwnedTestDocument(session, "", "/tmp/homeskz-test"));
	CHECK(!isOwnedTestDocument(session, "/tmp/homeskz-test/main/round-2-1.vwx", ""));
}

TEST(owned_test_document_outside_the_scratch_root_is_never_closed)
{
	// **記憶が置き場の外を指していたら、一致していても閉じない**（壊れた記憶・手で書き
	// 換えた記憶から利用者の図面へ届かせない）。
	FeedbackSession session;
	session.ownedDocuments = {"/Users/someone/物件A.vwx", "/tmp/homeskz-test-other/x.vwx"};
	CHECK(!isOwnedTestDocument(session, "/Users/someone/物件A.vwx", "/tmp/homeskz-test"));
	CHECK(!isOwnedTestDocument(session, "/tmp/homeskz-test-other/x.vwx", "/tmp/homeskz-test"));
}

TEST(owned_test_document_matches_another_spelling_of_the_same_file)
{
	// 字面が違っても同じファイルなら同じとみなす（std::filesystem::equivalent）。ここでは
	// "." を挟んだ綴りで確認する（実在するファイルが要る）。macOS の一時ディレクトリは
	// /var と /private/var の 2 通りで返るため。
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path root = fs::temp_directory_path(ec) / "homeskz-owned-doc-test";
	fs::create_directories(root / "main", ec);
	const fs::path file = root / "main" / "round-1-1.vwx";
	{
		std::ofstream(file) << "x";
	}
	FeedbackSession session;
	session.ownedDocuments = {file.string()};
	const std::string other = (root / "main" / "." / "round-1-1.vwx").string();
	CHECK(isOwnedTestDocument(session, other, root.string()));
	fs::remove_all(root, ec);
}

TEST_MAIN();
