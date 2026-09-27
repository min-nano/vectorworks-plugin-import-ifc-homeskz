//
//	ParseDimensionTests.cpp
//
//	寸法（src/parse/Dimension。docs/DEV-NOTES.md M31）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（通り芯 910 / 1820 の格子に部材を置いた小さな文書で手計算した値）。
//
//	検証項目:
//	  * 測点のまとめ方——昇順・許容以内は 1 点・部材と通り芯が重なれば通り芯の値が残る。
//	  * 伏図の外周の列——上と左に「部材＋通り芯 → 通り芯 → 全長」、下と右に通り芯の間隔。
//	    部材が通り芯とすべて重なるなら 1 段目を出さない。通り芯が無ければ部材の位置で代える。
//	  * 基礎伏図の立上りに沿う列——アンカーボルト・立上りの端・横切る通り芯が測点になり、
//	    通り芯とすべて重なる通りには作らない。列は図の外側へ出す。
//	  * 伏図の種類ごとに押さえるもの——床伏図は柱と梁（表示レイヤに載るものだけ）、
//	    母屋伏図は母屋だけ。
//	  * 軸組図——柱の位置と通り芯（図の下）、GL・FL・軒高と標準の横架材天端（図の左）、
//	    標準と違う高さの横架材の高さ、レベル記号。
//	  * 実フィクスチャ——寸法を入れた文書が検証を通り、何度組み立てても同じになること。
//	    寸法を入れない設定では 1 つも作らないこと。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "core/Progress.h"
#include "parse/BuildDocument.h"
#include "parse/Dimension.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::ColumnCommand;
using HomeskzIfcImport::core::DimensionAxis;
using HomeskzIfcImport::core::DimensionChainCommand;
using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::GridCommand;
using HomeskzIfcImport::core::LevelCommand;
using HomeskzIfcImport::core::LevelMarkCommand;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::PlanKind;
using HomeskzIfcImport::core::SectionCommand;
using HomeskzIfcImport::core::SectionDirection;
using HomeskzIfcImport::core::SheetCommand;
using HomeskzIfcImport::core::StoryCommand;
using HomeskzIfcImport::core::SymbolCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::WallCommand;
using HomeskzIfcTests::forEachFixtureDocument;
using HomeskzIfcTests::near;

namespace
{
	bool sameValues(const std::vector<double>& actual, const std::vector<double>& expected)
	{
		if (actual.size() != expected.size())
			return false;
		for (std::size_t i = 0; i < actual.size(); ++i)
		{
			if (!near(actual[i], expected[i]))
				return false;
		}
		return true;
	}

	// 列 1 つが期待どおりか（測る向き・測点・根元・向き・段）。
	bool isChain(const DimensionChainCommand& chain, DimensionAxis axis,
				 const std::vector<double>& stops, double base, int side, int tier)
	{
		return chain.axis == axis && sameValues(chain.stops, stops) && near(chain.base, base) &&
			   chain.side == side && chain.tier == tier;
	}

	GridCommand makeGrid(const std::string& label, Vec2 start, Vec2 end)
	{
		GridCommand grid;
		grid.label = label;
		grid.drawClass = "通り芯";
		grid.start = start;
		grid.end = end;
		return grid;
	}

	MemberCommand makeMember(const std::string& layer, Vec2 start, Vec2 end, double elevation)
	{
		MemberCommand member;
		member.layer = layer;
		member.memberId = "105×180";
		member.drawClass = "横架材";
		member.start = start;
		member.end = end;
		member.width = 105.0;
		member.height = 180.0;
		member.elevation = elevation;
		member.endElevation = elevation;
		return member;
	}

	ColumnCommand makeColumn(const std::string& layer, Vec2 position)
	{
		ColumnCommand column;
		column.layer = layer;
		column.memberId = "105×105";
		column.drawClass = "柱";
		column.position = position;
		column.width = 105.0;
		column.depth = 105.0;
		column.height = 2800.0;
		column.elevation = 500.0;
		return column;
	}

	StoryCommand makeStory(const std::string& name, double elevation,
						   std::vector<LevelCommand> levels)
	{
		StoryCommand story;
		story.name = name;
		story.suffix = name;
		story.elevation = elevation;
		story.levels = std::move(levels);
		return story;
	}

