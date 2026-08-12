//
//	ParseColumnMarkTests.cpp
//
//	断面記号・伏図記号の解析（src/parse/ColumnMark）の単体テスト。VectorWorks SDK を
//	一切 include せず、無 SDK のテストハーネス（TestFramework.h）で走る
//	（CLAUDE.md「テスト方針」）。Python 版 test_ifc_column_mark.py が見ていた性質を
//	写しつつ、**記号の絵を解析側で持つ**という本移植の差（PIO を使わない。
//	parse/ColumnMark.h）に合わせて期待値を書き直してある（手書き。
//	ROADMAP.md「Python 版出力との比較はしない」）。
//
//	検証項目（ROADMAP.md M12）: 断面記号の線分が実断面の対角線であること・柱は ×
//	（2 本）／小屋束は ／（1 本）・配置先が柱と同じ span レイヤであること・伏図記号の
//	レイヤ名 "{to}-柱伏図記号"・タグスタイルの選択・関連付け先の添字・退化した断面と
//	span でない配置先の除外・伏図記号レイヤの列挙と「切断の直下」の選び方・実フィクスチャ
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

#include <cstddef>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::ColumnCommand;
using HomeskzIfcImport::core::ColumnPlanMarkCommand;
using HomeskzIfcImport::core::ColumnSectionMarkCommand;
using HomeskzIfcImport::core::MarkSegment;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildColumnCommands;
using HomeskzIfcImport::parse::buildColumnPlanMarkCommands;
using HomeskzIfcImport::parse::buildColumnSectionMarkCommands;
using HomeskzIfcImport::parse::collectColumnSpans;
using HomeskzIfcImport::parse::collectPlanMarkLayers;
using HomeskzIfcImport::parse::ColumnSpan;
using HomeskzIfcImport::parse::kPlanMarkClass;
using HomeskzIfcImport::parse::kPlanMarkStyleColumn;
using HomeskzIfcImport::parse::kPlanMarkStyleKoyazuka;
using HomeskzIfcImport::parse::kSectionMarkClass;
using HomeskzIfcImport::parse::kStructuralUseColumn;
using HomeskzIfcImport::parse::kStructuralUseKoyazuka;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::PlanMarkLayer;
using HomeskzIfcImport::parse::planMarkLayerBelowCut;
using HomeskzIfcImport::parse::planMarkLayerName;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
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

	// 線分が (ax, ay)-(bx, by) か（向きも含めて）。
	bool isSegment(const MarkSegment& segment, double ax, double ay, double bx, double by)
	{
		return near(segment.start.x, ax) && near(segment.start.y, ay) && near(segment.end.x, bx) &&
			   near(segment.end.y, by);
	}
} // namespace

TEST(SectionMarkIsTheDiagonalsOfTheActualSection)
{
	// 柱（構造用途 4）は × ＝実断面の外接矩形の 2 本の対角線。105×120 の柱を
	// (1000, 2000) に置くと、隅は x=947.5/1052.5・y=1940/2060 になる。
	const std::vector<ColumnCommand> columns{
		makeColumn(1000.0, 2000.0, 105.0, 120.0, "1to2-柱", kStructuralUseColumn)};

	const std::vector<ColumnSectionMarkCommand> marks = buildColumnSectionMarkCommands(columns);
	CHECK(marks.size() == 1);
	CHECK(marks[0].layer == "1to2-柱"); // 柱と同じ span レイヤに重ねる
	CHECK(marks[0].drawClass == kSectionMarkClass);
	CHECK(marks[0].segments.size() == 2);
	CHECK(isSegment(marks[0].segments[0], 947.5, 1940.0, 1052.5, 2060.0)); // 左下→右上
	CHECK(isSegment(marks[0].segments[1], 1052.5, 1940.0, 947.5, 2060.0)); // 右下→左上
}

