//
//	ParseDimensionTests.cpp
//
//	寸法（src/parse/Dimension。docs/DEV-NOTES.md M31）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（通り芯 910 / 1820 の格子に部材を置いた小さな文書で手計算した値）。
//
//	検証項目:
//	  * 測点のまとめ方——昇順・許容以内は 1 点・部材と通り芯が重なれば通り芯の値が残る。
//	  * 伏図の外周の列——上と左に部材の位置（通り芯を合わせる）、全長は上（1 つ外の段）と右。
//	    通り芯の間隔だけの列は置かない。通り芯が無くても部材の位置と全長は出る。
//	  * 基礎伏図の立上りに沿う列——アンカーボルト・自由端・取り合う立上りの芯が測点になる。
//	    隅・T 字は立上りの芯で押さえ、取り合う立上りの無い通り芯は測点にしない。離れた
//	    立上りの間はまたがない（取り合いの無い一続きだけはつなぐ）。外側に面する立上り
//	    （段違いの外周を含む）は図の外に並べる。外周の列には外周の立上りに取り合う芯だけが載る。
//	    外側の列にある寸法は内部の列に重ねない。半島状の立上りは長さも押さえる。内部の
//	    立上りは芯・端の列とアンカーボルトの列を分ける。
//	  * 床伏図・小屋伏図の横架材に沿う列——外周には外周の梁に乗る柱と取り合う梁の芯だけを
//	    出し、それ以外の柱は内部の梁に沿って押さえる。柱と梁の芯が一致しない通りは、
//	    柱の列と横架材の列を別に押さえる。外周の柱の列は辺ごとに 1 本につなぎ、上と右は
//	    全長の端まで延ばす。外側の列にある寸法は内部の列に重ねない。
//	  * 伏図の種類ごとに押さえるもの——床伏図は柱と梁（表示レイヤに載るものだけ）、
//	    母屋伏図は母屋だけ。
//	  * 軸組図——柱の位置（通り芯を合わせる。図の下）、GL・FL・軒高と標準の横架材天端（図の左）、
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

#include <algorithm>
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

TEST(PerimeterChainsPutMembersTopLeftAndOverallTopRight)
{
	const Vec2 min{-300.0, -300.0};
	const Vec2 max{2120.0, 2120.0};
	const std::vector<DimensionChainCommand> chains = parse::perimeterDimensionChains(
		{0.0, 455.0, 910.0}, {}, {0.0, 910.0, 1820.0}, {0.0, 1820.0}, min, max, 0);

	CHECK(chains.size() == 4);
	if (chains.size() != 4)
		return;
	// 上: 部材の位置（通り芯を合わせる）→ 全長。通り芯の間隔だけの列は無く、下にも置かない。
	CHECK(isChain(chains[0], DimensionAxis::Horizontal, {0.0, 455.0, 910.0, 1820.0}, 2120.0, 1, 0));
	CHECK(isChain(chains[1], DimensionAxis::Horizontal, {0.0, 1820.0}, 2120.0, 1, 1));
	// 左: 部材の位置（ここでは通り芯だけ）。右: 全長（最も内側の段）。
	CHECK(isChain(chains[2], DimensionAxis::Vertical, {0.0, 1820.0}, -300.0, -1, 0));
	CHECK(isChain(chains[3], DimensionAxis::Vertical, {0.0, 1820.0}, 2120.0, 1, 0));
}

TEST(PerimeterChainsWorkWithoutGrids)
{
	const std::vector<DimensionChainCommand> chains = parse::perimeterDimensionChains(
		{0.0, 1000.0, 3000.0}, {}, {}, {}, Vec2{-300.0, -300.0}, Vec2{3300.0, 300.0}, 1);

	// 通り芯が無くても部材の位置と全長は出る。Y を測る部材が無ければ左右には何も無い。
	CHECK(chains.size() == 2);
	if (chains.size() != 2)
		return;
	CHECK(isChain(chains[0], DimensionAxis::Horizontal, {0.0, 1000.0, 3000.0}, 300.0, 1, 1));
	CHECK(isChain(chains[1], DimensionAxis::Horizontal, {0.0, 3000.0}, 300.0, 1, 2));
}

TEST(PerimeterChainsSkipTheTopOverallWhenItRepeatsTheMembers)
{
	// 測点が 2 つなら部材の位置の列がそのまま全長なので、上に 2 段重ねない（右には置く）。
	const std::vector<DimensionChainCommand> chains = parse::perimeterDimensionChains(
		{0.0}, {0.0}, {1820.0}, {1820.0}, Vec2{-300.0, -300.0}, Vec2{2120.0, 2120.0}, 0);
	CHECK(chains.size() == 3);
	if (chains.size() != 3)
		return;
	CHECK(isChain(chains[0], DimensionAxis::Horizontal, {0.0, 1820.0}, 2120.0, 1, 0));
	CHECK(isChain(chains[1], DimensionAxis::Vertical, {0.0, 1820.0}, -300.0, -1, 0));
	CHECK(isChain(chains[2], DimensionAxis::Vertical, {0.0, 1820.0}, 2120.0, 1, 0));
}

namespace
{
	WallCommand makeWall(Vec2 start, Vec2 end)
	{
		WallCommand w;
		w.layer = "F-立上り";
		w.drawClass = "立上り";
		w.start = start;
		w.end = end;
		w.thickness = 150.0;
		return w;
	}

	SymbolCommand makeBolt(Vec2 position)
	{
		SymbolCommand s;
		s.layer = "F-アンカーボルト";
		s.symbol = "アンカーボルト_M12";
		s.position = position;
		return s;
	}
} // namespace

namespace
{
	// 列のうち、指定の向き・根元・側・段・測点のものがあるか。
	bool hasChain(const std::vector<DimensionChainCommand>& chains, DimensionAxis axis,
				  const std::vector<double>& stops, double base, int side, int tier)
	{
		for (const DimensionChainCommand& chain : chains)
		{
			if (isChain(chain, axis, stops, base, side, tier))
				return true;
		}
		return false;
	}
} // namespace