	// 基礎・1 階（500）・2 階（3300）・屋根（軒高 6000）。横架材天端は各階 FL −36。
	std::vector<StoryCommand> twoStoreyStories()
	{
		return {
			makeStory("F", 0.0, {LevelCommand{core::kLevelGL, 0.0, "F-立上り"}}),
			makeStory("1", 500.0,
					  {LevelCommand{core::kLevelFL, 0.0, "1-FL"},
					   LevelCommand{core::kLevelBeamTop, -36.0, "1-横架材天端"}}),
			makeStory("2", 3300.0,
					  {LevelCommand{core::kLevelFL, 0.0, "2-FL"},
					   LevelCommand{core::kLevelBeamTop, -36.0, "2-横架材天端"},
					   LevelCommand{core::kLevelMoya, 200.0, "2-母屋"},
					   LevelCommand{core::kLevelNoboribari, 200.0, "2-登り梁"}}),
			makeStory("R", 6000.0, {LevelCommand{core::kLevelEaves, 0.0, "R-軒高"}}),
		};
	}

	// X1（x=0）・X2（910）・X3（1820）と Y1（y=0）・Y2（1820）の通り芯。
	std::vector<GridCommand> smallGrid()
	{
		return {
			makeGrid("X1", Vec2{0.0, -1000.0}, Vec2{0.0, 2820.0}),
			makeGrid("X2", Vec2{910.0, -1000.0}, Vec2{910.0, 2820.0}),
			makeGrid("X3", Vec2{1820.0, -1000.0}, Vec2{1820.0, 2820.0}),
			makeGrid("Y1", Vec2{-1000.0, 0.0}, Vec2{2820.0, 0.0}),
			makeGrid("Y2", Vec2{-1000.0, 1820.0}, Vec2{2820.0, 1820.0}),
		};
	}

	SheetCommand makeSheet(PlanKind kind, std::vector<std::string> layers)
	{
		SheetCommand sheet;
		sheet.number = "2";
		sheet.title = "伏図";
		sheet.kind = kind;
		sheet.viewport.layers = std::move(layers);
		sheet.viewport.layers.emplace_back(core::kGridLayer);
		return sheet;
	}

	// 列のうち、指定の向き・側・段のものを探す（無ければ nullptr）。
	const DimensionChainCommand* findChain(const std::vector<DimensionChainCommand>& chains,
										   DimensionAxis axis, int side, int tier)
	{
		for (const DimensionChainCommand& chain : chains)
		{
			if (chain.axis == axis && chain.side == side && chain.tier == tier)
				return &chain;
		}
		return nullptr;
	}
} // namespace

TEST(MergeStopsSortsAndMergesNearbyValues)
{
	// 0.5 は 0 に、910.4 は 910 にまとまる（許容 1mm）。まとめた群は最小の値を残す。
	CHECK(sameValues(parse::mergeStops({1820.0, 0.0, 0.5, 910.0, 910.4}), {0.0, 910.0, 1820.0}));
	CHECK(parse::mergeStops({}).empty());
}

TEST(UnionStopsKeepsTheGridValue)
{
	// 部材（0.5・909.8）が通り芯（0・910）の上にあれば通り芯の値が残り、455 だけが足される。
	CHECK(sameValues(parse::unionStops({0.0, 910.0}, {0.5, 455.0, 909.8}), {0.0, 455.0, 910.0}));
}

TEST(GridStopsSplitByDirection)
{
	std::vector<GridCommand> grids = smallGrid();
	// 同じ通りの別区間（910.3）はまとまる。
	grids.push_back(makeGrid("X2", Vec2{910.3, 3000.0}, Vec2{910.3, 4000.0}));
	CHECK(sameValues(parse::gridStops(grids, DimensionAxis::Horizontal), {0.0, 910.0, 1820.0}));
	CHECK(sameValues(parse::gridStops(grids, DimensionAxis::Vertical), {0.0, 1820.0}));
}

