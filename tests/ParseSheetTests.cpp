//
//	ParseSheetTests.cpp
//
//	シート（伏図）解析（src/parse/Sheet）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M13）: 伏図のタイトル（床／小屋／母屋）・切断レベルによる span 柱
//	レイヤの絞り込み・シートレイヤ番号の連番（基礎伏図 1 →柱梁伏図 2… →母屋伏図）・
//	基礎の有無による基礎伏図とアンカーボルトの出し分け・母屋伏図を作る階（屋根版のある階）・
//	**グラフィック凡例**（柱梁伏図・母屋伏図は常に "床伏図凡例"、基礎伏図はアンカーボルトを
//	置いたときだけ "基礎伏図凡例"）・
//	**表示レイヤが必ずストーリの作るレイヤに実在すること**（レイヤ名の規約がズレていない）・
//	並び順に依存しない決定性。実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/AnchorBolt.h"
#include "parse/Column.h"
#include "parse/ColumnMark.h"
#include "parse/Context.h"
#include "parse/Footing.h"
#include "parse/Loader.h"
#include "parse/Rafter.h"
#include "parse/Sheet.h"
#include "parse/Story.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::SheetCommand;
using HomeskzIfcImport::parse::buildAnchorBoltCommands;
using HomeskzIfcImport::parse::buildColumnCommands;
using HomeskzIfcImport::parse::buildFloorFramingSheetCommands;
using HomeskzIfcImport::parse::buildFoundationSheetCommands;
using HomeskzIfcImport::parse::buildFoundationStoryCommand;
using HomeskzIfcImport::parse::buildMoyaSheetCommands;
using HomeskzIfcImport::parse::buildSheetCommands;
using HomeskzIfcImport::parse::buildStoryCommands;
using HomeskzIfcImport::parse::collectColumnSpans;
using HomeskzIfcImport::parse::collectPlanMarkLayers;
using HomeskzIfcImport::parse::collectStories;
using HomeskzIfcImport::parse::ColumnSpan;
using HomeskzIfcImport::parse::Context;
using HomeskzIfcImport::parse::floorPlanTitle;
using HomeskzIfcImport::parse::hasFoundation;
using HomeskzIfcImport::parse::kFloorLegendStyle;
using HomeskzIfcImport::parse::kFloorPlanCutOffset;
using HomeskzIfcImport::parse::kFloorPlanStartNumber;
using HomeskzIfcImport::parse::kFoundationLegendStyle;
using HomeskzIfcImport::parse::kFoundationSheetNumber;
using HomeskzIfcImport::parse::kFoundationSheetTitle;
using HomeskzIfcImport::parse::kLayerFoundationAnchor;
using HomeskzIfcImport::parse::kMoyaPlanCutOffset;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::moyaPlanTitle;
using HomeskzIfcImport::parse::PlanMarkLayer;
using HomeskzIfcImport::parse::spanLayersAtCut;
using HomeskzIfcImport::parse::storyHasRoofSlab;
using HomeskzIfcImport::parse::storyLayerName;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;

namespace
{
	// 試験用の span 柱レイヤ。3 階建てで、1 階管柱・1〜2 階通し柱・下屋の小屋束・2 階管柱・
	// 主屋根の小屋束を並べたもの。
	std::vector<ColumnSpan> sampleSpans()
	{
		return {ColumnSpan{1.0, 2.0, "1to2-柱"}, ColumnSpan{1.0, 3.0, "1to3-柱"},
				ColumnSpan{2.0, 2.5, "2to2.5-柱"}, ColumnSpan{2.0, 3.0, "2to3-柱"},
				ColumnSpan{3.0, 3.5, "3to3.5-柱"}};
	}