TEST(FoundationChainsFollowEachWallLine)
{
	// Y1 の立上りが X2 と X3 の間で切れている（−75〜1820 と 2500〜3640）。X1 の立上りは
	// Y1 の立上りと隅で取り合い、2000 で終わる（取り合う立上りは無い）。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1820.0, 0.0}),
										 makeWall(Vec2{2500.0, 0.0}, Vec2{3640.0, 0.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 2000.0})};
	// 3 本目は芯から 10mm ずれても立上りの上（厚みの半分以内）。4 本目は立上りの外。
	const std::vector<SymbolCommand> bolts{makeBolt(Vec2{200.0, 0.0}), makeBolt(Vec2{1620.0, 0.0}),
										   makeBolt(Vec2{3000.0, 10.0}),
										   makeBolt(Vec2{1000.0, 500.0})};
	const std::vector<GridCommand> grids{
		makeGrid("X1", Vec2{0.0, -1000.0}, Vec2{0.0, 3000.0}),
		makeGrid("X2", Vec2{1820.0, -1000.0}, Vec2{1820.0, 3000.0}),
		makeGrid("X3", Vec2{3640.0, -1000.0}, Vec2{3640.0, 3000.0}),
		makeGrid("Y1", Vec2{-1000.0, 0.0}, Vec2{4640.0, 0.0}),
		makeGrid("Y2", Vec2{-1000.0, 1000.0}, Vec2{4640.0, 1000.0}),
	};

	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, bolts, grids, Vec2{-75.0, -75.0}, Vec2{3715.0, 2000.0});

	// Y1 は図の外側（下）に面するので、図の外形を根元に下へ出す。隅は外面（−75）ではなく
	// X1 の立上りの芯（0）から測る。切れ目（1820・2500）と自由端（3640）は端そのもの。
	// 2500〜3640 は何とも取り合わない（位置が決まらない）ので、左の一続きとつないだまま。
	CHECK(hasChain(chains, DimensionAxis::Horizontal,
				   {0.0, 200.0, 1620.0, 1820.0, 2500.0, 3000.0, 3640.0}, -75.0, -1, 0));
	// X1 は Y1 の立上りの芯から自由端（2000）まで。取り合う立上りの無い Y2（1000）は測点に
	// しない。左に面するので図の外形から左へ。
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 2000.0}, -75.0, -1, 0));
}

TEST(FoundationChainsMeasureTeeJunctionsAtTheCore)
{
	// 外周の立上り（Y1）に内部の立上り（x=910）が T 字で突き当たり（面で終わる）、
	// Y1 は x=1000 で 2 本に割れているが同じ通りへ続くので切れ目にしない。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1000.0, 0.0}),
										 makeWall(Vec2{1000.0, 0.0}, Vec2{1895.0, 0.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1820.0}),
										 makeWall(Vec2{910.0, 75.0}, Vec2{910.0, 910.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 1820.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, {}, smallGrid(), Vec2{-75.0, -75.0}, Vec2{1895.0, 1820.0});
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 910.0, 1820.0}, -75.0, -1, 0));
}

TEST(FoundationChainsSplitWhereAWallCrossesTheGap)
{
	// 外周 0〜1820 の矩形の中に、南北の立上り x=910 が通り、y=910 の立上りが 2 本（西の
	// 立上りに取り合う 0〜500 と、東の立上りに取り合う 1320〜1820）。間を x=910 が横切るので
	// 割る（間の 820 は測らない）。y=1365 は 3 本（0〜300・600〜700・1200〜1820）で、途切れを
	// それぞれ x=450・x=910 が横切る。600〜700 は何とも取り合わないので、近いほうの隣
	// （西の 0〜300。間 300 ＜ 500）とつないで間の 300 で位置を押さえる。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1895.0, 0.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 1895.0}),
										 makeWall(Vec2{910.0, 75.0}, Vec2{910.0, 1745.0}),
										 makeWall(Vec2{75.0, 910.0}, Vec2{500.0, 910.0}),
										 makeWall(Vec2{1320.0, 910.0}, Vec2{1745.0, 910.0}),
										 makeWall(Vec2{450.0, 1200.0}, Vec2{450.0, 1500.0}),
										 makeWall(Vec2{75.0, 1365.0}, Vec2{300.0, 1365.0}),
										 makeWall(Vec2{600.0, 1365.0}, Vec2{700.0, 1365.0}),
										 makeWall(Vec2{1200.0, 1365.0}, Vec2{1745.0, 1365.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, {}, smallGrid(), Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});

	// 内部の立上りなので、その芯から図の中心（910）の外側へ出す。
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 500.0}, 910.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {1320.0, 1820.0}, 910.0, 1, 0));
	CHECK(!hasChain(chains, DimensionAxis::Horizontal, {0.0, 500.0, 1320.0, 1820.0}, 910.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 300.0, 600.0, 700.0}, 1365.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {1200.0, 1820.0}, 1365.0, 1, 0));
}

TEST(FoundationChainsKeepOpeningsInTheChain)
{
	// 上と同じ矩形で x=910 の立上りが無い。y=910 の途切れ（500〜1320）を横切る立上りが
	// 無いので開口とみなし、開口の幅（820）も押さえる。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1895.0, 0.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 1895.0}),
										 makeWall(Vec2{75.0, 910.0}, Vec2{500.0, 910.0}),
										 makeWall(Vec2{1320.0, 910.0}, Vec2{1745.0, 910.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, {}, smallGrid(), Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 500.0, 1320.0, 1820.0}, 910.0, 1, 0));
}

