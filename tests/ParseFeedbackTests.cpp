//
//	ParseFeedbackTests.cpp
//
//	実機フィードバック本文（src/parse/Feedback）の単体テスト。無 SDK のテストハーネスで
//	走る（CLAUDE.md「テスト方針」）。投稿そのもの（同梱スクリプト）と実描画は対象外で、
//	ここで確かめるのは**組み上がった Markdown**だけ。
//
//	検証項目（docs/DEV-NOTES.md M23）:
//	  * 内訳の 1 行表現と、その差分（周回どうしの突き合わせ）
//	  * 匿名化——同じ入力なら同じ仮名・**素のファイル名・ユーザー名・図面枠のスタイル名が
//	    どこにも残らない**
//	  * 目印・注意・ログが本文に載ること、所見は載らないこと、上限で切り詰めること
//

#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Feedback.h"
#include "parse/Summary.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport::parse;
using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::DrawCounts;
using HomeskzIfcImport::parse::keepTail;
using HomeskzIfcImport::parse::kMaxTestReportBytes;
using HomeskzIfcImport::parse::TestRoundOutcome;

namespace
{
	// 横架材 4 本・柱 2 本の最小の命令セット（読むのは件数だけ）。
	Document sampleDocument()
	{
		Document document;
		document.members.resize(4);
		document.columns.resize(2);
		return document;
	}

	DrawCounts sampleCounts()
	{
		DrawCounts counts;
		counts.valid = true;
		counts.members = 4;
		counts.columns = 2;
		return counts;
	}

	FeedbackRound sampleRound()
	{
		FeedbackRound round;
		round.build.plugin = "min-nano_structureDev";
		round.build.channel = "dev";
		round.build.commit = "a1b2c3d";
		round.build.branch = "claude/feedback";
		round.build.platform = "macOS";
		round.ifcPath = "/Users/hanako/Documents/物件A.ifc";
		round.bytes = 3ULL * 1024ULL * 1024ULL;
		round.seconds = 12.25;
		round.startedAt = "2026-09-05 14:03:21";
		round.round = 1;
		return round;
	}

	bool contains(const std::string& text, const std::string& needle)
	{
		return text.find(needle) != std::string::npos;
	}
} // namespace

// ---------------------------------------------------------------------------
// 内訳の 1 行表現と差分
// ---------------------------------------------------------------------------

TEST(feedback_tally_lists_only_elements_with_commands)
{
	// 命令の無い要素は載せない（無い物の 0 を並べても差分の役に立たない）。
	const std::string tally = formatTally(elementRows(sampleDocument(), sampleCounts()));
	CHECK_EQ(tally, std::string("横架材:4/4,柱:2/2"));
}

TEST(feedback_tally_diff_reports_only_changes)
{
	const std::string diff = formatTallyDiff("横架材:2/4,柱:2/2", "横架材:4/4,柱:2/2");
	CHECK(contains(diff, "横架材: 2/4 → 4/4"));
	CHECK(!contains(diff, "柱")); // 変わっていない行は出さない
}

TEST(feedback_tally_diff_is_empty_when_nothing_moved)
{
	CHECK(formatTallyDiff("横架材:4/4", "横架材:4/4").empty());
}

TEST(feedback_tally_diff_reports_appearing_and_vanishing_elements)
{
	// 要素が丸ごと消えるのはたいてい退行なので、必ず出す。
	const std::string diff = formatTallyDiff("横架材:4/4,耐力壁:3/3", "横架材:4/4,通り芯:8/8");
	CHECK(contains(diff, "通り芯: （前回は無し）→ 8/8"));
	CHECK(contains(diff, "耐力壁: 3/3 → （今回は命令なし）"));
}

TEST(feedback_tally_diff_without_previous_is_empty)
{
	// 1 周目は比べる相手がいない（節ごと出さないので空でよい）。
	CHECK(formatTallyDiff("", "横架材:4/4").empty());
}