TEST(PerimeterChainsStackOutward)
{
	const Vec2 min{-300.0, -300.0};
	const Vec2 max{2120.0, 2120.0};
	const std::vector<DimensionChainCommand> chains = parse::perimeterDimensionChains(
		{0.0, 455.0, 910.0}, {}, {0.0, 910.0, 1820.0}, {0.0, 1820.0}, min, max, 0);

	CHECK(chains.size() == 6);
	if (chains.size() != 6)
		return;
	// 上: 部材＋通り芯 → 通り芯 → 全長。下: 通り芯の間隔。
	CHECK(isChain(chains[0], DimensionAxis::Horizontal, {0.0, 455.0, 910.0, 1820.0}, 2120.0, 1, 0));
	CHECK(isChain(chains[1], DimensionAxis::Horizontal, {0.0, 910.0, 1820.0}, 2120.0, 1, 1));
	CHECK(isChain(chains[2], DimensionAxis::Horizontal, {0.0, 1820.0}, 2120.0, 1, 2));
	CHECK(isChain(chains[3], DimensionAxis::Horizontal, {0.0, 910.0, 1820.0}, -300.0, -1, 0));
	// 左: 部材が無い（＝通り芯と同じ）ので 1 段目は出さず、通り芯が 2 本なので全長も出さない。
	// 右: 通り芯の間隔。
	CHECK(isChain(chains[4], DimensionAxis::Vertical, {0.0, 1820.0}, -300.0, -1, 0));
	CHECK(isChain(chains[5], DimensionAxis::Vertical, {0.0, 1820.0}, 2120.0, 1, 0));
}

TEST(PerimeterChainsFallBackToMembersWithoutGrids)
{
	const std::vector<DimensionChainCommand> chains = parse::perimeterDimensionChains(
		{0.0, 1000.0, 3000.0}, {}, {}, {}, Vec2{-300.0, -300.0}, Vec2{3300.0, 300.0}, 1);

	// 通り芯が無ければ部材の位置が「間隔」を担う（同じ列を 2 段に重ねない）。
	CHECK(chains.size() == 3);
	if (chains.size() != 3)
		return;
	CHECK(isChain(chains[0], DimensionAxis::Horizontal, {0.0, 1000.0, 3000.0}, 300.0, 1, 1));
	CHECK(isChain(chains[1], DimensionAxis::Horizontal, {0.0, 3000.0}, 300.0, 1, 2));
	CHECK(isChain(chains[2], DimensionAxis::Horizontal, {0.0, 1000.0, 3000.0}, -300.0, -1, 1));
}

TEST(FoundationWallChainsFollowEachWallLine)
{
	// Y1 の立上りが X2 と X3 の間で切れている（−100〜1820 と 2500〜3640）。X1 の立上りは
	// 通り芯の上で始まり通り芯の上で終わる。
	auto wall = [](Vec2 start, Vec2 end)
	{
		WallCommand w;
		w.layer = "F-立上り";
		w.drawClass = "立上り";
		w.start = start;
		w.end = end;
		w.thickness = 150.0;
		return w;
	};
	const std::vector<WallCommand> walls{wall(Vec2{-100.0, 0.0}, Vec2{1820.0, 0.0}),
										 wall(Vec2{2500.0, 0.0}, Vec2{3640.0, 0.0}),
										 wall(Vec2{0.0, 0.0}, Vec2{0.0, 2000.0})};
	auto bolt = [](Vec2 position)
	{
		SymbolCommand s;
		s.layer = "F-アンカーボルト";
		s.symbol = "アンカーボルト_M12";
		s.position = position;
		return s;
	};
	// 3 本目は芯から 10mm ずれても立上りの上（厚みの半分以内）。4 本目は立上りの外。
	const std::vector<SymbolCommand> bolts{bolt(Vec2{200.0, 0.0}), bolt(Vec2{1620.0, 0.0}),
										   bolt(Vec2{3000.0, 10.0}), bolt(Vec2{1000.0, 500.0})};
	const std::vector<GridCommand> grids{
		makeGrid("X1", Vec2{0.0, -1000.0}, Vec2{0.0, 3000.0}),
		makeGrid("X2", Vec2{1820.0, -1000.0}, Vec2{1820.0, 3000.0}),
		makeGrid("X3", Vec2{3640.0, -1000.0}, Vec2{3640.0, 3000.0}),
		makeGrid("Y1", Vec2{-1000.0, 0.0}, Vec2{4640.0, 0.0}),
		makeGrid("Y2", Vec2{-1000.0, 2000.0}, Vec2{4640.0, 2000.0}),
	};

	const std::vector<DimensionChainCommand> chains =
		parse::foundationWallDimensionChains(walls, bolts, grids, Vec2{1820.0, 1000.0});

	// X1 の立上り（端が通り芯 Y1・Y2 の上）は通り芯の間隔の繰り返しになるので作らない。
	CHECK(chains.size() == 1);
	if (chains.size() != 1)
		return;
	// Y1 は図の中心より下なので下（−Y）へ出す。
	CHECK(isChain(chains[0], DimensionAxis::Horizontal,
				  {-100.0, 0.0, 200.0, 1620.0, 1820.0, 2500.0, 3000.0, 3640.0}, 0.0, -1, 0));
}

