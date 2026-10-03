//
//	CoreFeedbackSessionTests.cpp
//
//	実機テストの記憶（src/core/FeedbackSession）の単体テスト。VectorWorks SDK を
//	一切 include せず、無 SDK のテストハーネスで走る（CLAUDE.md「テスト方針」）。
//
//	検証項目（docs/DEV-NOTES.md M23）: 既定は「何もしない」・書いて読んで元に戻る・
//	壊れた行を飛ばして読み続ける・ファイルへの読み書き。**2 周目が走るかどうかはこの
//	記憶だけに懸かっている**（MCP の vw_run_test はダイアログを出せない）ので、ここを
//	壊さないこと。
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
using HomeskzIfcImport::core::kSymbolRoleCount;
using HomeskzIfcImport::core::parseFeedbackSession;
using HomeskzIfcImport::core::readFeedbackSession;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::testReportPathFor;
using HomeskzIfcImport::core::writeFeedbackSession;

namespace
{
	// 一通り埋めた記憶（周をまたいで実際に運ぶ値の全部）。
	FeedbackSession sample()
	{
		FeedbackSession session;
		session.ifcPath = "/Users/someone/Documents/物件A.ifc";
		session.workPath = "/tmp/homeskz-work.vwx";
		session.round = 3;
		session.lastCommit = "a1b2c3d";
		session.lastTally = "ストーリ:3/3,通り芯:44/44";
		session.baselineRecorded = true;
		session.baselineLayers = {"共通", "デザイン レイヤ-1"};
		session.lastCreatedLayers = {"1-伏図", "2-伏図"};
		session.lastCreatedSheets = {"A-1", "A-2"};
		session.options.setSymbol(SymbolRole::FloorPost, "床束（特注）");
		session.options.setEnabled(SymbolRole::FireBrace, false);
		session.options.setTitleBlockStyle("図面枠 A3（構造）");
		session.options.setDimensionStandard("構造図 寸法");
		session.options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}, true);
		session.options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{2, 6374}, true);
		// M34 軸組図から外す通り（重なった図番の "(2)" も含めて）。
		session.options.setSkippedSections({"X1", "1(2)", "又い"});
		// 垂木の断面（端数付きの寸法も運べること）。
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

	// 環境変数の付け外し。**置き場所の決め方は環境変数だけで決まる**ので、これが無いと
	// その分岐（Windows 流・macOS 流・どちらも取れない）を確かめられない。綴りが処理系で
	// 違うのでここに閉じ込める（core/Trace が getenv を 1 か所へ閉じ込めているのと同じ）。
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

	// 環境変数を 1 つ預かって、抜けるときに元へ戻す（他のテストへ漏らさない）。
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
	// 古い版が書いた記憶（baseline の行が無い）。**空の基準と取り違えない**——
	// 取り違えると、基準を採る前の周を「戻っています」と報告してしまう。
	const FeedbackSession session = parseFeedbackSession("round=2\nbuild=a1b2c3d\n");
	CHECK(!session.baselineRecorded);
	CHECK(session.baselineLayers.empty());
	// 古い版には「前の周が作ったレイヤ」の行も無い。**空＝消す相手が分からない**ので、
	// そのときは図面に触らない（draw/Feedback の prepareDrawingForRound）。
	CHECK(session.lastCreatedLayers.empty());
	CHECK(session.lastCreatedSheets.empty());
	// 開き直しの行（M25）も無い。**空＝開き直さない**なので、古い記憶を読んでも従来
	// どおり「いま開いている図面へ描く」に落ちる。
	CHECK(session.workPath.empty());
}