	bool contains(const std::vector<std::string>& values, const std::string& value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	// story 命令が作るデザインレイヤ名の全集合（伏図の表示レイヤはここに実在しなければ
	// ならない）。基礎ストーリは buildStoryCommands には含まれないので、呼び出し側が
	// 基礎のレイヤを足す。
	std::set<std::string> storyLayerNames(const Model& model)
	{
		std::set<std::string> names;
		for (const core::StoryCommand& story : buildStoryCommands(model))
		{
			for (const core::LevelCommand& level : story.levels)
				names.insert(level.layer);
		}
		return names;
	}
} // namespace

// --- タイトル ---------------------------------------------------------------

TEST(FloorPlanTitleNamesFloorsAndRoof)
{
	// 一般階は「{階番号}階床伏図」、最上階は主屋根が架かる階番号を付けた「{階数}階小屋伏図」。
	CHECK(floorPlanTitle(0, false, 3) == "1階床伏図");
	CHECK(floorPlanTitle(1, false, 3) == "2階床伏図");
	CHECK(floorPlanTitle(2, true, 3) == "2階小屋伏図");
	// 2 階建て（ストーリ 2 つ）なら 1 階床伏図 + 1 階小屋伏図。
	CHECK(floorPlanTitle(1, true, 2) == "1階小屋伏図");
}

TEST(MoyaPlanTitleUsesStoryIndex)
{
	// 屋根が架かる階番号（0 起点 index をそのまま）。3 階建ての主屋根は index=3。
	CHECK(moyaPlanTitle(1) == "1階母屋伏図");
	CHECK(moyaPlanTitle(3) == "3階母屋伏図");
}

// --- 切断レベルによる span 柱レイヤの絞り込み --------------------------------

TEST(SpanLayersAtCutIncludesSpansCrossingTheCut)
{
	const std::vector<ColumnSpan> spans = sampleSpans();

	// 1 階床伏図（切断 1.25）: 1 階を base とする柱と、そこから上へ貫く通し柱。
	const std::vector<std::string> first = spanLayersAtCut(spans, 0 + kFloorPlanCutOffset);
	CHECK(first == (std::vector<std::string>{"1to2-柱", "1to3-柱"}));

	// 2 階床伏図（切断 2.25）: 2 階を base とする柱・小屋束に加え、貫いてきた通し柱。
	const std::vector<std::string> second = spanLayersAtCut(spans, 1 + kFloorPlanCutOffset);
	CHECK(second == (std::vector<std::string>{"1to3-柱", "2to2.5-柱", "2to3-柱"}));

	// 3 階小屋伏図（切断 3.25）: **下屋の小屋束（to=2.5）は写り込まない**。3 階床までの
	// 柱（to=3）も切断より下なので出ず、主屋根の小屋束だけが残る。
	const std::vector<std::string> top = spanLayersAtCut(spans, 2 + kFloorPlanCutOffset);
	CHECK(top == (std::vector<std::string>{"3to3.5-柱"}));
}

TEST(MoyaCutShowsOnlyColumnsPiercingTheRoof)
{
	const std::vector<ColumnSpan> spans = sampleSpans();

	// 1 階母屋伏図（切断 2.75）: 下屋を貫いて立ち上がる主屋の柱（1to3 / 2to3）だけ。
	// 母屋を支える下屋の小屋束（2to2.5）は切断より低いので断面としては出ない。
	const std::vector<std::string> lower = spanLayersAtCut(spans, 1 + kMoyaPlanCutOffset);
	CHECK(lower == (std::vector<std::string>{"1to3-柱", "2to3-柱"}));

	// 2 階母屋伏図（切断 3.75）: **柱レイヤは 1 つも載らない**。主屋根を貫いて立ち上がる柱は
	// 無く、主屋根を支える小屋束（3to3.5）も切断より低い（to=3.5 < 3.75）ため断面は出ない
	// ——母屋伏図は母屋の上からの見下げ図なので、これが意図した結果（小屋束の位置は M12 の
	// 伏図記号で示す）。
	const std::vector<std::string> upper = spanLayersAtCut(spans, 2 + kMoyaPlanCutOffset);
	CHECK(upper.empty());
}

TEST(SpanLayersAtCutHandlesEmptySpans)
{
	// 柱が 1 本も無い（span レイヤが無い）文書でも落ちず、単に空になる。
	CHECK(spanLayersAtCut({}, 1.25).empty());
}

// --- フィクスチャ通し --------------------------------------------------------

#ifdef HOMESKZ_FIXTURES_DIR

TEST(FoundationSheetOnlyWhenFoundationExists)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<SheetCommand> sheets = buildFoundationSheetCommands(model);
		if (!hasFoundation(model))
		{
			CHECK(sheets.empty());
			continue;
		}
		CHECK(sheets.size() == 1);
		CHECK(sheets[0].number == kFoundationSheetNumber);
		CHECK(sheets[0].title == kFoundationSheetTitle);
		// 図番・図面タイトルはシートレイヤ番号・タイトルと同じ値。
		CHECK(sheets[0].viewport.drawingNumber == sheets[0].number);
		CHECK(sheets[0].viewport.drawingTitle == sheets[0].title);
		// 底盤・立上り・床束・アンカーボルト・通り芯の 5 枚。
		CHECK(sheets[0].viewport.layers.size() == 5);
		CHECK(contains(sheets[0].viewport.layers, "F-底盤"));
		CHECK(contains(sheets[0].viewport.layers, "F-立上り"));
		CHECK(contains(sheets[0].viewport.layers, "F-床束"));
		CHECK(contains(sheets[0].viewport.layers, kLayerFoundationAnchor));
		CHECK(contains(sheets[0].viewport.layers, core::kGridLayer));
	}
}