TEST(SectionMarkForKoyazukaIsASingleSlash)
{
	// 小屋束（構造用途 5）は ／ ＝左下→右上の 1 本だけ。
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 90.0, 90.0, "2to2.5-柱", kStructuralUseKoyazuka)};

	const std::vector<ColumnSectionMarkCommand> marks = buildColumnSectionMarkCommands(columns);
	CHECK(marks.size() == 1);
	CHECK(marks[0].segments.size() == 1);
	CHECK(isSegment(marks[0].segments[0], -45.0, -45.0, 45.0, 45.0));
}

TEST(SectionMarkSkipsDegenerateSections)
{
	// 幅・せいのどちらかが 0 以下の柱は縮退した線しか引けないので記号を作らない。
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 0.0, 105.0, "1to2-柱", kStructuralUseColumn),
		makeColumn(0.0, 0.0, 105.0, -1.0, "1to2-柱", kStructuralUseColumn),
		makeColumn(0.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn)};

	CHECK(buildColumnSectionMarkCommands(columns).size() == 1);
}

TEST(PlanMarkLayerNameUsesTheSpanTopLevel)
{
	// レイヤ名は span の上側の数値。整数は小数点なし・半整数は ".5" 付き（span 柱レイヤの
	// 表記と同じ formatSpanLevel を通る）。
	CHECK(planMarkLayerName(2.0) == "2-柱伏図記号");
	CHECK(planMarkLayerName(2.5) == "2.5-柱伏図記号");
	CHECK(planMarkLayerName(3.0) == "3-柱伏図記号");
}

TEST(PlanMarkGoesToTheLayerOfItsSpanTop)
{
	// 伏図記号は "{to}-柱伏図記号" へ。**同じ to の span は同じレイヤに載る**
	// （"1to2.5-柱" と "2to2.5-柱" → どちらも "2.5-柱伏図記号"）。
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn),
		makeColumn(500.0, 0.0, 90.0, 90.0, "1to2.5-柱", kStructuralUseKoyazuka),
		makeColumn(1000.0, 0.0, 90.0, 90.0, "2to2.5-柱", kStructuralUseKoyazuka)};

	const std::vector<ColumnPlanMarkCommand> marks = buildColumnPlanMarkCommands(columns);
	CHECK(marks.size() == 3);
	CHECK(marks[0].layer == "2-柱伏図記号");
	CHECK(marks[1].layer == "2.5-柱伏図記号");
	CHECK(marks[2].layer == "2.5-柱伏図記号");
}

TEST(PlanMarkStyleFollowsTheStructuralUse)
{
	// 記号の絵はタグスタイルが持ち、スタイルは構造用途で選ぶ（柱＝柱伏図記号／
	// 小屋束＝束伏図記号）。挿入点は柱の断面中心、関連付け先は columns の添字。
	const std::vector<ColumnCommand> columns{
		makeColumn(100.0, 200.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn),
		makeColumn(300.0, 400.0, 90.0, 90.0, "2to2.5-柱", kStructuralUseKoyazuka)};

	const std::vector<ColumnPlanMarkCommand> marks = buildColumnPlanMarkCommands(columns);
	CHECK(marks.size() == 2);

	CHECK(marks[0].styleName == kPlanMarkStyleColumn);
	CHECK(marks[0].drawClass == kPlanMarkClass);
	CHECK(marks[0].columnIndex == 0);
	CHECK(near(marks[0].position.x, 100.0) && near(marks[0].position.y, 200.0));

	CHECK(marks[1].styleName == kPlanMarkStyleKoyazuka);
	CHECK(marks[1].columnIndex == 1);
	CHECK(near(marks[1].position.x, 300.0) && near(marks[1].position.y, 400.0));
}