TEST(FoundationChainsSplitGapsBetweenCorners)
{
	// 下の外周に切り欠き: y=0 は 0〜910 と 1820〜2730 で、間の 910〜1820 は y=910 まで
	// 引っ込む。y=0 の途切れ（985〜1745）は両側が x=910・x=1820 の隅なので開口ではなく、
	// 割る。3 本とも下の外側に面して外周に並び、2 段目に芯の列 0/910/1820/2730 が出る。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{985.0, 0.0}),
										 makeWall(Vec2{1745.0, 0.0}, Vec2{2805.0, 0.0}),
										 makeWall(Vec2{835.0, 910.0}, Vec2{1895.0, 910.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{2805.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{910.0, -75.0}, Vec2{910.0, 985.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 985.0}),
										 makeWall(Vec2{2730.0, -75.0}, Vec2{2730.0, 1895.0})};
	const std::vector<SymbolCommand> bolts{makeBolt(Vec2{200.0, 0.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, bolts, {}, Vec2{-75.0, -75.0}, Vec2{2805.0, 1895.0});

	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 200.0, 910.0}, -75.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {910.0, 1820.0}, -75.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {1820.0, 2730.0}, -75.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 910.0, 1820.0, 2730.0}, -75.0, -1, 1));
}

TEST(FoundationChainsSkipSegmentsTheOuterChainsAlreadyShow)
{
	// 矩形の中を x=910 が通り、内部の y=600 が 0〜910（両端とも取り合い）に、y=1300 が
	// 0〜910 にアンカーボルト 1 本（x=455）で渡る。y=600 の 0〜910 は外周の芯の列に
	// あるので書かない。y=1300 は 0〜455〜910 で重ならないので書く。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1895.0, 0.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 1895.0}),
										 makeWall(Vec2{910.0, 75.0}, Vec2{910.0, 1745.0}),
										 makeWall(Vec2{75.0, 600.0}, Vec2{835.0, 600.0}),
										 makeWall(Vec2{75.0, 1300.0}, Vec2{835.0, 1300.0})};
	const std::vector<SymbolCommand> bolts{makeBolt(Vec2{455.0, 1300.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, bolts, {}, Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});

	for (const DimensionChainCommand& chain : chains)
		CHECK(!(chain.axis == DimensionAxis::Horizontal && near(chain.base, 600.0)));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 455.0, 910.0}, 1300.0, 1, 0));
}

TEST(FoundationChainsSeparateWallCoresFromAnchorBolts)
{
	// 矩形の中を x=910 が通り、y=600 の半島状の立上り（910〜1400）が取り合う。x=910 には
	// アンカーボルトが 1 本（y=300）。立上りの芯・端の列 0/600/1820 を 2 段目に、1 段目には
	// アンカーボルトの絡む 0/300/600 だけを出す（600〜1820 は 2 段目にある）。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1895.0, 0.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 1895.0}),
										 makeWall(Vec2{910.0, 75.0}, Vec2{910.0, 1745.0}),
										 makeWall(Vec2{985.0, 600.0}, Vec2{1400.0, 600.0})};
	const std::vector<SymbolCommand> bolts{makeBolt(Vec2{910.0, 300.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, bolts, {}, Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});

	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 600.0, 1820.0}, 910.0, 1, 1));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 300.0, 600.0}, 910.0, 1, 0));
	CHECK(!hasChain(chains, DimensionAxis::Vertical, {0.0, 300.0, 600.0, 1820.0}, 910.0, 1, 0));
}

TEST(FoundationChainsGivePeninsulasTheirLength)
{
	// 西の外周から突き出た半島状の立上り（y=910 の 0〜970。先は自由端）にアンカーボルトが
	// 2 本。1 段目に 0/200/710/970、その外に長さ 970。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1895.0, 0.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 1895.0}),
										 makeWall(Vec2{75.0, 910.0}, Vec2{970.0, 910.0})};
	const std::vector<SymbolCommand> bolts{makeBolt(Vec2{200.0, 910.0}),
										   makeBolt(Vec2{710.0, 910.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, bolts, {}, Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});

	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 200.0, 710.0, 970.0}, 910.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 970.0}, 910.0, 1, 1));
}

TEST(FoundationChainsPutSteppedExteriorWallsOutside)
{
	// 下が段違いの外周: 西半分（0〜910）は y=0、東半分（910〜1820）は y=910 が外周。
	// どちらも下の外側に面するので、図の外形（y=−75）を根元に同じ段へ並べる。段の
	// 立上り（x=910 の 0〜910）は右の外側に面するので、右の外形から出す。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{985.0, 0.0}),
										 makeWall(Vec2{835.0, 910.0}, Vec2{1895.0, 910.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{910.0, -75.0}, Vec2{910.0, 985.0}),
										 makeWall(Vec2{1820.0, 835.0}, Vec2{1820.0, 1895.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, {}, smallGrid(), Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});

	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 910.0}, -75.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {910.0, 1820.0}, -75.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 910.0}, 1895.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {910.0, 1820.0}, 1895.0, 1, 0));
	// 上・左は外周の 1 本の通りなので 1 段目だけ（芯の列は同じなので重ねない）。全長は
	// 上に 1820（1 段目と同じなので出さない）、右に 1820。
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 1820.0}, 1895.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 1820.0}, -75.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 1820.0}, 1895.0, 1, 1));
	CHECK(!hasChain(chains, DimensionAxis::Horizontal, {0.0, 1820.0}, 1895.0, 1, 1));
}

