//
//	CoreDrawTimingTests.cpp
//
//	描画の区間計測（src/core/DrawTiming）の単体テスト。VectorWorks SDK も STEP も使わない
//	純ロジック——名前ごとの累計・並び（決定性）・本文の整形——なので、無 SDK のテスト
//	ハーネスだけで完結する（CLAUDE.md「テスト方針」）。
//
//	**実際に何ミリ秒かかるか**は測らない（実描画はローカルの VectorWorks でしか走らず、
//	時間に依るテストは CI で必ず揺れる）。ここで担保するのは「積んだものが、積んだとおりに
//	数えられ、同じ並びで出てくる」ところまで。
//

#include "TestFramework.h"

#include "core/DrawTiming.h"

#include <cstddef>
#include <string>
#include <vector>

using HomeskzIfcImport::core::drawTiming;
using HomeskzIfcImport::core::TimingScope;
using HomeskzIfcImport::core::TimingTable;

// --- 名前ごとの累計 --------------------------------------------------------

TEST(add_accumulates_per_name)
{
	TimingTable table;
	table.add("A", 10.0);
	table.add("B", 3.0);
	table.add("A", 5.0);

	const std::vector<TimingTable::Entry> entries = table.sorted();
	CHECK_EQ(entries.size(), static_cast<std::size_t>(2));
	CHECK_EQ(entries[0].name, "A");
	CHECK(entries[0].milliseconds == 15.0);
	CHECK_EQ(entries[0].count, static_cast<std::size_t>(2));
	CHECK_EQ(entries[1].name, "B");
	CHECK_EQ(entries[1].count, static_cast<std::size_t>(1));
	CHECK(table.total() == 18.0);
}

TEST(add_counts_the_call_even_when_it_took_no_time)
{
	// 速すぎて 0 と出る区間も「呼ばれた回数」は数える——回数が 0 だと 1 回あたりを
	// 出せないし、「呼ばれていない」と読み違える。
	TimingTable table;
	table.add("A", 0.0);
	table.add("A", 0.0);
	CHECK_EQ(table.sorted()[0].count, static_cast<std::size_t>(2));
	CHECK(table.total() == 0.0);
}

TEST(add_treats_a_negative_span_as_zero)
{
	// 合計が減ると読む側が必ず混乱するので、負は 0 として積む（回数は数える）。
	TimingTable table;
	table.add("A", 5.0);
	table.add("A", -3.0);
	CHECK(table.total() == 5.0);
	CHECK_EQ(table.sorted()[0].count, static_cast<std::size_t>(2));
}

// --- 並び（決定性）--------------------------------------------------------

TEST(sorted_puts_the_slowest_first)
{
	TimingTable table;
	table.add("速い", 1.0);
	table.add("遅い", 100.0);
	table.add("中くらい", 10.0);

	const std::vector<TimingTable::Entry> entries = table.sorted();
	CHECK_EQ(entries[0].name, "遅い");
	CHECK_EQ(entries[1].name, "中くらい");
	CHECK_EQ(entries[2].name, "速い");
}

TEST(sorted_keeps_the_first_seen_order_on_a_tie)
{
	// 同じ時間の区間は積まれた順のまま。走らせるたびに並びが入れ替わると、周どうしの
	// 引き比べができない（CLAUDE.md「決定性を守る」）。
	TimingTable table;
	table.add("先", 7.0);
	table.add("後", 7.0);
	const std::vector<TimingTable::Entry> entries = table.sorted();
	CHECK_EQ(entries[0].name, "先");
	CHECK_EQ(entries[1].name, "後");
}

// --- 整形 ------------------------------------------------------------------

TEST(format_is_empty_when_nothing_was_measured)
{
	// 呼び出し側は AppendLine へ渡すだけでよい（空行を積ませない）。
	const TimingTable table;
	CHECK(table.empty());
	CHECK_EQ(table.format("描画の内訳"), "");
}

TEST(format_lists_the_heading_total_and_one_line_per_section)
{
	TimingTable table;
	table.add("構造材:リセット", 30.0);
	table.add("構造材:リセット", 30.0);
	table.add("構造材:名前解決", 10.0);

	CHECK_EQ(table.format("描画の内訳"), "描画の内訳（合計 70ms）:\n"
										 "  構造材:リセット 60ms（2 回・30.00ms/回）\n"
										 "  構造材:名前解決 10ms（1 回・10.00ms/回）");
}