TEST(feedback_tally_diff_ignores_broken_entries)
{
	// 壊れた記憶を読んでも落ちない・止まらない。区切りの無い項目・数字でない数・
	// 名前の無い項目は、どれも黙って飛ばす。
	const std::string diff =
		formatTallyDiff("こわれた,柱:x/2,:3/3,横架材:2/4,梁:4/y", "横架材:4/4");
	CHECK(contains(diff, "横架材: 2/4 → 4/4"));
	CHECK(!contains(diff, "柱"));
	CHECK(!contains(diff, "梁"));
}

// ---------------------------------------------------------------------------
// 匿名化
// ---------------------------------------------------------------------------

TEST(test_report_starts_with_the_round_and_build)
{
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), sampleCounts());
	// 見出しで「何周目・どのビルド」を言う。
	CHECK(body.starts_with("## 実機テスト round 1 — `min-nano_structureDev` a1b2c3d"));
	CHECK(contains(body, "claude/feedback"));
	CHECK(contains(body, "**結果: 成功**"));
	CHECK(contains(body, "| 横架材 | 4 / 4 本 |"));
}

TEST(feedback_comment_does_not_carry_the_human_note)
{
	// **所見はこの本文に載らない。** 絵を見て気付いたことは人が Claude とのチャットへ
	// 直接書く——プラグインは所見を訊く仕組みを持たない（docs/DEV-NOTES.md M23）。
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), sampleCounts());
	CHECK(!contains(body, "所見"));
}

TEST(feedback_comment_shows_the_diff_from_the_previous_round)
{
	FeedbackRound round = sampleRound();
	round.round = 2;
	round.previousCommit = "9f8e7d6";
	round.previousTally = "横架材:2/4,柱:2/2";
	const std::string body = formatTestRoundReport(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "前の周（round 1 / 9f8e7d6）からの変化"));
	CHECK(contains(body, "横架材: 2/4 → 4/4"));
}

TEST(feedback_comment_says_when_nothing_changed)
{
	FeedbackRound round = sampleRound();
	round.round = 2;
	round.previousTally = "横架材:4/4,柱:2/2";
	const std::string body = formatTestRoundReport(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "内訳に変化はありません"));
}

TEST(feedback_comment_shows_the_size_in_kb_or_nothing)
{
	// 1 MB 未満は KB で出す。
	FeedbackRound small = sampleRound();
	small.bytes = 4096;
	CHECK(contains(formatTestRoundReport(small, sampleDocument(), sampleCounts()), "4.0 KB"));

	// 大きさが取れなかった（0）ときは括弧ごと出さない——「0 バイトのファイルを
	// 取り込んだ」と読み違えさせないため。
	FeedbackRound unknown = sampleRound();
	unknown.bytes = 0;
	unknown.seconds = 0.0;
	const std::string body = formatTestRoundReport(unknown, sampleDocument(), sampleCounts());
	CHECK(!contains(body, "0.0 KB"));
	CHECK(!contains(body, "所要"));
}

TEST(feedback_comment_says_when_there_are_no_commands)
{
	// 「取り込める要素が 1 つも無い」も報告の対象（ホームズ君の IFC かどうかを疑う場面）。
	const Document empty;
	DrawCounts counts;
	counts.valid = true;
	const std::string body = formatTestRoundReport(sampleRound(), empty, counts);
	CHECK(contains(body, "命令が 1 つも出ていません"));
	CHECK(contains(body, "対象なし"));
}

TEST(feedback_comment_takes_the_baseline_on_the_first_round)
{
	// **1 周目は判定しない。** 図面のテンプレートに「共通」等が最初から在るのは普通なので、
	// ここで「戻っていません」と書くと毎回の誤報になる——採ったことだけ言う。
	DrawCounts counts = sampleCounts();
	counts.existingLayers = {"共通"};
	FeedbackRound first = sampleRound();
	first.baselineKnown = false;
	const std::string body = formatTestRoundReport(first, sampleDocument(), counts);
	CHECK(contains(body, "図面の状態:"));
	CHECK(contains(body, "取り込み前から在ったレイヤ 1 枚"));
	CHECK(contains(body, "基準にします"));
	CHECK(!contains(body, "重ねて描きました"));
}