TEST(feedback_session_keeps_an_empty_baseline_distinct_from_none)
{
	// まっさらな図面で始めた 1 周目は「基準は採ったが 0 枚」。真偽が無いと区別できない。
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
	// **毎周開き直す図面**（M25）。テンプレートのパスが落ちると、次の周は開き直さずに
	// 前の周の図へ重ねて描いてしまう。開いた複製のパスが落ちると、その図面を閉じられず
	// 周の数だけ積み上がる。
	CHECK_EQ(after.workPath, before.workPath);
	CHECK_EQ(after.round, before.round);
	CHECK_EQ(after.lastCommit, before.lastCommit);
	CHECK_EQ(after.lastTally, before.lastTally);
	// 1 周目に採った基準（レイヤの顔ぶれ）。**ここが落ちると次の周で図面が戻っているかを
	// 判定できなくなる**（テンプレートのレイヤと前の周の残りを区別できない）。
	CHECK_EQ(after.baselineRecorded, before.baselineRecorded);
	CHECK_EQ(after.baselineLayers.size(), before.baselineLayers.size());
	for (std::size_t i = 0; i < before.baselineLayers.size(); ++i)
		CHECK_EQ(after.baselineLayers[i], before.baselineLayers[i]);
	// **前の周が作ったレイヤ**（M25）。次の周の前にこれ**だけ**を図面から取り除くので、
	// ここが落ちると「消してよいもの」を見失う——見失ったまま別の基準で消す作りに
	// してはならない（利用者が足したレイヤを巻き込む）。デザインとシートは混ぜない
	// （消す順序が違う: シートが先）。
	CHECK_EQ(after.lastCreatedLayers.size(), before.lastCreatedLayers.size());
	for (std::size_t i = 0; i < before.lastCreatedLayers.size(); ++i)
		CHECK_EQ(after.lastCreatedLayers[i], before.lastCreatedLayers[i]);
	CHECK_EQ(after.lastCreatedSheets.size(), before.lastCreatedSheets.size());
	for (std::size_t i = 0; i < before.lastCreatedSheets.size(); ++i)
		CHECK_EQ(after.lastCreatedSheets[i], before.lastCreatedSheets[i]);
	// 取り込み設定も 1 周目のまま運ばれる（ここが落ちると 2 周目が別の条件で走る）。
	for (std::size_t i = 0; i < kSymbolRoleCount; ++i)
	{
		const auto role = static_cast<SymbolRole>(i);
		CHECK_EQ(after.options.symbol(role), before.options.symbol(role));
		CHECK_EQ(after.options.isEnabled(role), before.options.isEnabled(role));
	}
	// 役割の表の外にある設定（M28 図面枠のスタイル）も運ばれる。ここが落ちると 2 周目
	// 以降は図面枠が 1 枚も置かれない（PR #133 の round 2 で実際に起きた）。
	CHECK_EQ(after.options.titleBlockStyle(), before.options.titleBlockStyle());
	CHECK(after.options.hasTitleBlock());
	// M31 寸法規格も運ばれる（落ちると 2 周目以降は寸法が 1 つも入らない）。
	CHECK_EQ(after.options.dimensionStandard(), before.options.dimensionStandard());
	CHECK(after.options.hasDimensions());
	// 伏図のまとめ方も運ばれる（落ちると 2 周目以降は伏図の枚数が 1 周目と変わる）。
	CHECK(after.options.mergedPlanLevels == before.options.mergedPlanLevels);
	CHECK_EQ(after.options.mergedPlanLevels.size(), std::size_t(2));
	// M34 外した通りも運ばれる（落ちると 2 周目以降は外したはずの通りまで描く）。
	CHECK(after.options.skippedSections == before.options.skippedSections);
	CHECK_EQ(after.options.skippedSections.size(), std::size_t(3));
	// 垂木の断面も運ばれる（落ちると 2 周目以降は既定の 45×45 で描く）。
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
	// M34 より前の記憶には section.skip の行も無い。**全部描く**と読む。
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
	// posted / loop）が並ぶ。**黙って読み飛ばし、IFC と設定はそのまま続きの周に使う**
	// ——更新しただけで 1 周目からやり直しになると、MCP から続きの周を起こせない。
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
	// 見出し・空行・"=" の無い行・知らないキー・番号にならない／範囲外の役割は黙って
	// 飛ばし、読める行だけを拾う（古い版が書いたファイルで往復を止めない）。
	const std::string text = "# コメント\n"
							 "\n"
							 "イコールがまったく無い行\n"
							 "unknown=なにか\n"
							 "role.99.symbol=存在しない役割\n"
							 "role.x.on=1\n"
							 "roleでもドットが続かない=1\n"
							 "round=7\n"
							 "send=yes\n"
							 "auto=off\n"; // 昔の版が書いた行。知らない鍵は黙って飛ばす
	const FeedbackSession session = parseFeedbackSession(text);
	CHECK_EQ(session.round, 7);
}

TEST(feedback_session_parse_keeps_defaults_for_unreadable_values)
{
	// 真偽にならない綴り・桁あふれは**既定のまま**（0 に潰さない・例外を投げない）。
	const FeedbackSession session = parseFeedbackSession("baseline=たぶん\n"
														 "round=99999999999\n");
	CHECK(!session.baselineRecorded); // 既定（false）のまま
	CHECK_EQ(session.round, 0);
}

TEST(feedback_session_parse_trims_blank_values)
{
	// 値が空白だけの行は「空」として読む（前後の空白を落とすので何も残らない）。
	const FeedbackSession session = parseFeedbackSession("work=   \nifc= /tmp/a.ifc \n");
	CHECK(session.workPath.empty());
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
	// 二度消しても落ちない。
	clearFeedbackSession(path);
}

TEST(feedback_session_write_reports_a_place_it_cannot_write)
{
	// 書けなくても取り込みは続けられる（2 周目が走らないだけ）ので、**例外ではなく false**。
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
	// **置き場所は環境変数だけで決まる。** 一時ディレクトリには置かない（消えると
	// 2 周目が走らない）ので、ここが狂うと往復が静かに 1 周で終わる。
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
	// 置き場所が決まらない環境では、黙って諦める（記憶を持たずに 1 周で終わる）。
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
}

// ---------------------------------------------------------------------------
// **実機テストの周がどれになるか**（M25 / M38。core/FeedbackSession.h の feedbackRoundKind）。
// この場合分けを描画側に散らさず、無 SDK でここに固定する。

namespace
{
	// 「1 周済んだ」記憶（この 2 つが揃って初めて続きの周を組み立てられる）。
	FeedbackSession ranOnce()
	{
		FeedbackSession session;
		session.round = 1;
		session.ifcPath = "/tmp/model.ifc";
		session.lastCommit = "aaaaaaa";
		return session;
	}
} // namespace

TEST(feedback_round_kind_continues_with_memory)
{
	// メニューからでも MCP からでも、記憶があれば続きの周になる。
	CHECK(feedbackRoundKind(ranOnce(), /*allowDialogs*/ true) == FeedbackRoundKind::ContinueRound);
	CHECK(feedbackRoundKind(ranOnce(), /*allowDialogs*/ false) == FeedbackRoundKind::ContinueRound);
}

TEST(feedback_round_kind_imports_again_on_the_same_build)
{
	// **同じビルドでも取り込む**（M38）。M37 までは RearmOnly で取り込まなかったが、投稿を
	// やめたので、取り込み直すかは頼んだ側が決める。判断はビルドの sha を見ない。
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

TEST_MAIN();