TEST(FoundationPerimeterCarriesOnlyWhatMeetsThePerimeter)
{
	// 外周 0〜1820 の矩形。北の立上りには内部の立上り（x=910）が取り合い、アンカーボルトが
	// 1 本（x=300）乗る。内部の東西の立上り（y=600）は西の立上りと取り合わない
	// （x=910 の立上りにだけ取り合う）ので、左の芯の列に載らない。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{1895.0, 0.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{1820.0, -75.0}, Vec2{1820.0, 1895.0}),
										 makeWall(Vec2{910.0, 600.0}, Vec2{910.0, 1745.0}),
										 makeWall(Vec2{910.0, 600.0}, Vec2{1400.0, 600.0})};
	const std::vector<SymbolCommand> bolts{makeBolt(Vec2{300.0, 1820.0}),
										   makeBolt(Vec2{0.0, 400.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, bolts, smallGrid(), Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});

	CHECK(chains.size() >= 3);
	if (chains.size() < 3)
		return;
	// 上: 芯の列（アンカーボルトを除く）→ 全長。
	CHECK(isChain(chains[0], DimensionAxis::Horizontal, {0.0, 910.0, 1820.0}, 1895.0, 1, 1));
	CHECK(isChain(chains[1], DimensionAxis::Horizontal, {0.0, 1820.0}, 1895.0, 1, 2));
	// 左: 芯の列（y=600 は入らない）。
	CHECK(isChain(chains[2], DimensionAxis::Vertical, {0.0, 1820.0}, -75.0, -1, 1));
	// 下・右は 1 段目（隅から隅）と同じなので芯の列も全長も重ねない。
	CHECK(!hasChain(chains, DimensionAxis::Horizontal, {0.0, 1820.0}, -75.0, -1, 1));
	CHECK(!hasChain(chains, DimensionAxis::Vertical, {0.0, 1820.0}, 1895.0, 1, 1));
}

TEST(FoundationPerimeterHasCoresOnEverySide)
{
	// 下が段違いの外周（西は y=0、東は y=910）で、y=0 に内部の立上り x=455 が、東の外周
	// x=1820 に内部の立上り y=1365 が取り合う。下の 2 段目は 0 / 455 / 910 / 1820、
	// 右の 2 段目は 910 / 1365 / 1820 で、右の全長（0〜1820）はその外。
	const std::vector<WallCommand> walls{makeWall(Vec2{-75.0, 0.0}, Vec2{985.0, 0.0}),
										 makeWall(Vec2{835.0, 910.0}, Vec2{1895.0, 910.0}),
										 makeWall(Vec2{-75.0, 1820.0}, Vec2{1895.0, 1820.0}),
										 makeWall(Vec2{0.0, -75.0}, Vec2{0.0, 1895.0}),
										 makeWall(Vec2{910.0, -75.0}, Vec2{910.0, 985.0}),
										 makeWall(Vec2{1820.0, 835.0}, Vec2{1820.0, 1895.0}),
										 makeWall(Vec2{455.0, 75.0}, Vec2{455.0, 600.0}),
										 makeWall(Vec2{1200.0, 1365.0}, Vec2{1745.0, 1365.0})};
	const std::vector<SymbolCommand> bolts{makeBolt(Vec2{200.0, 0.0}),
										   makeBolt(Vec2{1820.0, 1100.0})};
	const std::vector<DimensionChainCommand> chains = parse::foundationDimensionChains(
		walls, bolts, smallGrid(), Vec2{-75.0, -75.0}, Vec2{1895.0, 1895.0});

	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 455.0, 910.0, 1820.0}, -75.0, -1, 1));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 910.0, 1365.0, 1820.0}, 1895.0, 1, 1));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 1820.0}, 1895.0, 1, 2));
	// 下に全長は置かない。
	CHECK(!hasChain(chains, DimensionAxis::Horizontal, {0.0, 1820.0}, -75.0, -1, 2));
}

TEST(FramingChainsPutOnlyWhatFacesThePerimeterOutside)
{
	// 外周 0〜1820 の矩形の梁（端点は取り付く相手の芯）。内部に南北の梁 x=910（上下の外周に
	// 取り合う）と、東西の梁 y=910（西の外周と x=910 に取り合う）。柱は四隅・南の外周の
	// x=455・内部の梁の上の (455, 910) と (910, 1365)。
	const std::vector<MemberCommand> members{
		makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{1820.0, 0.0}, 3264.0),
		makeMember("2-横架材天端", Vec2{0.0, 1820.0}, Vec2{1820.0, 1820.0}, 3264.0),
		makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 1820.0}, 3264.0),
		makeMember("2-横架材天端", Vec2{1820.0, 0.0}, Vec2{1820.0, 1820.0}, 3264.0),
		makeMember("2-横架材天端", Vec2{910.0, 0.0}, Vec2{910.0, 1820.0}, 3264.0),
		makeMember("2-横架材天端", Vec2{0.0, 910.0}, Vec2{910.0, 910.0}, 3264.0)};
	const std::vector<ColumnCommand> columns{
		makeColumn("1to2-柱", Vec2{0.0, 0.0}),	   makeColumn("1to2-柱", Vec2{1820.0, 0.0}),
		makeColumn("1to2-柱", Vec2{0.0, 1820.0}),  makeColumn("1to2-柱", Vec2{1820.0, 1820.0}),
		makeColumn("1to2-柱", Vec2{455.0, 0.0}),   makeColumn("1to2-柱", Vec2{455.0, 910.0}),
		makeColumn("1to2-柱", Vec2{910.0, 1365.0})};
	const std::vector<DimensionChainCommand> chains = parse::framingDimensionChains(
		members, columns, smallGrid(), Vec2{-60.0, -60.0}, Vec2{1880.0, 1880.0});

	// 外周には外周の梁に乗る柱と取り合う梁の芯だけ。内部の柱（455, 910）の x=455 は
	// 上には載らない。上・左・右は柱が取り合いの上にあるので 1 列。
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 910.0, 1820.0}, 1880.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 1820.0}, 1880.0, 1, 1));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 910.0, 1820.0}, -60.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 1820.0}, 1880.0, 1, 0));
	CHECK(!hasChain(chains, DimensionAxis::Vertical, {0.0, 1820.0}, 1880.0, 1, 1));
	// 下は柱（455）が取り合いから外れ、取り合い（910）が柱から外れるが、横架材の列
	// （0 / 910 / 1820）は上の柱の列と同じ寸法なので重ねず、柱の列だけが 1 段目に残る。
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 455.0, 1820.0}, -60.0, -1, 0));
	CHECK(!hasChain(chains, DimensionAxis::Horizontal, {0.0, 910.0, 1820.0}, -60.0, -1, 0));

	// 外周に載らない柱は、乗っている内部の梁に沿って押さえる（梁の芯から外側へ）。
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 455.0, 910.0}, 910.0, 1, 0));
	// x=910 の横架材の列（0 / 910 / 1820）は左の外周の列と同じ寸法なので重ねず、柱の
	// 列だけが 1 段目に残る。
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 1365.0, 1820.0}, 910.0, 1, 0));
	for (const DimensionChainCommand& chain : chains)
		CHECK(
			!(chain.axis == DimensionAxis::Vertical && near(chain.base, 910.0) && chain.tier != 0));
}