TEST(feedback_comment_says_the_drawing_was_restored_when_it_matches_the_baseline)
{
	// テンプレートのレイヤが基準どおりに在るだけ＝取り込み前へ戻してある。
	DrawCounts counts = sampleCounts();
	counts.existingLayers = {"共通"};
	FeedbackRound later = sampleRound();
	later.baselineKnown = true;
	later.baselineLayers = {"共通"};
	const std::string body = formatTestRoundReport(later, sampleDocument(), counts);
	CHECK(contains(body, "取り込み前の状態へ戻してから実行されています"));
	CHECK(contains(body, "1 周目と同じ 1 枚"));
}

TEST(feedback_comment_flags_a_drawing_that_was_not_restored)
{
	// 基準に無いレイヤ（前の周が作ったもの）へも描いている＝戻していない。
	DrawCounts counts = sampleCounts();
	counts.existingLayers = {"共通", "1-FL", "2-FL"};
	FeedbackRound later = sampleRound();
	later.baselineKnown = true;
	later.baselineLayers = {"共通"};
	const std::string body = formatTestRoundReport(later, sampleDocument(), counts);
	CHECK(contains(body, "前の周の図が残ったまま重ねて描きました"));
	CHECK(contains(body, "レイヤ 2 枚"));
	CHECK(contains(body, "実装のせいにしないでください"));
}

TEST(feedback_comment_notices_a_different_drawing)
{
	// 基準にあったものが無い＝別の図面か、テンプレートが変わった。**戻し忘れとは言わない。**
	DrawCounts counts = sampleCounts();
	counts.existingLayers.clear();
	FeedbackRound later = sampleRound();
	later.baselineKnown = true;
	later.baselineLayers = {"共通"};
	const std::string body = formatTestRoundReport(later, sampleDocument(), counts);
	CHECK(contains(body, "見当たりません"));
	CHECK(!contains(body, "重ねて描きました"));
}

TEST(feedback_comment_handles_a_first_round_on_an_empty_drawing)
{
	DrawCounts counts = sampleCounts();
	counts.existingLayers.clear();
	FeedbackRound first = sampleRound();
	first.baselineKnown = false;
	const std::string body = formatTestRoundReport(first, sampleDocument(), counts);
	CHECK(contains(body, "まっさらな図面から取り込みました"));
}

TEST(test_report_shows_the_ordinary_notes)
{
	// 平常でも出る記録（用紙の割り付け等）も載せる（注意とは節を分ける）。
	DrawCounts counts = sampleCounts();
	counts.notes = "伏図: 1:50 で 3 面";
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), counts);
	CHECK(contains(body, "記録（用紙の割り付けなど）"));
	CHECK(contains(body, "1:50 で 3 面"));
}

TEST(feedback_comment_shows_draw_diagnostics_unfolded)
{
	DrawCounts counts = sampleCounts();
	counts.members = 2;
	counts.diagnostics = "横架材: レイヤが無く置けなかった 2 本";
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), counts);
	CHECK(contains(body, "### 注意（描画側の異常）"));
	CHECK(contains(body, "レイヤが無く置けなかった"));
}

TEST(feedback_comment_trims_an_oversized_log)
{
	FeedbackRound round = sampleRound();
	round.log = std::string(200000, 'x') + "\n最後の行\n";
	const std::string body = formatTestRoundReport(round, sampleDocument(), sampleCounts());
	CHECK(body.size() <= kMaxTestReportBytes);
	// 削るのは古いほう（結果に近い末尾を残す）。
	CHECK(contains(body, "最後の行"));
	CHECK(contains(body, "を省略"));
}