TEST(FramingPlanDimensionsColumnsAndBeamsOnItsLayers)
{
	Document document;
	document.stories = twoStoreyStories();
	document.grids = smallGrid();
	document.columns = {makeColumn("1to2-柱", Vec2{0.0, 0.0}),
						makeColumn("1to2-柱", Vec2{455.0, 1820.0}),
						// 別の階の柱は映らないので押さえない。
						makeColumn("2to3-柱", Vec2{1365.0, 0.0})};
	document.members = {// 東西に走る梁 → Y=910 を押さえる。
						makeMember("2-横架材天端", Vec2{0.0, 910.0}, Vec2{1820.0, 910.0}, 3264.0),
						// 斜めの材は押さえない。
						makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{910.0, 700.0}, 3264.0),
						// 表示レイヤに無い材は押さえない。
						makeMember("1-横架材天端", Vec2{1500.0, 0.0}, Vec2{1500.0, 1820.0}, 464.0)};
	const SheetCommand sheet = makeSheet(PlanKind::Framing, {"2-横架材天端", "1to2-柱"});

	const std::vector<DimensionChainCommand> chains =
		parse::buildPlanDimensionCommands(document, sheet);

	// 上の 1 段目: 通り芯 0 / 910 / 1820 に柱の 455 が加わる（1365・1500 は入らない）。
	const DimensionChainCommand* top = findChain(chains, DimensionAxis::Horizontal, 1, 0);
	CHECK(top != nullptr);
	if (top != nullptr)
		CHECK(sameValues(top->stops, {0.0, 455.0, 910.0, 1820.0}));
	// 左の 1 段目: 通り芯 0 / 1820 に梁の 910 が加わる。
	const DimensionChainCommand* left = findChain(chains, DimensionAxis::Vertical, -1, 0);
	CHECK(left != nullptr);
	if (left != nullptr)
		CHECK(sameValues(left->stops, {0.0, 910.0, 1820.0}));
}

TEST(MoyaPlanDimensionsOnlyMoya)
{
	Document document;
	document.stories = twoStoreyStories();
	document.grids = smallGrid();
	document.members = {// 母屋（南北に走る）→ X=455 を押さえる。
						makeMember("2-母屋", Vec2{455.0, 0.0}, Vec2{455.0, 1820.0}, 4000.0),
						// 登り梁は押さえない。
						makeMember("2-登り梁", Vec2{1365.0, 0.0}, Vec2{1365.0, 1820.0}, 4000.0)};
	const SheetCommand sheet = makeSheet(PlanKind::Moya, {"2-母屋", "2-登り梁"});

	const std::vector<DimensionChainCommand> chains =
		parse::buildPlanDimensionCommands(document, sheet);
	const DimensionChainCommand* top = findChain(chains, DimensionAxis::Horizontal, 1, 0);
	CHECK(top != nullptr);
	if (top != nullptr)
		CHECK(sameValues(top->stops, {0.0, 455.0, 910.0, 1820.0}));
}

TEST(PlanWithoutContentHasNoDimensions)
{
	Document document;
	document.grids = smallGrid();
	// 通り芯しか映らない伏図は外形が決まらない（通り芯は外形に数えない）。
	CHECK(parse::buildPlanDimensionCommands(document, makeSheet(PlanKind::Framing, {})).empty());
}

namespace
{
	// X=0 の通りを切る軸組図（断面線は y=−5000 → 5000。注釈の横の原点は終点の y=5000）。
	SectionCommand xSection()
	{
		SectionCommand section;
		section.direction = SectionDirection::X;
		section.lineStart = Vec2{0.0, -5000.0};
		section.lineEnd = Vec2{0.0, 5000.0};
		section.viewPoint = Vec2{-1000.0, 0.0};
		section.viewport.drawingNumber = "X1";
		section.viewport.drawingTitle = "X1通り";
		section.viewport.layers = {"1to2-柱", core::kGridLayer};
		return section;
	}

