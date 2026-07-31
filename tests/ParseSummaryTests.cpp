//
//	ParseSummaryTests.cpp
//
//	IFC 読み取りサマリ（src/parse/Summary）の単体テスト。無 SDK のテストハーネスで走る
//	（CLAUDE.md「テスト方針」）。ダイアログ表示（SDK 依存）を切り離し、件数集計と文言
//	整形だけを検証する。フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "parse/Loader.h"
#include "parse/Summary.h"

#include <string>

using namespace HomeskzIfcImport::parse;

namespace
{
	// counts の中から表示用 IFC 型名で件数を引く（見つからなければ最大値を返して
	// テストを分かりやすく落とす）。
	std::size_t countOf(const IfcSummary& summary, const std::string& ifcType)
	{
		for (const IfcTypeCount& c : summary.counts)
			if (c.ifcType == ifcType)
				return c.count;
		return static_cast<std::size_t>(-1);
	}
} // namespace

// ---------------------------------------------------------------------------
// 合成モデルでの集計
// ---------------------------------------------------------------------------

TEST(summarize_model_counts_known_types)
{
	// 通り芯 2・柱 1・金物 1。ほかの主要型は 0 件でも counts に並ぶ（固定順）。
	std::string const text = "DATA;\n"
							 "#1=IFCGRIDAXIS('X1',#10,.T.);\n"
							 "#2=IFCGRIDAXIS('X2',#10,.T.);\n"
							 "#3=IFCCOLUMN('c',$,'STANDCOLUMN',$,$,$,$,$,$);\n"
							 "#4=IFCMECHANICALFASTENER('m',$,$,$,$,$,$,$,$);\n"
							 "ENDSEC;\n";
	Model const model = loadIfcFromText(text);
	IfcSummary const summary = summarizeModel(model);

	CHECK(summary.loaded);
	// 主要型は固定の 7 種すべてが並ぶ。
	CHECK_EQ(summary.counts.size(), static_cast<std::size_t>(7));
	CHECK_EQ(countOf(summary, "IfcGridAxis"), static_cast<std::size_t>(2));
	CHECK_EQ(countOf(summary, "IfcColumn"), static_cast<std::size_t>(1));
	CHECK_EQ(countOf(summary, "IfcMechanicalFastener"), static_cast<std::size_t>(1));
	// 出現しない型も 0 件で含まれる（表示が入力でブレない）。
	CHECK_EQ(countOf(summary, "IfcBeam"), static_cast<std::size_t>(0));
	CHECK_EQ(countOf(summary, "IfcFooting"), static_cast<std::size_t>(0));
	// エンティティ総数は 4。
	CHECK_EQ(summary.entityCount, static_cast<std::size_t>(4));
}

TEST(summarize_model_order_is_grid_first)
{
	// 表示順の先頭は通り芯（M1 が最初の縦切り）。決定的な並びであることを固定する。
	Model const model = loadIfcFromText("#1=IFCGRIDAXIS('X1',$,.T.);\n");
	IfcSummary const summary = summarizeModel(model);
	CHECK(!summary.counts.empty());
	CHECK_EQ(summary.counts.front().ifcType, std::string("IfcGridAxis"));
	CHECK_EQ(summary.counts.front().label, std::string("通り芯"));
}

// ---------------------------------------------------------------------------
// 文言整形
// ---------------------------------------------------------------------------

TEST(format_summary_lists_counts)
{
	Model const model = loadIfcFromText("#1=IFCGRIDAXIS('X1',$,.T.);\n"
										"#2=IFCGRIDAXIS('X2',$,.T.);\n"
										"#3=IFCGRIDAXIS('X3',$,.T.);\n");
	std::string const text = formatSummary(summarizeModel(model));

	// 日本語ラベル・IFC 型名・件数が本文に出る（「通り芯 (IfcGridAxis): 3」）。
	CHECK(text.find("通り芯") != std::string::npos);
	CHECK(text.find("IfcGridAxis") != std::string::npos);
	CHECK(text.find("(IfcGridAxis): 3") != std::string::npos);
	CHECK(text.find("エンティティ総数: 3") != std::string::npos);
}

TEST(format_summary_reports_load_failure)
{
	// 読み込めなかったサマリ（loaded=false）は失敗メッセージになる。
	IfcSummary const failed; // 既定は loaded=false
	std::string const text = formatSummary(failed);
	CHECK(text.find("読み込めませんでした") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(summarize_ifc_reads_fixture)
{
	std::string const path = HomeskzIfcTests::fixturePath("minimal_grid.ifc");
	IfcSummary const summary = summarizeIfc(path);

	CHECK(summary.loaded);
	// minimal_grid.ifc は通り芯 3・階 1（LoaderTests と同じ既知の内訳）。
	CHECK_EQ(countOf(summary, "IfcGridAxis"), static_cast<std::size_t>(3));
	CHECK_EQ(countOf(summary, "IfcBuildingStorey"), static_cast<std::size_t>(1));
	CHECK(summary.entityCount > 0);
}

TEST(summarize_ifc_missing_file_reports_not_loaded)
{
	IfcSummary const summary = summarizeIfc(HomeskzIfcTests::fixturePath("does_not_exist.ifc"));
	CHECK(!summary.loaded);
	CHECK(summary.counts.empty());
	CHECK(formatSummary(summary).find("読み込めませんでした") != std::string::npos);
}

TEST_MAIN();
