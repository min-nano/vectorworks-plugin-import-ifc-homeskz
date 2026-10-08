//
//	ParseFeedbackTests.cpp
//
//	実機テストの報告（src/parse/Feedback）の単体テスト。無 SDK のテストハーネスで
//	走る（CLAUDE.md「テスト方針」）。報告をファイルへ書くところ（draw/Feedback）と実描画は
//	対象外で、ここで確かめるのは**組み上がった Markdown**と周の結末の文言だけ。
//
//	検証項目（docs/DEV-NOTES.md M23 / M38）:
//	  * 図面の状態の 1 行（取り込み前から在ったレイヤの枚数。周どうしの比較は読む側。M43）
//	  * **伏せない**——ファイル名をそのまま示し、PR 向けの目印や依頼を持ち込まない（M38）
//	  * 注意・ログが本文に載ること、所見は載らないこと、上限で（文字境界で）切り詰めること
//	  * 周の結末（formatTestRoundResult）の文言
//

#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Feedback.h"
#include "parse/Summary.h"

#include <cstddef>
#include <string>

using namespace HomeskzIfcImport::parse;
using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::DrawCounts;
using HomeskzIfcImport::parse::keepTail;
using HomeskzIfcImport::parse::kMaxTestReportBytes;
using HomeskzIfcImport::parse::TestRoundOutcome;

namespace
{
	// 横架材 4 本・柱 2 本の最小の命令セット（参照するのは件数だけ）。
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
		return round;
	}

	bool contains(const std::string& text, const std::string& needle)
	{
		return text.find(needle) != std::string::npos;
	}
} // namespace

// ---------------------------------------------------------------------------
// 匿名化
// ---------------------------------------------------------------------------

TEST(test_report_starts_with_the_build)
{
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), sampleCounts());
	// 見出しで「どのビルド」かを示す。**前の周と比べない**（M43）ので周の番号も差分も無い。
	CHECK(body.starts_with("## 実機テスト — `min-nano_structureDev` a1b2c3d"));
	CHECK(!contains(body, "前の周"));
	CHECK(contains(body, "claude/feedback"));
	CHECK(contains(body, "**結果: 成功**"));
	CHECK(contains(body, "| 横架材 | 4 / 4 本 |"));
}

TEST(feedback_comment_does_not_carry_the_human_note)
{
	// **所見はこの本文に載らない。** 描画結果を見て気付いたことは人が Claude とのチャットへ
	// 直接書く——プラグインは所見を訊く仕組みを持たない（docs/DEV-NOTES.md M23）。
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), sampleCounts());
	CHECK(!contains(body, "所見"));
}

TEST(feedback_comment_shows_the_size_in_kb_or_nothing)
{
	// 1 MB 未満は KB で出す。
	FeedbackRound small = sampleRound();
	small.bytes = 4096;
	CHECK(contains(formatTestRoundReport(small, sampleDocument(), sampleCounts()), "4.0 KB"));

	// 大きさを取得できなかった（0）ときは括弧ごと出さない——「0 バイトのファイルを
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

TEST(feedback_comment_counts_the_layers_that_were_there_before)
{
	// **判定はしない。** 図面のテンプレートに「共通」等が最初から在るのは普通なので、
	// 「戻っていません」と書くと毎回の誤報になる——枚数だけを書き、周どうしの比較は報告を
	// 読む側に任せる（M43）。
	DrawCounts counts = sampleCounts();
	counts.existingLayers = {"共通"};
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), counts);
	CHECK(contains(body, "図面の状態:"));
	CHECK(contains(body, "取り込み前から在ったレイヤ 1 枚"));
	// レイヤの名前そのものは載せない（顔ぶれは診断ログにある）。
	CHECK(!contains(body, "図面の状態: 共通"));
}