TEST(PlanMarkSkipsColumnsThatAreNotOnASpanLayer)
{
	// 配置先が span レイヤでない柱は to が決まらないので記号を作らない（存在しない
	// レイヤ名の命令を出すより、記号が出ないほうが原因を追いやすい）。**添字は
	// columns のものを保つ**ので、飛ばした後の柱も正しい柱を指す。
	const std::vector<ColumnCommand> columns{
		makeColumn(0.0, 0.0, 105.0, 105.0, "1-柱", kStructuralUseColumn),
		makeColumn(0.0, 0.0, 105.0, 105.0, "1to2-柱", kStructuralUseColumn)};

	const std::vector<ColumnPlanMarkCommand> marks = buildColumnPlanMarkCommands(columns);
	CHECK(marks.size() == 1);
	CHECK(marks[0].columnIndex == 1);
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

TEST(MarksMatchTheColumnsOnEveryFixture)
{
	// 実フィクスチャ通し: 断面記号は「断面が正の柱」と同数、伏図記号は「span レイヤに
	// 載る柱」と同数で、どの命令も実在する柱を指し、配置先レイヤの規約が守られていること。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<ColumnCommand> columns = buildColumnCommands(model);
		const std::vector<ColumnSectionMarkCommand> sections =
			buildColumnSectionMarkCommands(columns);
		const std::vector<ColumnPlanMarkCommand> plans = buildColumnPlanMarkCommands(columns);

		std::size_t drawable = 0;
		for (const ColumnCommand& column : columns)
			if (column.width > 0.0 && column.depth > 0.0)
				++drawable;
		CHECK(sections.size() == drawable);

		// 伏図記号のレイヤは、そのフィクスチャの span から作られる名前のどれかに一致する。
		std::set<std::string> known;
		for (const PlanMarkLayer& layer : collectPlanMarkLayers(collectColumnSpans(columns)))
			known.insert(layer.layer);
		for (const ColumnPlanMarkCommand& mark : plans)
		{
			CHECK(known.count(mark.layer) == 1);
			CHECK(mark.columnIndex < columns.size());
			// スタイルは関連付け先の柱の構造用途と一致する。
			const bool koyazuka = columns[mark.columnIndex].structuralUse == kStructuralUseKoyazuka;
			CHECK(mark.styleName == (koyazuka ? kPlanMarkStyleKoyazuka : kPlanMarkStyleColumn));
		}

		// 断面記号は柱と同じレイヤに載る（伏図が span レイヤを映せば記号も併せて出る）。
		std::set<std::string> columnLayers;
		for (const ColumnCommand& column : columns)
			columnLayers.insert(column.layer);
		for (const ColumnSectionMarkCommand& mark : sections)
			CHECK(columnLayers.count(mark.layer) == 1);
	}
}

TEST(MarkCommandsAreDeterministic)
{
	// 同じ入力から 2 度組み立てても同じ並び・同じ値（CLAUDE.md「決定性を守る」）。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<ColumnCommand> columns = buildColumnCommands(model);
		const std::vector<ColumnSectionMarkCommand> sectionsA =
			buildColumnSectionMarkCommands(columns);
		const std::vector<ColumnSectionMarkCommand> sectionsB =
			buildColumnSectionMarkCommands(columns);
		CHECK(sectionsA.size() == sectionsB.size());
		for (std::size_t i = 0; i < sectionsA.size() && i < sectionsB.size(); ++i)
		{
			CHECK(sectionsA[i].layer == sectionsB[i].layer);
			CHECK(sectionsA[i].segments.size() == sectionsB[i].segments.size());
		}

		const std::vector<ColumnPlanMarkCommand> plansA = buildColumnPlanMarkCommands(columns);
		const std::vector<ColumnPlanMarkCommand> plansB = buildColumnPlanMarkCommands(columns);
		CHECK(plansA.size() == plansB.size());
		for (std::size_t i = 0; i < plansA.size() && i < plansB.size(); ++i)
		{
			CHECK(plansA[i].layer == plansB[i].layer);
			CHECK(plansA[i].styleName == plansB[i].styleName);
			CHECK(plansA[i].columnIndex == plansB[i].columnIndex);
		}
	}
}

TEST_MAIN();