TEST(FramingChainsMeasureBeamsApartFromColumnsAndJoinThePerimeterRow)
{
	// 外周 0〜1820 の矩形で、東の外周の梁は下半分（0〜910。先は自由端）だけ。y=650 の
	// 大引が東西に渡り、y=910 の短い梁が西の外周に取り合う。西の柱は 0 / 1200 / 1820
	// （1200 には梁が取り合わない）、東の柱は 0 / 455。
	const std::vector<MemberCommand> members{
		makeMember("1-横架材天端", Vec2{0.0, 0.0}, Vec2{1820.0, 0.0}, 464.0),
		makeMember("1-横架材天端", Vec2{0.0, 1820.0}, Vec2{1820.0, 1820.0}, 464.0),
		makeMember("1-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 1820.0}, 464.0),
		makeMember("1-横架材天端", Vec2{1820.0, 0.0}, Vec2{1820.0, 910.0}, 464.0),
		makeMember("1-横架材天端", Vec2{0.0, 650.0}, Vec2{1820.0, 650.0}, 464.0),
		makeMember("1-横架材天端", Vec2{0.0, 910.0}, Vec2{300.0, 910.0}, 464.0)};
	const std::vector<ColumnCommand> columns{
		makeColumn("1to2-柱", Vec2{0.0, 0.0}),	  makeColumn("1to2-柱", Vec2{1820.0, 0.0}),
		makeColumn("1to2-柱", Vec2{0.0, 1200.0}), makeColumn("1to2-柱", Vec2{1820.0, 455.0}),
		makeColumn("1to2-柱", Vec2{0.0, 1820.0}), makeColumn("1to2-柱", Vec2{1820.0, 1820.0})};
	const std::vector<DimensionChainCommand> chains = parse::framingDimensionChains(
		members, columns, {}, Vec2{-60.0, -60.0}, Vec2{1880.0, 1880.0});

	// 西: 柱の間隔（1200 / 620）と、柱とは別に横架材の取り合い（650 / 260）。910〜1820 は
	// 東の柱の列（全長の端まで延ばした区間）と同じ寸法なので重ねない。
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 1200.0, 1820.0}, -60.0, -1, 1));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 650.0, 910.0}, -60.0, -1, 0));
	CHECK(!hasChain(chains, DimensionAxis::Vertical, {0.0, 650.0, 910.0, 1820.0}, -60.0, -1, 0));
	// 東: 柱の列は外周の梁の無い 910〜1820 も全長の端まで延ばして 1 本につなぐ。横架材の
	// 取り合い（0 / 650 / 910）は西の列と同じ寸法なので重ねず、柱の列が 1 段目。
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 455.0, 910.0, 1820.0}, 1880.0, 1, 0));
	CHECK(hasChain(chains, DimensionAxis::Vertical, {0.0, 1820.0}, 1880.0, 1, 1));
	CHECK(!hasChain(chains, DimensionAxis::Vertical, {0.0, 650.0, 910.0}, 1880.0, 1, 0));
	// y=650 の大引（0〜1820）は上の外周の列と同じ寸法なので内部には書かない。
	for (const DimensionChainCommand& chain : chains)
		CHECK(!(chain.axis == DimensionAxis::Horizontal && near(chain.base, 650.0)));
}

TEST(FramingChainsKeepTheOverallOnASideWithoutAnExteriorLine)
{
	// 上が開いた U 字の架構。東西の梁は下（y=0）だけなので下に面し、上に面する通りは
	// 無いが、上の全長（0〜1820）は出す。
	const std::vector<MemberCommand> members{
		makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{1820.0, 0.0}, 3264.0),
		makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 1820.0}, 3264.0),
		makeMember("2-横架材天端", Vec2{1820.0, 0.0}, Vec2{1820.0, 1820.0}, 3264.0)};
	const std::vector<DimensionChainCommand> chains =
		parse::framingDimensionChains(members, {}, {}, Vec2{-60.0, -60.0}, Vec2{1880.0, 1880.0});

	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 1820.0}, -60.0, -1, 0));
	CHECK(hasChain(chains, DimensionAxis::Horizontal, {0.0, 1820.0}, 1880.0, 1, 0));
}

