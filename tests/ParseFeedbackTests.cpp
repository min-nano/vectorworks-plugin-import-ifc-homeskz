//
//	ParseFeedbackTests.cpp
//
//	実機フィードバック本文（src/parse/Feedback）の単体テスト。無 SDK のテストハーネスで
//	走る（CLAUDE.md「テスト方針」）。投稿そのもの（同梱スクリプト）と実描画は対象外で、
//	ここで確かめるのは**組み上がった Markdown**だけ。
//
//	検証項目（docs/DEV-NOTES.md M23）:
//	  * 内訳の 1 行表現と、その差分（周回どうしの突き合わせ）
//	  * 匿名化——同じ入力なら同じ仮名・**素のファイル名とユーザー名がどこにも残らない**
//	  * 目印・所見・注意・ログが本文に載ること、上限で切り詰めること
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
		round.build.plugin = "HomeskzIfcImportDev";
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

TEST(feedback_anonymized_name_is_stable_and_opaque)
{
	const std::string a = anonymizedFileName("/Users/hanako/Documents/物件A.ifc");
	const std::string b = anonymizedFileName("/elsewhere/物件A.ifc");
	// 同じファイル名なら置き場所が変わっても同じ仮名（周回どうしで対象の同一性が分かる）。
	CHECK_EQ(a, b);
	// 別のファイルは別の仮名。
	CHECK(a != anonymizedFileName("/Users/hanako/Documents/物件B.ifc"));
	// 元の名前は残らない。拡張子は保つ（IFC を取り込んだことは伏せる必要が無い）。
	CHECK(!contains(a, "物件A"));
	CHECK(contains(a, ".ifc"));
	CHECK(contains(a, "model-"));
}

TEST(feedback_redaction_removes_path_and_user_name)
{
	const std::string log = "ファイル: /Users/hanako/Documents/物件A.ifc\n"
							"ログ: /Users/hanako/Library/Logs/HomeskzIfcImport.log\n"
							"対象 物件A.ifc を読み込みました\n";
	const std::string clean = redactText(log, "/Users/hanako/Documents/物件A.ifc");
	CHECK(!contains(clean, "hanako"));
	CHECK(!contains(clean, "物件A"));
	CHECK(contains(clean, "model-"));
	// 伏せるのは名前だけ。診断の中身（何をしたか）はそのまま残る。
	CHECK(contains(clean, "読み込みました"));
}

TEST(feedback_anonymized_name_handles_odd_paths)
{
	// 区切りの無い名前（相対パス）でも仮名になる。
	CHECK(contains(anonymizedFileName("model.ifc"), "model-"));
	// 末尾が区切りで終わっていてファイル名が取れないときも、名前を作って返す
	// （**空の仮名を出さない**——伏せたつもりの本名が出るより悪い）。
	CHECK_EQ(anonymizedFileName("/Users/hanako/"), std::string("model-000000.ifc"));
	// 拡張子が無ければ .ifc を付ける（取り込んだのが IFC であることは伏せる必要が無い）。
	CHECK(contains(anonymizedFileName("/tmp/plan"), ".ifc"));
}

TEST(feedback_redaction_survives_paths_without_a_file_name)
{
	// 対象のパスがディレクトリで終わっていても落ちない・止まらない。
	const std::string clean = redactText("どこかの /Users/hanako/ の中", "/Users/hanako/");
	CHECK(!contains(clean, "hanako"));
}

TEST(feedback_redaction_masks_a_trailing_or_doubled_user_segment)
{
	// 区切りで終わらないユーザー名（ログの末尾）も伏せる。
	CHECK(!contains(redactText("home=/Users/hanako", ""), "hanako"));
	// 区切りが続いただけの箇所は、伏せるものが無いのでそのまま進む（無限に回らない）。
	CHECK_EQ(redactText("/Users//tmp", ""), std::string("/Users//tmp"));
}

TEST(feedback_redaction_handles_windows_paths)
{
	const std::string clean =
		redactText("C:\\Users\\Taro\\Desktop\\model.ifc", "C:\\Users\\Taro\\Desktop\\model.ifc");
	CHECK(!contains(clean, "Taro"));
	CHECK(!contains(clean, "\\model.ifc"));
}

// ---------------------------------------------------------------------------
// 本文
// ---------------------------------------------------------------------------

TEST(feedback_comment_starts_with_machine_marker)
{
	const std::string body = formatFeedbackComment(sampleRound(), sampleDocument(), sampleCounts());
	// 目印は先頭行。読む側はこれで「プラグインの自動投稿・何周目・どのビルド」を拾う。
	CHECK(body.starts_with("<!-- homeskz-ifc-feedback v1 round=1 build=a1b2c3d"));
	CHECK(contains(body, "## 実機フィードバック round 1"));
	CHECK(contains(body, "**結果: 成功**"));
	CHECK(contains(body, "| 横架材 | 4 / 4 本 |"));
}

TEST(feedback_comment_hides_the_file_name_by_default)
{
	const std::string body = formatFeedbackComment(sampleRound(), sampleDocument(), sampleCounts());
	CHECK(!contains(body, "物件A"));
	CHECK(!contains(body, "hanako"));
	CHECK(contains(body, "model-"));
	CHECK(contains(body, "伏せてあります"));
}

