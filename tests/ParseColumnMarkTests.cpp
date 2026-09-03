//
//	ParseColumnMarkTests.cpp
//
//	断面記号・伏図記号の解析（src/parse/ColumnMark）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M12）: span レイヤごとに断面記号・伏図記号が 1 つずつ出ること・
//	配置先と検索対象レイヤ（断面記号＝span レイヤ自身／伏図記号＝"{to}-柱伏図記号"）・
//	作図クラス・シンボルの選択（柱／小屋束）・対象クラスが空＝全クラスであること・
//	並び（断面記号が先）・伏図記号レイヤの列挙と「切断の直下」の選び方・実フィクスチャ
//	での通し・並び順に依存しない決定性。実フィクスチャのパスは CMake が
//	HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Column.h"
#include "parse/ColumnMark.h"
#include "parse/Loader.h"
#include "parse/Story.h"
#include "core/ImportOptions.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::ColumnCommand;
using HomeskzIfcImport::core::ColumnMarkCommand;
using HomeskzIfcImport::core::ColumnMarkStyle;
using HomeskzIfcImport::core::defaultSymbolName;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildColumnCommands;
using HomeskzIfcImport::parse::buildColumnMarkCommands;
using HomeskzIfcImport::parse::collectColumnSpans;
using HomeskzIfcImport::parse::collectPlanMarkLayers;
using HomeskzIfcImport::parse::ColumnSpan;
using HomeskzIfcImport::parse::kPlanMarkClass;
using HomeskzIfcImport::parse::kSectionMarkClass;
using HomeskzIfcImport::parse::kStructuralUseColumn;
using HomeskzIfcImport::parse::kStructuralUseKoyazuka;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::PlanMarkLayer;
using HomeskzIfcImport::parse::planMarkLayerBelowCut;
using HomeskzIfcImport::parse::planMarkLayerName;
using HomeskzIfcTests::forEachFixture;
using HomeskzIfcTests::near;

namespace
{
	// 既定のシンボル名。**唯一の定義は役割の表**（core::symbolRoles()）なので、
	// テストもそこから引く（名前を書き写すと表と食い違っても気付けない）。
	const std::string kPlanMarkSymbolColumn = defaultSymbolName(SymbolRole::PlanMarkColumn);
	const std::string kPlanMarkSymbolKoyazuka = defaultSymbolName(SymbolRole::PlanMarkKoyazuka);

	// 試験用の柱 1 本。中心 (x, y)・断面 width×depth・span レイヤ・構造用途だけを持つ
	// （記号の組み立てはこの 4 つしか見ない）。
	ColumnCommand makeColumn(double x, double y, double width, double depth,
							 const std::string& layer, const char* use)
	{
		ColumnCommand column;
		column.layer = layer;
		column.structuralUse = use;
		column.position = Vec2{x, y};
		column.width = width;
		column.depth = depth;
		column.height = 2800.0;
		return column;
	}
} // namespace

TEST(EachSpanLayerGetsASectionMarkAndAPlanMark)
{
	// span レイヤ 1 つにつき断面記号 1 つ・伏図記号 1 つ。**断面記号が先**に全部並ぶ。
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn),
		makeColumn(500.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn), // 同じ span
		makeColumn(1000.0, 0.0, 90.0, 90.0, "2to2.5-柱", kStructuralUseKoyazuka)};

	const std::vector<ColumnMarkCommand> marks = buildColumnMarkCommands(columns);
	// span は 2 つ（"1to2-柱" / "2to2.5-柱"）なので記号は 4 つ。**柱の本数には依存しない**
	// ——記号は PIO が対象レイヤを検索して描くので、柱が何本あっても命令は 1 つ。
	CHECK(marks.size() == 4);
	CHECK(marks[0].style == ColumnMarkStyle::Section);
	CHECK(marks[1].style == ColumnMarkStyle::Section);
	CHECK(marks[2].style == ColumnMarkStyle::Plan);
	CHECK(marks[3].style == ColumnMarkStyle::Plan);
}