// --- 集計先とスコープ ------------------------------------------------------

TEST(clear_empties_the_table)
{
	TimingTable table;
	table.add("A", 1.0);
	table.clear();
	CHECK(table.empty());
	CHECK(table.total() == 0.0);
}

// --- 入れ子の見張り --------------------------------------------------------

TEST(nested_scopes_are_reported_by_name)
{
	// 入れ子は禁じ手（core/DrawTiming.h「使う側の作法」）だが、**防がずに数える**——
	// 計測点は draw/ のあちこちに散るので、共有の関数が自分でも区間を開いていた、
	// という取りこぼしは必ず起きる。二重計上は消せないので、**気付けるようにする**。
	drawTiming().clear();
	{
		const TimingScope outer("外側");
		{
			const TimingScope inner("内側");
		}
	}
	CHECK_EQ(drawTiming().nested().size(), static_cast<std::size_t>(1));
	CHECK_EQ(drawTiming().nested()[0], "内側");
	// 報告の末尾に警告が出る（名前で分かる）。
	const std::string text = drawTiming().format("描画の内訳");
	CHECK(text.find("⚠ 入れ子になった区間") != std::string::npos);
	CHECK(text.find("内側") != std::string::npos);
	drawTiming().clear();
}

TEST(sibling_scopes_are_not_nested)
{
	// 同じ深さで順に開くのは入れ子ではない（構造材の「名前解決」「パラメータ書き」が
	// 交互に来る形がこれ）。
	drawTiming().clear();
	{
		const TimingScope first("先");
	}
	{
		const TimingScope second("後");
	}
	CHECK(drawTiming().nested().empty());
	CHECK(drawTiming().format("描画の内訳").find("⚠") == std::string::npos);
	drawTiming().clear();
}

TEST(the_same_nested_section_is_named_once)
{
	TimingTable table;
	table.noteNested("A");
	table.noteNested("A");
	table.noteNested("B");
	CHECK_EQ(table.nested().size(), static_cast<std::size_t>(2));
	CHECK_EQ(table.nested()[0], "A");
	CHECK_EQ(table.nested()[1], "B");
}

TEST(format_lists_every_nested_section_separated_by_commas)
{
	// 入れ子は 1 か所とは限らない。**全部の名前を出す**——1 つしか出さないと、直した
	// つもりで残っているもう 1 か所に気付けない。
	TimingTable table;
	table.add("外側", 10.0);
	table.noteNested("構造材:パス生成");
	table.noteNested("共通:構成層");

	CHECK_EQ(table.format("描画の内訳"), "描画の内訳（合計 10ms）:\n"
										 "  外側 10ms（1 回・10.00ms/回）\n"
										 "  ⚠ 入れ子になった区間（時間が外側にも積まれています）: "
										 "構造材:パス生成, 共通:構成層");
}

TEST(leaving_a_scope_that_was_never_entered_does_not_go_negative)
{
	// 0 を下回らせると、以後に本当に起きた入れ子を見逃す。
	TimingTable table;
	table.leaveScope();
	CHECK(!table.enterScope()); // 深さは 0 のまま＝最初の区間は入れ子ではない
	CHECK(table.enterScope());	// その中は入れ子
	table.leaveScope();
	table.leaveScope();
}

TEST(clear_also_forgets_the_nesting_watch)
{
	// 周ごとに測り直すので、前の周の入れ子の記録を持ち越さない。
	TimingTable table;
	table.noteNested("A");
	CHECK(table.enterScope() == false);
	table.clear();
	CHECK(table.nested().empty());
	CHECK(!table.enterScope()); // 深さも戻っている
	table.leaveScope();
}

TEST(scope_adds_one_entry_to_the_shared_table)
{
	// 計測点は draw/ のあちこちに散るが、集計先は drawTiming() ただ 1 つ
	// （core/DrawTiming.h「集計先が 1 つである理由」）。
	drawTiming().clear();
	{
		const TimingScope scope("区間");
	}
	CHECK_EQ(drawTiming().sorted().size(), static_cast<std::size_t>(1));
	CHECK_EQ(drawTiming().sorted()[0].name, "区間");
	CHECK_EQ(drawTiming().sorted()[0].count, static_cast<std::size_t>(1));
	drawTiming().clear();
}

TEST_MAIN();