TEST(FloorFramingSheetPerStoryWithBeamAndGridLayers)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		Context context(model);
		const auto stories = collectStories(context);
		const std::vector<SheetCommand> sheets = buildFloorFramingSheetCommands(model);
		// ストーリ 1 つにつき伏図 1 枚。番号は 2 から連番。
		CHECK(sheets.size() == stories.size());
		for (std::size_t i = 0; i < sheets.size(); ++i)
		{
			const bool isTop = stories[i].isTop;
			CHECK(sheets[i].number == std::to_string(kFloorPlanStartNumber + static_cast<int>(i)));
			CHECK(sheets[i].title == floorPlanTitle(i, isTop, stories.size()));

			// その階の横架材レイヤ（一般階＝横架材天端・最上階＝軒高）と通り芯は必ず載る。
			CHECK(contains(sheets[i].viewport.layers,
						   storyLayerName(i, isTop, isTop ? "軒高" : "横架材天端")));
			CHECK(contains(sheets[i].viewport.layers, core::kGridLayer));

			// 最上階以外は床（FL）が載り、最下階は基礎があるときだけアンカーボルトが載る。
			CHECK(contains(sheets[i].viewport.layers, storyLayerName(i, isTop, "FL")) == !isTop);
			const bool expectAnchor = (i == 0) && !isTop && hasFoundation(model);
			CHECK(contains(sheets[i].viewport.layers, kLayerFoundationAnchor) == expectAnchor);
		}
	}
}

TEST(MoyaSheetPerStoryWithRoofSlab)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		Context context(model);
		const auto stories = collectStories(context);
		const std::vector<SheetCommand> sheets = buildMoyaSheetCommands(model);

		// 屋根版を持つ階の数だけ作られ、番号は柱梁伏図の後に続く。
		std::size_t expected = 0;
		for (const auto& info : stories)
		{
			if (storyHasRoofSlab(context, info.id))
				++expected;
		}
		CHECK(sheets.size() == expected);

		std::size_t seq = 0;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			if (!storyHasRoofSlab(context, stories[i].id))
				continue;
			const SheetCommand& sheet = sheets[seq];
			const int base = kFloorPlanStartNumber + static_cast<int>(stories.size());
			CHECK(sheet.number == std::to_string(base + static_cast<int>(seq)));
			CHECK(sheet.title == moyaPlanTitle(i));
			// 屋根版のある階には必ず垂木・野地板レイヤが作られる（parse/Story）。
			CHECK(contains(sheet.viewport.layers, storyLayerName(i, stories[i].isTop, "垂木")));
			CHECK(contains(sheet.viewport.layers, storyLayerName(i, stories[i].isTop, "野地板")));
			CHECK(contains(sheet.viewport.layers, core::kGridLayer));
			// 母屋伏図には床（FL）を載せない（梁組と分ける図なので）。
			CHECK(!contains(sheet.viewport.layers, storyLayerName(i, stories[i].isTop, "FL")));
			++seq;
		}
	}
}