	Document sectionDocument()
	{
		Document document;
		document.stories = twoStoreyStories();
		document.grids = smallGrid();
		// Y4（y=9000）は図の範囲の外なので寸法に入らない。
		document.grids.push_back(makeGrid("Y4", Vec2{-1000.0, 9000.0}, Vec2{2820.0, 9000.0}));
		document.columns = {makeColumn("1to2-柱", Vec2{0.0, 0.0}),
							makeColumn("1to2-柱", Vec2{0.0, 910.0}),
							makeColumn("1to2-柱", Vec2{0.0, 1820.0}),
							// 切断面の外。
							makeColumn("1to2-柱", Vec2{1820.0, 455.0})};
		document.members = {// 標準（2FL−36＝3264）の天端の梁。
							makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 1820.0}, 3264.0),
							// 標準より 100 低い梁。
							makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 910.0}, 3164.0),
							// 母屋は高さがもともと材ごとに違うので押さえない。
							makeMember("2-母屋", Vec2{0.0, 0.0}, Vec2{0.0, 1820.0}, 4500.0)};
		return document;
	}
} // namespace

TEST(SectionDimensionsCoverColumnsLevelsAndOffStandardBeams)
{
	const Document document = sectionDocument();
	const SectionCommand section = xSection();
	const std::vector<DimensionChainCommand> chains =
		parse::buildSectionDimensionCommands(document, section);

	// 注釈の横＝y − 5000。柱 0 / 910 / 1820 → −5000 / −4090 / −3180、通り芯 Y1・Y2 →
	// −5000 / −3180。
	const DimensionChainCommand* columns = findChain(chains, DimensionAxis::Horizontal, -1, 0);
	const DimensionChainCommand* grid = findChain(chains, DimensionAxis::Horizontal, -1, 1);
	CHECK(columns != nullptr);
	CHECK(grid != nullptr);
	if (columns != nullptr)
		CHECK(sameValues(columns->stops, {-5000.0, -4090.0, -3180.0}));
	if (grid != nullptr)
		CHECK(sameValues(grid->stops, {-5000.0, -3180.0}));
	// 下に出す列の根元は建物の下端（どの要素よりも下）。
	if (columns != nullptr)
		CHECK(columns->base <= 0.0);

	// 左: GL 0・1FL 500・2FL 3300・軒高 6000 に、横架材天端 464・3264 が加わる。
	const DimensionChainCommand* beams = findChain(chains, DimensionAxis::Vertical, -1, 0);
	const DimensionChainCommand* levels = findChain(chains, DimensionAxis::Vertical, -1, 1);
	CHECK(beams != nullptr);
	CHECK(levels != nullptr);
	if (beams != nullptr)
		CHECK(isChain(*beams, DimensionAxis::Vertical, {0.0, 464.0, 500.0, 3264.0, 3300.0, 6000.0},
					  -5000.0, -1, 0));
	if (levels != nullptr)
		CHECK(isChain(*levels, DimensionAxis::Vertical, {0.0, 500.0, 3300.0, 6000.0}, -5000.0, -1,
					  1));

	// 標準より低い梁 1 本だけ、その中央（(−5000 + −4090) / 2）で 3164〜3264 を押さえる。
	const DimensionChainCommand* offStandard = findChain(chains, DimensionAxis::Vertical, 1, 0);
	CHECK(offStandard != nullptr);
	if (offStandard != nullptr)
		CHECK(isChain(*offStandard, DimensionAxis::Vertical, {3164.0, 3264.0}, -4545.0, 1, 0));
	CHECK(chains.size() == 5);
}

TEST(SectionLevelMarksNameGlFloorsAndEaves)
{
	const std::vector<LevelMarkCommand> marks =
		parse::buildSectionLevelMarks(sectionDocument(), xSection());
	CHECK(marks.size() == 4);
	if (marks.size() != 4)
		return;
	const std::vector<std::string> names{"GL", "1FL", "2FL", "軒高"};
	const std::vector<double> heights{0.0, 500.0, 3300.0, 6000.0};
	for (std::size_t i = 0; i < marks.size(); ++i)
	{
		CHECK(marks[i].name == names[i]);
		CHECK(near(marks[i].elevation, heights[i]));
		// 図の左端（高さの寸法列の根元と同じ）。
		CHECK(near(marks[i].x, -5000.0));
	}
}