TEST(FramingPlanDimensionsColumnsAndBeamsOnItsLayers)
{
	Document document;
	document.stories = twoStoreyStories();
	document.grids = smallGrid();
	document.columns = {makeColumn("1to2-柱", Vec2{0.0, 0.0}),
						makeColumn("1to2-柱", Vec2{455.0, 1820.0}),
						// 別の階の柱は映らないので押さえない。
						makeColumn("2to3-柱", Vec2{1365.0, 1820.0})};
	document.members = {makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{1820.0, 0.0}, 3264.0),
						makeMember("2-横架材天端", Vec2{0.0, 1820.0}, Vec2{1820.0, 1820.0}, 3264.0),
						makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 1820.0}, 3264.0),
						makeMember("2-横架材天端", Vec2{1820.0, 0.0}, Vec2{1820.0, 1820.0}, 3264.0),
						// 斜めの材は押さえない。
						makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{910.0, 700.0}, 3264.0),
						// 表示レイヤに無い材は押さえない。
						makeMember("1-横架材天端", Vec2{1500.0, 0.0}, Vec2{1500.0, 1820.0}, 464.0)};
	const SheetCommand sheet = makeSheet(PlanKind::Framing, {"2-横架材天端", "1to2-柱"});

	const std::vector<DimensionChainCommand> chains =
		parse::buildPlanDimensionCommands(document, sheet);

	// 上の 1 段目: 外周の梁の両端に柱の 455（1365・1500・通り芯だけの 910 は入らない）。
	const DimensionChainCommand* top = findChain(chains, DimensionAxis::Horizontal, 1, 0);
	CHECK(top != nullptr);
	if (top != nullptr)
		CHECK(sameValues(top->stops, {0.0, 455.0, 1820.0}));
	// 左の 1 段目: 外周の梁の両端だけ。
	const DimensionChainCommand* left = findChain(chains, DimensionAxis::Vertical, -1, 0);
	CHECK(left != nullptr);
	if (left != nullptr)
		CHECK(sameValues(left->stops, {0.0, 1820.0}));
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

	// 注釈の横＝y − 5000。柱 0 / 910 / 1820 → −5000 / −4090 / −3180（通り芯 Y1・Y2 の
	// −5000 / −3180 と重なる）。通り芯の間隔だけの列は置かない。
	const DimensionChainCommand* columns = findChain(chains, DimensionAxis::Horizontal, -1, 0);
	CHECK(columns != nullptr);
	if (columns != nullptr)
		CHECK(sameValues(columns->stops, {-5000.0, -4090.0, -3180.0}));
	CHECK(findChain(chains, DimensionAxis::Horizontal, -1, 1) == nullptr);
	// 下に出す列の根元は建物の下端（どの要素よりも下）。
	if (columns != nullptr)
		CHECK(columns->base <= 0.0);

	// 左の 1 段目: 各階の FL から横架材天端まで（1FL 500〜464・2FL 3300〜3264）を 1 本ずつ。
	// GL〜土台天端・FL〜上階の横架材天端のような基準をまたぐ寸法は出さない。
	std::vector<const DimensionChainCommand*> drops;
	for (const DimensionChainCommand& chain : chains)
	{
		if (chain.axis == DimensionAxis::Vertical && chain.side == -1 && chain.tier == 0)
			drops.push_back(&chain);
	}
	CHECK(drops.size() == 2);
	if (drops.size() == 2)
	{
		CHECK(isChain(*drops[0], DimensionAxis::Vertical, {464.0, 500.0}, -5000.0, -1, 0));
		CHECK(isChain(*drops[1], DimensionAxis::Vertical, {3264.0, 3300.0}, -5000.0, -1, 0));
	}
	// 左の 2 段目: GL 0・1FL 500・2FL 3300・軒高 6000 の間隔。
	const DimensionChainCommand* levels = findChain(chains, DimensionAxis::Vertical, -1, 1);
	CHECK(levels != nullptr);
	if (levels != nullptr)
		CHECK(isChain(*levels, DimensionAxis::Vertical, {0.0, 500.0, 3300.0, 6000.0}, -5000.0, -1,
					  1));

	// 標準より低い梁 1 本だけ、その中央（(−5000 + −4090) / 2）で 3164〜3264 を押さえる。
	const DimensionChainCommand* offStandard = findChain(chains, DimensionAxis::Vertical, 1, 0);
	CHECK(offStandard != nullptr);
	if (offStandard != nullptr)
		CHECK(isChain(*offStandard, DimensionAxis::Vertical, {3164.0, 3264.0}, -4545.0, 1, 0));
	// 上階の柱も小屋束も無いので、図の上には何も出さない。
	CHECK(findChain(chains, DimensionAxis::Horizontal, 1, 0) == nullptr);
	CHECK(chains.size() == 5);
}

TEST(SectionDimensionsMergeOffStandardBeamsOfTheSameHeight)
{
	// 標準より 100 低い梁が 2 本（0〜910 と 910〜1820）。寸法は 3164〜3264 の 1 本だけに
	// まとまる。並びを変えても同じ結果になる。
	Document document = sectionDocument();
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{0.0, 910.0}, Vec2{0.0, 1820.0}, 3164.0));
	// 別の高さ（標準より 200 低い）は別の寸法になる。
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{0.0, 910.0}, Vec2{0.0, 1820.0}, 3064.0));
	const SectionCommand section = xSection();
	const std::vector<DimensionChainCommand> chains =
		parse::buildSectionDimensionCommands(document, section);

	std::vector<const DimensionChainCommand*> offStandard;
	for (const DimensionChainCommand& chain : chains)
	{
		if (chain.axis == DimensionAxis::Vertical && chain.side == 1)
			offStandard.push_back(&chain);
	}
	CHECK(offStandard.size() == 2);
	if (offStandard.size() == 2)
	{
		CHECK(isChain(*offStandard[0], DimensionAxis::Vertical, {3064.0, 3264.0}, -3635.0, 1, 0));
		// 同じ長さの材が 2 本なら位置の小さい方（0〜910 の中央 −4545）。
		CHECK(isChain(*offStandard[1], DimensionAxis::Vertical, {3164.0, 3264.0}, -4545.0, 1, 0));
	}

	// 並びを逆にしても同じ。
	Document reversed = document;
	std::ranges::reverse(reversed.members);
	const std::vector<DimensionChainCommand> again =
		parse::buildSectionDimensionCommands(reversed, section);
	CHECK(again.size() == chains.size());
	for (std::size_t i = 0; i < again.size() && i < chains.size(); ++i)
		CHECK(isChain(again[i], chains[i].axis, chains[i].stops, chains[i].base, chains[i].side,
					  chains[i].tier));
}