TEST(feedback_comment_can_show_the_real_name)
{
	// 私有リポジトリへ投げるときは伏せない（core::FeedbackSession::anonymize）。
	FeedbackRound round = sampleRound();
	round.anonymize = false;
	const std::string body = formatFeedbackComment(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "物件A.ifc"));
	CHECK(!contains(body, "伏せてあります"));
}

TEST(feedback_comment_puts_the_human_note_first)
{
	FeedbackRound round = sampleRound();
	round.note = "3 階の梁が 300mm 浮いている。\n1 階は問題なし。";
	const std::string body = formatFeedbackComment(round, sampleDocument(), sampleCounts());
	const std::size_t note = body.find("実機を見ての所見");
	const std::size_t table = body.find("要素の内訳");
	CHECK(note != std::string::npos);
	CHECK(note < table); // 数字より上に置く
	CHECK(contains(body, "> 3 階の梁が 300mm 浮いている。"));
	CHECK(contains(body, "> 1 階は問題なし。"));
}

TEST(feedback_comment_shows_the_diff_from_the_previous_round)
{
	FeedbackRound round = sampleRound();
	round.round = 2;
	round.previousCommit = "9f8e7d6";
	round.previousTally = "横架材:2/4,柱:2/2";
	const std::string body = formatFeedbackComment(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "前の周（round 1 / 9f8e7d6）からの変化"));
	CHECK(contains(body, "横架材: 2/4 → 4/4"));
	CHECK(contains(body, "round 3 を投稿します"));
}

TEST(feedback_comment_says_when_nothing_changed)
{
	FeedbackRound round = sampleRound();
	round.round = 2;
	round.previousTally = "横架材:4/4,柱:2/2";
	const std::string body = formatFeedbackComment(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "内訳に変化はありません"));
}

TEST(feedback_comment_shows_the_size_in_kb_or_nothing)
{
	// 1 MB 未満は KB で出す。
	FeedbackRound small = sampleRound();
	small.bytes = 4096;
	CHECK(contains(formatFeedbackComment(small, sampleDocument(), sampleCounts()), "4.0 KB"));

	// 大きさが取れなかった（0）ときは括弧ごと出さない——「0 バイトのファイルを
	// 取り込んだ」と読み違えさせないため。
	FeedbackRound unknown = sampleRound();
	unknown.bytes = 0;
	unknown.seconds = 0.0;
	const std::string body = formatFeedbackComment(unknown, sampleDocument(), sampleCounts());
	CHECK(!contains(body, "0.0 KB"));
	CHECK(!contains(body, "所要"));
}

TEST(feedback_comment_says_when_there_are_no_commands)
{
	// 「取り込める要素が 1 つも無い」も報告の対象（ホームズ君の IFC かどうかを疑う場面）。
	const Document empty;
	DrawCounts counts;
	counts.valid = true;
	const std::string body = formatFeedbackComment(sampleRound(), empty, counts);
	CHECK(contains(body, "命令が 1 つも出ていません"));
	CHECK(contains(body, "対象なし"));
}

TEST(feedback_comment_folds_the_ordinary_notes)
{
	// 平常でも出る記録（用紙の割り付け等）は折り畳む——毎回開いて読むものではない。
	DrawCounts counts = sampleCounts();
	counts.notes = "伏図: 1:50 で 3 面";
	const std::string body = formatFeedbackComment(sampleRound(), sampleDocument(), counts);
	CHECK(contains(body, "記録（用紙の割り付けなど）"));
	CHECK(contains(body, "1:50 で 3 面"));
}

TEST(feedback_comment_shows_draw_diagnostics_unfolded)
{
	DrawCounts counts = sampleCounts();
	counts.members = 2;
	counts.diagnostics = "横架材: レイヤが無く置けなかった 2 本";
	const std::string body = formatFeedbackComment(sampleRound(), sampleDocument(), counts);
	CHECK(contains(body, "### 注意（描画側の異常）"));
	CHECK(contains(body, "レイヤが無く置けなかった"));
	// 異常は折り畳まない（読ませたいものを隠さない）。
	const std::size_t at = body.find("### 注意（描画側の異常）");
	CHECK(body.rfind("<details>", at) == std::string::npos);
}

TEST(feedback_comment_trims_an_oversized_log)
{
	FeedbackRound round = sampleRound();
	round.log = std::string(200000, 'x') + "\n最後の行\n";
	const std::string body = formatFeedbackComment(round, sampleDocument(), sampleCounts());
	CHECK(body.size() <= kMaxFeedbackCommentBytes);
	// 削るのは古いほう（結果に近い末尾を残す）。
	CHECK(contains(body, "最後の行"));
	CHECK(contains(body, "を省略"));
}

TEST(feedback_comment_without_auto_continue_says_so)
{
	FeedbackRound round = sampleRound();
	round.autoContinue = false;
	const std::string body = formatFeedbackComment(round, sampleDocument(), sampleCounts());
	CHECK(contains(body, "自動継続は切ってあるので"));
}

TEST_MAIN();