TEST(feedback_comment_handles_an_empty_drawing)
{
	DrawCounts counts = sampleCounts();
	counts.existingLayers.clear();
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), counts);
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
// **実機テストの結末は、実機テスト自身の文言で報告する**（M25）。取り込みコマンドの完了文言を
// 流用すると、実行した人には本番の取り込みが同じことをしているように見える——コマンドを
// 分けた意味が見た目の上で崩れる（実機の指摘）。

TEST(test_round_result_speaks_for_itself_not_for_the_import_command)
{
	const std::string done = formatTestRoundResult(TestRoundOutcome::Completed, "a1b2c3d");
	CHECK(contains(done, "実機テストを終えました"));
	CHECK(contains(done, "a1b2c3d"));

	const std::string failed = formatTestRoundResult(TestRoundOutcome::ImportFailed, {});
	CHECK(contains(failed, "取り込みがエラーで中断しました"));
	CHECK(contains(failed, "診断ログ"));
	// 取り込みが中断したのだから「終えました」とは書かない。
	CHECK(!contains(failed, "終えました"));
	// PR には言及しない（M38）。
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

TEST(test_round_result_rejects_an_unusable_request_without_drawing)
{
	// **依頼された IFC・テンプレート・settings を使えない**（M40 / M43）。何も描画していないことを明示し、
	// 使えなかった理由を添える。
	const std::string text =
		formatTestRoundResult(TestRoundOutcome::InvalidRequest, "IFC が見つかりません（/x.ifc）");
	CHECK(contains(text, "図面には何も描いていません"));
	CHECK(contains(text, "IFC が見つかりません（/x.ifc）"));
}

// ---------------------------------------------------------------------------
// **切り詰めは UTF-8 の文字境界で**（M25）。ここが崩れると壊れたバイト列が本文へ入る。
// PR へ投稿していた頃は GitHub が 400 で拒否して**その周の投稿がすべて失われた**（実機で
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
	// 実行される場面を作る。**開始位置を 1 バイトずつずらしても**壊れないことを確かめる
	// ——実機で失敗したのは、本文へ 1 行追加したことで予算が数十バイトずれた回だった。
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
	// （実機 round 2 でこの行が省略部分に入って読めなかった）。
	FeedbackRound round = sampleRound();
	round.preparation = "準備: 前の周の図面 1 枚を保存せずに閉じました。テンプレートから新しい"
						"図面を開きました（/tmp/homeskz-test/main/template-1.sta）";
	const std::string body = formatTestRoundReport(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "準備: 前の周の図面 1 枚を保存せずに閉じました"));
	// 空なら 1 行も増やさない。
	round.preparation.clear();
	CHECK(!contains(formatTestRoundReport(round, sampleDocument(), sampleCounts()), "準備:"));
}

TEST(test_report_never_asks_for_undo)
{
	// **人に「取り消し」を依頼しない**（M39）。毎周テンプレートから開いた新しい図面へ描画する
	// ので、取り込み前から在ったレイヤへ描画した周でも、次の周はすべて元の状態から始まる。
	DrawCounts counts = sampleCounts();
	counts.undoPartial = true;
	counts.existingLayers = {"共通"};
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), counts);
	CHECK(!contains(body, "取り消し"));
}

TEST(test_report_shows_the_file_name_without_hiding_it)
{
	// **伏せない**（M38）。報告は利用者の計算機の中だけで読まれる（PR へは投稿しない）ので、
	// どのファイルを取り込んだかはそのまま示す。
	const std::string body = formatTestRoundReport(sampleRound(), sampleDocument(), sampleCounts());
	CHECK(contains(body, "物件A.ifc"));
	CHECK(!contains(body, "model-"));
	CHECK(!contains(body, "伏せてあります"));
}

TEST(test_report_carries_no_pull_request_markup)
{
	// **PR の作法を持ち込まない**（M38）。目印の HTML コメントも、合図の案内も、
	// 「push したらもう一度実行して」の依頼も、もう不要。
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