TEST(SectionDimensionsPlaceMergedBeamHeightOnTheLongestMember)
{
	// 同じ高さの材（0〜910 と 0〜1820）のうち、最も長い材の中央に置く。
	Document document = sectionDocument();
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 1820.0}, 3164.0));
	const std::vector<DimensionChainCommand> chains =
		parse::buildSectionDimensionCommands(document, xSection());
	const DimensionChainCommand* offStandard = findChain(chains, DimensionAxis::Vertical, 1, 0);
	CHECK(offStandard != nullptr);
	if (offStandard != nullptr)
		CHECK(isChain(*offStandard, DimensionAxis::Vertical, {3164.0, 3264.0}, -4090.0, 1, 0));
	CHECK(chains.size() == 5);
}

TEST(SectionDimensionsPutUpperColumnsAndKoyazukaAboveTheDrawing)
{
	// 1 階の柱 0 / 910 / 1820 に対し、2 階の柱は 0 / 455 / 1820（455 が 1 階と合わない）。
	// 通し柱（1to3）は 1 階・2 階の両方に数える。小屋束は 2 本（600・1820）。
	Document document = sectionDocument();
	document.columns.push_back(makeColumn("2to3-柱", Vec2{0.0, 0.0}));
	document.columns.push_back(makeColumn("2to3-柱", Vec2{0.0, 455.0}));
	document.columns.push_back(makeColumn("1to3-柱", Vec2{0.0, 1820.0}));
	ColumnCommand koyazuka = makeColumn("2to2.5-柱", Vec2{0.0, 600.0});
	koyazuka.structuralUse = core::kStructuralUseKoyazuka;
	document.columns.push_back(koyazuka);
	koyazuka.position = Vec2{0.0, 1820.0};
	document.columns.push_back(koyazuka);
	// 部材の無い通り芯（y=1365）は測点にしない。
	document.grids.push_back(makeGrid("Y1.5", Vec2{-1000.0, 1365.0}, Vec2{2820.0, 1365.0}));
	const std::vector<DimensionChainCommand> chains =
		parse::buildSectionDimensionCommands(document, xSection());

	// 下: 1 階の柱だけ（2 階の 455・小屋束・通り芯 1365 は入らない）。
	const DimensionChainCommand* bottom = findChain(chains, DimensionAxis::Horizontal, -1, 0);
	CHECK(bottom != nullptr);
	if (bottom != nullptr)
		CHECK(sameValues(bottom->stops, {-5000.0, -4090.0, -3180.0}));

	double start = 0.0;
	double end = 0.0;
	CHECK(core::sectionHeightRange(document, start, end));
	const double top = end - core::kSectionHeightMargin;
	// 上の 1 段目: 2 階の柱すべて（1 階と重なる 0・1820 も含む）。2 段目: 小屋束に、
	// 小屋束が立つ 2 階の外壁芯（2 階の柱の両端 0・1820）を足したもの。
	const DimensionChainCommand* upper = findChain(chains, DimensionAxis::Horizontal, 1, 0);
	CHECK(upper != nullptr);
	if (upper != nullptr)
		CHECK(isChain(*upper, DimensionAxis::Horizontal, {-5000.0, -4545.0, -3180.0}, top, 1, 0));
	const DimensionChainCommand* posts = findChain(chains, DimensionAxis::Horizontal, 1, 1);
	CHECK(posts != nullptr);
	if (posts != nullptr)
		CHECK(isChain(*posts, DimensionAxis::Horizontal, {-5000.0, -4400.0, -3180.0}, top, 1, 1));
}

TEST(SectionDimensionsSkipUpperColumnsThatMatchTheFloorBelow)
{
	// 2 階の柱がすべて 1 階の柱の上にあれば、図の上に 2 階の列は出さない。小屋束は 1 本
	// でも外壁芯（2 階の柱の両端）から押さえる。
	Document document = sectionDocument();
	document.columns.push_back(makeColumn("2to3-柱", Vec2{0.0, 0.0}));
	document.columns.push_back(makeColumn("2to3-柱", Vec2{0.0, 1820.0}));
	ColumnCommand koyazuka = makeColumn("2to2.5-柱", Vec2{0.0, 600.0});
	koyazuka.structuralUse = core::kStructuralUseKoyazuka;
	document.columns.push_back(koyazuka);
	const std::vector<DimensionChainCommand> chains =
		parse::buildSectionDimensionCommands(document, xSection());
	const DimensionChainCommand* posts = findChain(chains, DimensionAxis::Horizontal, 1, 0);
	CHECK(posts != nullptr);
	if (posts != nullptr)
		CHECK(sameValues(posts->stops, {-5000.0, -4400.0, -3180.0}));
	CHECK(findChain(chains, DimensionAxis::Horizontal, 1, 1) == nullptr);
	CHECK(chains.size() == 6);
}