TEST(SectionWithNothingOnTheCutHasNoDimensions)
{
	Document document = sectionDocument();
	SectionCommand section = xSection();
	section.lineStart = Vec2{5000.0, -5000.0};
	section.lineEnd = Vec2{5000.0, 5000.0};
	CHECK(parse::buildSectionDimensionCommands(document, section).empty());
	CHECK(parse::buildSectionLevelMarks(document, section).empty());
}

TEST(AttachedDimensionsAreValidAndDeterministicOnRealFixtures)
{
	std::size_t planChains = 0;
	std::size_t sectionChains = 0;
	std::size_t foundationLocal = 0;
	forEachFixtureDocument(
		[&](const std::string&, const Document& original)
		{
			// 既定の設定（寸法なし）では 1 つも作らない。
			for (const SheetCommand& sheet : original.sheets)
				CHECK(sheet.viewport.dimensions.empty());
			for (const SectionCommand& section : original.sections)
			{
				CHECK(section.viewport.dimensions.empty());
				CHECK(section.levels.empty());
			}

			Document first = original;
			first.dimensionStyle = "寸法";
			parse::attachDimensionCommands(first);
			CHECK(core::validateDocument(first));

			// もう一度組み立てても同じ（決定性）。
			Document second = original;
			second.dimensionStyle = "寸法";
			parse::attachDimensionCommands(second);
			CHECK(first.sheets.size() == second.sheets.size());
			for (std::size_t i = 0; i < first.sheets.size() && i < second.sheets.size(); ++i)
			{
				const auto& a = first.sheets[i].viewport.dimensions;
				const auto& b = second.sheets[i].viewport.dimensions;
				CHECK(a.size() == b.size());
				for (std::size_t k = 0; k < a.size() && k < b.size(); ++k)
					CHECK(isChain(a[k], b[k].axis, b[k].stops, b[k].base, b[k].side, b[k].tier));
				planChains += a.size();
				if (first.sheets[i].kind == PlanKind::Foundation)
				{
					for (const DimensionChainCommand& chain : a)
						foundationLocal += chain.tier == 0 ? 1 : 0;
				}
			}
			for (std::size_t i = 0; i < first.sections.size() && i < second.sections.size(); ++i)
			{
				const auto& a = first.sections[i].viewport.dimensions;
				const auto& b = second.sections[i].viewport.dimensions;
				CHECK(a.size() == b.size());
				for (std::size_t k = 0; k < a.size() && k < b.size(); ++k)
					CHECK(isChain(a[k], b[k].axis, b[k].stops, b[k].base, b[k].side, b[k].tier));
				sectionChains += a.size();
				// 寸法のある軸組図にはレベル記号も載る。
				if (!a.empty())
					CHECK(!first.sections[i].levels.empty());
			}
		});
	// どのフィクスチャでも伏図・軸組図の両方に寸法が入り、基礎伏図には立上りに沿う列がある。
	CHECK(planChains > 0);
	CHECK(sectionChains > 0);
	CHECK(foundationLocal > 0);
}

TEST(BuildDocumentAddsDimensionsOnlyWhenAStyleIsChosen)
{
	const std::string path = HomeskzIfcTests::fixturePath(HomeskzIfcTests::allFixtures().front());
	core::NullProgressReporter progress;

	core::ImportOptions options;
	options.setDimensionStyle("JIS");
	const Document withStyle = parse::buildDocument(path, progress, options);
	CHECK(withStyle.dimensionStyle == "JIS");
	CHECK(core::validateDocument(withStyle));
	std::size_t chains = 0;
	for (const SheetCommand& sheet : withStyle.sheets)
		chains += sheet.viewport.dimensions.size();
	CHECK(chains > 0);

	const Document without = parse::buildDocument(path, progress, core::ImportOptions{});
	CHECK(without.dimensionStyle.empty());
	for (const SheetCommand& sheet : without.sheets)
		CHECK(sheet.viewport.dimensions.empty());
}

TEST_MAIN();