TEST(SheetNumbersAreUniqueAndConsecutive)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<SheetCommand> sheets = buildSheetCommands(model);
		CHECK(!sheets.empty());

		// 基礎がある文書は基礎伏図が先頭（番号 1）。無ければ柱梁伏図の 2 から始まる。
		std::set<std::string> numbers;
		int expected = hasFoundation(model) ? 1 : kFloorPlanStartNumber;
		for (const SheetCommand& sheet : sheets)
		{
			CHECK(sheet.number == std::to_string(expected));
			CHECK(!sheet.title.empty());
			CHECK(!sheet.viewport.layers.empty());
			numbers.insert(sheet.number);
			++expected;
		}
		CHECK(numbers.size() == sheets.size());
	}
}

TEST(ViewportLayersExistAmongStoryLayers)
{
	// **伏図が映そうとするレイヤは必ず実在する**。ストーリが作るレイヤ名（＋通り芯・
	// 基礎ストーリの 4 枚・伏図記号レイヤ）に含まれない名前があれば、レイヤ名の規約が
	// どこかでズレている（命令はあるのにビューポートが空になる、という形の不具合を防ぐ
	// 関門）。**伏図記号レイヤ（"{to}-柱伏図記号"）はストーリが作らない独立レイヤ**で、
	// 通り芯 "共通" と同じく描画側（draw/ColumnMark）が用意するので、ここも通り芯と
	// 同じ扱いで既知の名前に足す（規約を持つ parse/ColumnMark から引く）。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		std::set<std::string> known = storyLayerNames(model);
		known.insert(core::kGridLayer);
		core::StoryCommand foundation;
		if (buildFoundationStoryCommand(model, foundation))
		{
			for (const core::LevelCommand& level : foundation.levels)
				known.insert(level.layer);
		}
		for (const PlanMarkLayer& layer :
			 collectPlanMarkLayers(collectColumnSpans(buildColumnCommands(model))))
			known.insert(layer.layer);

		for (const SheetCommand& sheet : buildSheetCommands(model))
		{
			for (const std::string& layer : sheet.viewport.layers)
				CHECK(known.count(layer) == 1);
		}
	}
}

// --- グラフィック凡例（M13） ------------------------------------------------

TEST(FoundationSheetLegendFollowsAnchorBolts)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<SheetCommand> sheets = buildFoundationSheetCommands(model);
		if (sheets.empty())
			continue;

		// 凡例に並ぶのは基礎伏図に映るシンボル（＝アンカーボルト）なので、**1 本も
		// 置かない文書では凡例そのものを作らない**（空の箱を図面に残さない）。
		const bool expected = !buildAnchorBoltCommands(model).empty();
		CHECK(sheets[0].legend.has_value() == expected);
		if (expected)
			CHECK(sheets[0].legend->style == kFoundationLegendStyle);
	}
}

TEST(FloorAndMoyaSheetsAlwaysCarryFloorLegend)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		// 柱梁伏図・母屋伏図は**必ず**凡例を載せる（何が並ぶかはスタイルが決めるので、
		// 解析側では中身の有無を判断できない）。スタイルは基礎伏図とは別のもの。
		for (const std::vector<SheetCommand>& sheets :
			 {buildFloorFramingSheetCommands(model), buildMoyaSheetCommands(model)})
		{
			for (const SheetCommand& sheet : sheets)
			{
				CHECK(sheet.legend.has_value());
				CHECK(sheet.legend->style == kFloorLegendStyle);
			}
		}
	}
}

TEST(SheetCommandsAreDeterministic)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<SheetCommand> first = buildSheetCommands(model);
		const std::vector<SheetCommand> second = buildSheetCommands(model);
		CHECK(first.size() == second.size());
		for (std::size_t i = 0; i < first.size(); ++i)
		{
			CHECK(first[i].number == second[i].number);
			CHECK(first[i].title == second[i].title);
			CHECK(first[i].viewport.layers == second[i].viewport.layers);
			CHECK(first[i].legend.has_value() == second[i].legend.has_value());
			if (first[i].legend.has_value() && second[i].legend.has_value())
				CHECK(first[i].legend->style == second[i].legend->style);
		}
	}
}

#endif // HOMESKZ_FIXTURES_DIR

TEST_MAIN();