TEST(SectionDimensionsIncludeWhereAlongBeamsMeetCrossingBeams)
{
	// 1 階の土台（1-横架材天端）が y=0〜1365 に沿い、y=1365 で直交する 1 階の土台に
	// ぶつかる。柱の無い 1365 も下の列の測点になる。y=1365〜1600 の土台は y=1600 で
	// **2 階の**直交材の下を通るだけなので、1600 は数えない（階が違う）。沿う材の端に
	// 無い切り口（y=700 を横切る 1 階の材）も数えない。
	Document document = sectionDocument();
	document.members.push_back(
		makeMember("1-横架材天端", Vec2{0.0, 0.0}, Vec2{0.0, 1365.0}, 464.0));
	document.members.push_back(
		makeMember("1-横架材天端", Vec2{0.0, 1365.0}, Vec2{0.0, 1600.0}, 464.0));
	document.members.push_back(
		makeMember("1-横架材天端", Vec2{-910.0, 1365.0}, Vec2{910.0, 1365.0}, 464.0));
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{-910.0, 1600.0}, Vec2{910.0, 1600.0}, 3264.0));
	document.members.push_back(
		makeMember("1-横架材天端", Vec2{-910.0, 700.0}, Vec2{910.0, 700.0}, 464.0));
	const std::vector<DimensionChainCommand> chains =
		parse::buildSectionDimensionCommands(document, xSection());
	const DimensionChainCommand* bottom = findChain(chains, DimensionAxis::Horizontal, -1, 0);
	CHECK(bottom != nullptr);
	if (bottom != nullptr)
		CHECK(sameValues(bottom->stops, {-5000.0, -4090.0, -3635.0, -3180.0}));
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
	// 結ぶストーリレベル（高さの基準をそこへ拘束する）。
	const std::vector<std::string> stories{"F", "1", "2", "R"};
	const std::vector<std::string> levelTypes{core::kLevelGL, core::kLevelFL, core::kLevelFL,
											  core::kLevelEaves};
	for (std::size_t i = 0; i < marks.size(); ++i)
	{
		CHECK(marks[i].name == names[i]);
		CHECK(near(marks[i].elevation, heights[i]));
		CHECK(marks[i].story == stories[i]);
		CHECK(marks[i].levelType == levelTypes[i]);
		// 図の左端（高さの寸法列の根元と同じ）と右端（柱 1820 → −3180）。
		CHECK(near(marks[i].x, -5000.0));
		CHECK(near(marks[i].right, -3180.0));
		// 左の高さの列は 2 段（FL〜横架材天端・GL/FL/軒高の間隔）なので最も外は段 1。
		CHECK(marks[i].dimensionTier == 1);
	}
}

TEST(SectionDimensionsAndLevelMarksReachCrossingMembers)
{
	// 切断面を横切る横架材（東西に走る＝X通りの断面に切り口が写る）が、柱・沿う材より
	// 外（y=−500 → 注釈の横 −5500）にある。レベル記号と高さの列はその切り口（材幅 105 の
	// 半分だけ外）より外から出す。
	Document document = sectionDocument();
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{-910.0, -500.0}, Vec2{910.0, -500.0}, 3264.0));
	// 切断面で止まる材（x=0 で終わる）も切り口を持つ。右端（y=2500 → −2500）を広げる。
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{-910.0, 2500.0}, Vec2{0.0, 2500.0}, 3264.0));
	// 切断面へ届かない材は数えない。
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{500.0, -2000.0}, Vec2{1500.0, -2000.0}, 3264.0));
	// 柱の範囲の内側の切り口は、下の列へ足さない（押さえるのは最外周だけ）。
	document.members.push_back(
		makeMember("2-横架材天端", Vec2{-910.0, 455.0}, Vec2{910.0, 455.0}, 3264.0));
	// 外側の切り口の近く（20mm）に通り芯がある。下の列はその通り芯の値を採る。
	document.grids.push_back(makeGrid("Y0", Vec2{-1000.0, -480.0}, Vec2{2820.0, -480.0}));
	const SectionCommand section = xSection();

	const std::vector<LevelMarkCommand> marks = parse::buildSectionLevelMarks(document, section);
	CHECK(marks.size() == 4);
	for (const LevelMarkCommand& mark : marks)
	{
		CHECK(near(mark.x, -5552.5));
		CHECK(near(mark.right, -2447.5));
	}

	const std::vector<DimensionChainCommand> chains =
		parse::buildSectionDimensionCommands(document, section);
	const DimensionChainCommand* levels = findChain(chains, DimensionAxis::Vertical, -1, 1);
	CHECK(levels != nullptr);
	if (levels != nullptr)
		CHECK(near(levels->base, -5552.5));
	// 下の列は柱・束の位置に、その面の最外周の切り口（左は通り芯 Y0 の −5480、右は材の芯の
	// −2500）を足す。内側の切り口（−4545）は足さない。
	const DimensionChainCommand* columns = findChain(chains, DimensionAxis::Horizontal, -1, 0);
	CHECK(columns != nullptr);
	if (columns != nullptr)
		CHECK(sameValues(columns->stops, {-5480.0, -5000.0, -4090.0, -3180.0, -2500.0}));
}

TEST(SectionWithOnlyCrossingMembersHasNoDimensions)
{
	// 切り口だけでは記号を置く根拠にしない（柱も沿う材も無い断面は従来どおり空）。
	Document document;
	document.stories = twoStoreyStories();
	document.members = {makeMember("2-横架材天端", Vec2{-910.0, 0.0}, Vec2{910.0, 0.0}, 3264.0)};
	CHECK(parse::buildSectionDimensionCommands(document, xSection()).empty());
	CHECK(parse::buildSectionLevelMarks(document, xSection()).empty());
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
			first.dimensionStandard = "寸法";
			parse::attachDimensionCommands(first);
			CHECK(core::validateDocument(first));

			// もう一度組み立てても同じ（決定性）。
			Document second = original;
			second.dimensionStandard = "寸法";
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
	options.setDimensionStandard("JIS");
	const Document withStyle = parse::buildDocument(path, progress, options);
	CHECK(withStyle.dimensionStandard == "JIS");
	CHECK(core::validateDocument(withStyle));
	std::size_t chains = 0;
	for (const SheetCommand& sheet : withStyle.sheets)
		chains += sheet.viewport.dimensions.size();
	CHECK(chains > 0);

	const Document without = parse::buildDocument(path, progress, core::ImportOptions{});
	CHECK(without.dimensionStandard.empty());
	for (const SheetCommand& sheet : without.sheets)
		CHECK(sheet.viewport.dimensions.empty());
}

TEST_MAIN();