TEST(SectionMarkSitsOnAndSearchesItsOwnSpanLayer)
{
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn)};

	const std::vector<ColumnMarkCommand> marks = buildColumnMarkCommands(columns);
	CHECK(marks.size() == 2);

	const ColumnMarkCommand& section = marks[0];
	CHECK(section.layer == "1to2-柱");		 // 配置先＝span レイヤ自身
	CHECK(section.targetLayer == "1to2-柱"); // 検索対象も同じレイヤ
	CHECK(section.drawClass == kSectionMarkClass);
	CHECK(section.targetClass.empty()); // 空＝全クラス
	CHECK(section.symbol.empty());		// 断面記号はシンボルを使わない
}

TEST(PlanMarkGoesToItsOwnLayerButSearchesTheSpanLayer)
{
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn)};

	const std::vector<ColumnMarkCommand> marks = buildColumnMarkCommands(columns);
	CHECK(marks.size() == 2);

	const ColumnMarkCommand& plan = marks[1];
	CHECK(plan.layer == "2-柱伏図記号");  // 配置先＝span の to の専用レイヤ
	CHECK(plan.targetLayer == "1to2-柱"); // 検索対象は span レイヤ
	CHECK(plan.drawClass == kPlanMarkClass);
	CHECK(plan.targetClass.empty());
	CHECK(plan.symbol == kPlanMarkSymbolColumn);
}

TEST(PlanMarkSymbolFollowsTheSpanStructuralUse)
{
	// シンボルはその span レイヤの種別で決まる（柱＝柱伏図記号／小屋束＝束伏図記号）。
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn),
		makeColumn(0.0, 0.0, 90.0, 90.0, "2to2.5-柱", kStructuralUseKoyazuka)};

	const std::vector<ColumnMarkCommand> marks = buildColumnMarkCommands(columns);
	CHECK(marks.size() == 4);
	CHECK(marks[2].symbol == kPlanMarkSymbolColumn);   // "1to2-柱" の伏図記号
	CHECK(marks[3].symbol == kPlanMarkSymbolKoyazuka); // "2to2.5-柱" の伏図記号
}

TEST(SameSpanTopSharesOnePlanMarkLayer)
{
	// 同じ to の span（"1to2.5-柱" と "2to2.5-柱"）は同じ伏図記号レイヤに載る。
	// **記号は span ごとに 1 つずつ**なので、命令は 2 つ出て配置先だけが同じになる
	// （検索対象がそれぞれの span レイヤなので、まとめてしまうと片方が描かれない）。
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 90.0, 90.0, "1to2.5-柱", kStructuralUseKoyazuka),
		makeColumn(500.0, 0.0, 90.0, 90.0, "2to2.5-柱", kStructuralUseKoyazuka)};

	const std::vector<ColumnMarkCommand> marks = buildColumnMarkCommands(columns);
	CHECK(marks.size() == 4);
	CHECK(marks[2].layer == "2.5-柱伏図記号");
	CHECK(marks[3].layer == "2.5-柱伏図記号");
	CHECK(marks[2].targetLayer == "1to2.5-柱");
	CHECK(marks[3].targetLayer == "2to2.5-柱");
}

TEST(NoColumnsMeansNoMarks)
{
	CHECK(buildColumnMarkCommands({}).empty());
}

TEST(PlanMarkLayerNameUsesTheSpanTopLevel)
{
	// レイヤ名は span の上側の数値。整数は小数点なし・半整数は ".5" 付き（span 柱レイヤの
	// 表記と同じ formatSpanLevel を通る）。
	CHECK(planMarkLayerName(2.0) == "2-柱伏図記号");
	CHECK(planMarkLayerName(2.5) == "2.5-柱伏図記号");
	CHECK(planMarkLayerName(3.0) == "3-柱伏図記号");
}

TEST(PlanMarkLayersAreUniqueAndSortedByLevel)
{
	// span は (from, to) 昇順で来るが to は単調でない（"1to3" の次に "2to2.5"）。
	// 列挙は to 昇順・重複なし。
	const std::vector<ColumnSpan> spans{{1.0, 2.0, "1to2-柱"},
										{1.0, 3.0, "1to3-柱"},
										{2.0, 2.5, "2to2.5-柱"},
										{2.0, 3.0, "2to3-柱"},
										{3.0, 3.5, "3to3.5-柱"}};

	const std::vector<PlanMarkLayer> layers = collectPlanMarkLayers(spans);
	CHECK(layers.size() == 4);
	CHECK(layers[0].layer == "2-柱伏図記号");
	CHECK(layers[1].layer == "2.5-柱伏図記号");
	CHECK(layers[2].layer == "3-柱伏図記号");
	CHECK(layers[3].layer == "3.5-柱伏図記号");
}