// ---------------------------------------------------------------------------
// **実機テストの結末は、実機テスト自身の言葉で言う**（M25）。取り込みコマンドの完了文言を
// 借りると、押した人には本番の取り込みが同じことをしているように見える——コマンドを
// 分けた意味が見た目の上で崩れる（実機の指摘）。

TEST(test_round_result_speaks_for_itself_not_for_the_import_command)
{
	const std::string done =
		formatTestRoundResult(TestRoundOutcome::Completed, "round 3（a1b2c3d）");
	CHECK(contains(done, "実機テストを終えました"));
	CHECK(contains(done, "round 3（a1b2c3d）"));

	const std::string failed = formatTestRoundResult(TestRoundOutcome::ImportFailed, {});
	CHECK(contains(failed, "取り込みがエラーで中断しました"));
	CHECK(contains(failed, "診断ログ"));
	// 取り込みが中断したのだから「終えました」とは言わない。
	CHECK(!contains(failed, "終えました"));
	// PR の話はもうしない（M38）。
	CHECK(!contains(failed, "PR"));
	// 中断の詳しい事情があれば、診断ログの案内の前に添える。
	const std::string detailed =
		formatTestRoundResult(TestRoundOutcome::ImportFailed, "IFC を開けませんでした");
	CHECK(contains(detailed, "IFC を開けませんでした"));
	CHECK(detailed.find("IFC を開けませんでした") < detailed.find("診断ログ"));

	const std::string document =
		formatTestRoundResult(TestRoundOutcome::DocumentFailed, "準備: 開き直せませんでした");
	CHECK(contains(document, "図面には何も描いていません"));
	CHECK(contains(document, "準備: 開き直せませんでした"));
}

TEST(test_round_result_says_how_to_start_the_first_round)
{
	// **MCP から 1 周目は起こせない**（IFC と設定はダイアログでしか決まらない）。何をすれば
	// 続けられるかを、Claude が人へそのまま伝えられる形で言う。
	const std::string text = formatTestRoundResult(TestRoundOutcome::NotRemembered, {});
	CHECK(contains(text, "1 周目がまだ済んでいません"));
	CHECK(contains(text, "「実機テストを実行…」"));
}

// ---------------------------------------------------------------------------
// **切り詰めは UTF-8 の文字境界で**（M25）。ここが崩れると壊れたバイト列が本文へ入る。
// PR へ投稿していた頃は GitHub が 400 で弾いて**その周の投稿がまるごと落ちた**（実機で
// 発生）。いまの読み手（MCP の JSON）も壊れた UTF-8 は受け付けない。

namespace
{
	// UTF-8 として妥当か（継続バイトの数が先頭バイトの宣言どおりか）。
	bool validUtf8(const std::string& text)
	{
		std::size_t i = 0;
		while (i < text.size())
		{
			const auto b = static_cast<unsigned char>(text[i]);
			std::size_t len = 0;
			if (b < 0x80U)
				len = 1;
			else if ((b & 0xE0U) == 0xC0U)
				len = 2;
			else if ((b & 0xF0U) == 0xE0U)
				len = 3;
			else if ((b & 0xF8U) == 0xF0U)
				len = 4;
			else
				return false; // 継続バイトから始まっている＝途中で切れている
			if (i + len > text.size())
				return false;
			for (std::size_t k = 1; k < len; ++k)
			{
				if ((static_cast<unsigned char>(text[i + k]) & 0xC0U) != 0x80U)
					return false;
			}
			i += len;
		}
		return true;
	}
} // namespace

TEST(feedback_comment_truncates_the_log_on_a_character_boundary)
{
	// 日本語だけの長いログ（1 文字 3 バイト）。上限を必ず超える長さにして、切り詰めが
	// 走る場面を作る。**開始位置を 1 バイトずつずらしても**壊れないことを見る——実機で
	// 落ちたのは、本文へ 1 行足したせいで予算が数十バイトずれた回だった。
	for (std::size_t pad = 0; pad < 6; ++pad)
	{
		FeedbackRound round = sampleRound();
		round.preparation = std::string(pad, 'x'); // 予算を 1 バイトずつずらす
		std::string log;
		while (log.size() < kMaxTestReportBytes + 4096)
			log += "あいうえお かきくけこ さしすせそ\n";
		round.log = log;

		const std::string body = formatTestRoundReport(round, sampleDocument(), sampleCounts());
		CHECK(contains(body, "バイトを省略"));
		CHECK(validUtf8(body));
		CHECK(body.size() <= kMaxTestReportBytes);
	}
}

TEST(feedback_comment_shows_what_the_round_did_to_the_drawing_before_importing)
{
	// **図面をどう用意したかは「図面の状態」の隣に置く。** 同じ 1 行は診断ログにも入るが、
	// ログは上限で切り詰められるので、そこだけを頼りにすると読めない周が出る
	// （実機 round 2 でこの行が省略部分へ落ちて読めなかった）。
	FeedbackRound round = sampleRound();
	round.preparation = "準備: 前の周の図面 1 枚を保存せずに閉じました。テンプレートから新しい"
						"図面を開きました（/tmp/homeskz-test/main/template-1.sta）";
	const std::string body = formatTestRoundReport(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "準備: 前の周の図面 1 枚を保存せずに閉じました"));
	// 空なら 1 行も増やさない（1 周目や古い版の記憶）。
	round.preparation.clear();
	CHECK(!contains(formatTestRoundReport(round, sampleDocument(), sampleCounts()), "準備:"));
}

TEST(test_report_never_asks_for_undo)
{
	// **人に「取り消し」を頼まない**（M39）。毎周テンプレートから開いた新しい図面へ描くので、
	// 取り込み前から在ったレイヤへ描いた周でも、次の周は丸ごと元の状態から始まる。
	DrawCounts counts = sampleCounts();
	counts.undoPartial = true;
	counts.existingLayers = {"共通"};
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), counts);
	CHECK(!contains(body, "取り消し"));
}

TEST(test_report_shows_the_file_name_without_hiding_it)
{
	// **伏せない**（M38）。報告は利用者の計算機の中だけで読まれる（PR へは投稿しない）ので、
	// どのファイルを取り込んだかはそのまま見せる。
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), sampleCounts());
	CHECK(contains(body, "物件A.ifc"));
	CHECK(!contains(body, "model-"));
	CHECK(!contains(body, "伏せてあります"));
}

TEST(test_report_carries_no_pull_request_markup)
{
	// **PR の作法を持ち込まない**（M38）。目印の HTML コメントも、合図の案内も、
	// 「push したらもう一度実行して」の頼みも、もう要らない。
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), sampleCounts());
	CHECK(!contains(body, "<!--"));
	CHECK(!contains(body, "control=stop"));
	CHECK(!contains(body, "push"));
}

TEST(keep_tail_keeps_short_text_and_trims_long_text_from_the_front)
{
	CHECK_EQ(keepTail("短い\n", 100), std::string("短い\n"));
	std::string log;
	for (int i = 0; i < 100; ++i)
		log += "行 " + std::to_string(i) + "\n";
	const std::string kept = keepTail(log, 200);
	CHECK(kept.size() <= 200);
	CHECK(contains(kept, "行 99"));
	CHECK(!contains(kept, "行 0\n"));
	CHECK(contains(kept, "バイトを省略"));
	CHECK(validUtf8(kept));
	// 予算が省略の案内より小さくても、範囲の外を読まない（本文は 1 行も残らない）。
	const std::string tiny = keepTail(log, 10);
	CHECK(contains(tiny, "バイトを省略"));
	CHECK(!contains(tiny, "行 99"));
	CHECK(validUtf8(tiny));
}

TEST_MAIN();