TEST(PlanMarkLayerBelowCutPicksTheHighestLevelUnderTheCut)
{
	const std::vector<PlanMarkLayer> layers{{2.0, "2-柱伏図記号"},
											{2.5, "2.5-柱伏図記号"},
											{3.0, "3-柱伏図記号"},
											{3.5, "3.5-柱伏図記号"}};

	// 2 階床伏図（切断 2.25）は下階＝1 階管柱 "1to2-柱" の平面記号。
	CHECK(planMarkLayerBelowCut(layers, 2.25) == "2-柱伏図記号");
	// 1 階母屋伏図（切断 2.75）は下屋小屋束 "2to2.5-柱" の平面記号。
	CHECK(planMarkLayerBelowCut(layers, 2.75) == "2.5-柱伏図記号");
	// 2 階小屋伏図（切断 3.25）。
	CHECK(planMarkLayerBelowCut(layers, 3.25) == "3-柱伏図記号");
	// 1 階床伏図（切断 1.25）より下のレベルは無い＝載せる伏図記号レイヤが無い。
	CHECK(planMarkLayerBelowCut(layers, 1.25).empty());
	// **切断ちょうどのレベルは「直下」ではない**（そのレベルの柱は断面記号で出るので、
	// 伏図記号でも出すと二重になる）。
	CHECK(planMarkLayerBelowCut(layers, 2.0).empty());
}

TEST(MarksMatchTheSpansOnEveryFixture)
{
	// 実フィクスチャ通し: 記号は span レイヤ 1 つにつき 2 つ（断面記号・伏図記号）で、
	// 検索対象レイヤは必ず実在する span レイヤ、伏図記号のシンボルはその span の種別と
	// 一致すること。
	forEachFixture(failures,
				   [&](const std::string&, const Model& model)
				   {
					   const std::vector<ColumnCommand> columns = buildColumnCommands(model);
					   const std::vector<ColumnSpan> spans = collectColumnSpans(columns);
					   const std::vector<ColumnMarkCommand> marks =
						   buildColumnMarkCommands(columns);
					   CHECK(marks.size() == spans.size() * 2);

					   std::set<std::string> spanLayers;
					   for (const ColumnSpan& span : spans)
						   spanLayers.insert(span.layer);

					   std::set<std::string> markLayers;
					   for (const PlanMarkLayer& layer : collectPlanMarkLayers(spans))
						   markLayers.insert(layer.layer);

					   for (const ColumnMarkCommand& mark : marks)
					   {
						   CHECK(spanLayers.count(mark.targetLayer) == 1);
						   if (mark.style == ColumnMarkStyle::Section)
						   {
							   CHECK(mark.layer == mark.targetLayer);
							   CHECK(mark.symbol.empty());
						   }
						   else
						   {
							   CHECK(markLayers.count(mark.layer) == 1);
							   CHECK(!mark.symbol.empty());
						   }
					   }
				   });
}

TEST(MarkCommandsAreDeterministic)
{
	// 同じ入力から 2 度組み立てても同じ並び・同じ値（CLAUDE.md「決定性を守る」）。
	forEachFixture(failures,
				   [&](const std::string&, const Model& model)
				   {
					   const std::vector<ColumnCommand> columns = buildColumnCommands(model);
					   const std::vector<ColumnMarkCommand> a = buildColumnMarkCommands(columns);
					   const std::vector<ColumnMarkCommand> b = buildColumnMarkCommands(columns);
					   CHECK(a.size() == b.size());
					   for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
					   {
						   CHECK(a[i].layer == b[i].layer);
						   CHECK(a[i].targetLayer == b[i].targetLayer);
						   CHECK(a[i].symbol == b[i].symbol);
						   CHECK(a[i].style == b[i].style);
					   }
				   });
}

TEST_MAIN();
