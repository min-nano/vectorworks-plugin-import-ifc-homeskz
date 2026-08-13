//
//	ParseSectionTests.cpp
//
//	軸組図（断面ビューポート）解析（src/parse/Section）の単体テスト。VectorWorks SDK を
//	一切 include せず、無 SDK のテストハーネス（TestFramework.h）で走る
//	（CLAUDE.md「テスト方針」）。Python 版 test_ifc_section.py のケースを写しつつ、期待値は
//	手書きする（ROADMAP.md「Python 版出力との比較はしない」）。
//
//	検証項目（ROADMAP.md M14）: 柱と梁の**両方**が通る通りだけを切断位置にすること
//	（大引・母屋は梁とみなさない）・名前付き通り芯への照合と中間通りの命名（`'` / `又`）・
//	指示線の向きと長さ（X通り＝定 X の縦線・Y通り＝定 Y の横線＋余白）・視線の向きが
//	通り名の並ぶ側を向くこと・断面の高さ範囲が要素を包むこと・映すレイヤがストーリの作る
//	レイヤに実在すること・命令の並び順に依存しない決定性。
//
//	Python 版が `resolve_lines` を monkeypatch していたところは、**最小の STEP テキストから
//	Model を作る**（loadIfcFromText）ことで置き換える——通り芯の読み方は parse/Grid の
//	担当で、ここでは「通り芯がこう並んでいるとき何を切るか」だけを見たいため。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/BuildDocument.h"
#include "parse/Grid.h"
#include "parse/Loader.h"
#include "parse/Section.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::SectionCommand;
using HomeskzIfcImport::core::SectionDirection;
using HomeskzIfcImport::parse::buildSectionCommands;
using HomeskzIfcImport::parse::kAxisMatchTol;
using HomeskzIfcImport::parse::kSectionDepth;
using HomeskzIfcImport::parse::kSectionHeightMargin;
using HomeskzIfcImport::parse::kSectionLineMargin;
using HomeskzIfcImport::parse::kSectionSheetNumber;
using HomeskzIfcImport::parse::kSectionSheetTitle;
using HomeskzIfcImport::parse::kSectionTitleSuffix;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::namedAxes;
using HomeskzIfcImport::parse::NamedAxis;
using HomeskzIfcImport::parse::nameSectionCuts;
using HomeskzIfcImport::parse::sectionCutPositions;
using HomeskzIfcImport::parse::sectionHeightRange;
using HomeskzIfcImport::parse::sectionLayers;
using HomeskzIfcTests::near;

namespace
{
	// 試験用の通り芯（Python 版テストの _LINES と同じ配置）。X通り（縦線）= X1/X2/X3、
	// Y通り（横線）= い/ろ。bbox は x∈[0,8000]・y∈[0,6000] で中心は (4000,3000) なので、
	// センタリング後は Python 版テストと同じ ±4000 / ±3000 になる。
	Model sampleGridModel()
	{
		return loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
							   "#11=IFCCARTESIANPOINT((0.,6000.,0.));\n"
							   "#12=IFCCARTESIANPOINT((4000.,0.,0.));\n"
							   "#13=IFCCARTESIANPOINT((4000.,6000.,0.));\n"
							   "#14=IFCCARTESIANPOINT((8000.,0.,0.));\n"
							   "#15=IFCCARTESIANPOINT((8000.,6000.,0.));\n"
							   "#20=IFCPOLYLINE((#10,#11));\n"
							   "#21=IFCPOLYLINE((#12,#13));\n"
							   "#22=IFCPOLYLINE((#14,#15));\n"
							   "#23=IFCPOLYLINE((#10,#14));\n"
							   "#24=IFCPOLYLINE((#11,#15));\n"
							   "#30=IFCGRIDAXIS('X1',#20,.T.);\n"
							   "#31=IFCGRIDAXIS('X2',#21,.T.);\n"
							   "#32=IFCGRIDAXIS('X3',#22,.T.);\n"
							   "#33=IFCGRIDAXIS('い',#23,.T.);\n"
							   "#34=IFCGRIDAXIS('ろ',#24,.T.);\n");
	}

	core::ColumnCommand column(double x, double y)
	{
		core::ColumnCommand command;
		command.position = core::Vec2{x, y};
		command.elevation = 0.0;
		command.height = 3000.0;
		return command;
	}

	core::MemberCommand member(double sx, double sy, double ex, double ey,
							   const std::string& drawClass = "")
	{
		core::MemberCommand command;
		command.start = core::Vec2{sx, sy};
		command.end = core::Vec2{ex, ey};
		command.drawClass = drawClass;
		command.elevation = 3000.0;
		command.endElevation = 3000.0;
		command.height = 180.0;
		return command;
	}

	// 柱: X1∩い・X2∩い・X2∩ろ・中間 (X=2000, Y=0)。Python 版テストの _COLUMNS と同じ。
	std::vector<core::ColumnCommand> sampleColumns()
	{
		return {column(-4000.0, -3000.0), column(0.0, -3000.0), column(0.0, 3000.0),
				column(2000.0, 0.0)};
	}

	// 梁: Y 方向（X通り検出用）が X=-4000/0/2000、X 方向（Y通り検出用）が Y=-3000/0。
	std::vector<core::MemberCommand> sampleMembers()
	{
		return {member(-4000.0, -3000.0, -4000.0, 3000.0), member(0.0, -3000.0, 0.0, 3000.0),
				member(2000.0, -3000.0, 2000.0, 3000.0), member(-4000.0, -3000.0, 4000.0, -3000.0),
				member(-4000.0, 0.0, 4000.0, 0.0)};
	}

	// ストーリ 1 つ（レイヤ 2 枚）。表示レイヤの組み立てを見るための最小構成。
	std::vector<core::StoryCommand> sampleStories()
	{
		core::StoryCommand story;
		story.name = "1階";
		story.suffix = "1";
		story.elevation = 0.0;
		story.levels = {core::LevelCommand{core::kLevelFL, 0.0, "1-FL"},
						core::LevelCommand{core::kLevelBeamTop, -48.0, "1-横架材天端"}};
		return {story};
	}

	// 上の小道具で組んだ命令セット（通り芯以外）。
	core::Document sampleDocument()
	{
		core::Document document;
		document.stories = sampleStories();
		document.columns = sampleColumns();
		document.members = sampleMembers();
		return document;
	}

	std::vector<std::string> drawingNumbers(const std::vector<SectionCommand>& commands)
	{
		std::vector<std::string> numbers;
		numbers.reserve(commands.size());
		for (const SectionCommand& command : commands)
			numbers.push_back(command.viewport.drawingNumber);
		return numbers;
	}
} // namespace

// --- 名前付き通り芯 ----------------------------------------------------------

TEST(NamedAxesSplitsByDirectionAndSorts)
{
	Model const model = sampleGridModel();
	const std::vector<parse::GridLine> lines = parse::collectGridLines(model);
	core::Vec2 center;
	CHECK(parse::gridCenterOf(lines, center));

	const std::vector<NamedAxis> x = namedAxes(lines, center, SectionDirection::X);
	CHECK(x.size() == static_cast<std::size_t>(3));
	CHECK(x[0].name == "X1");
	CHECK(near(x[0].coord, -4000.0));
	CHECK(x[1].name == "X2");
	CHECK(near(x[1].coord, 0.0));
	CHECK(x[2].name == "X3");
	CHECK(near(x[2].coord, 4000.0));

	const std::vector<NamedAxis> y = namedAxes(lines, center, SectionDirection::Y);
	CHECK(y.size() == static_cast<std::size_t>(2));
	CHECK(y[0].name == "い");
	CHECK(near(y[0].coord, -3000.0));
	CHECK(y[1].name == "ろ");
	CHECK(near(y[1].coord, 3000.0));
}

// --- 切断位置（柱梁の芯）-----------------------------------------------------

TEST(CutPositionsNeedBothColumnAndBeam)
{
	const std::vector<double> x =
		sectionCutPositions(sampleColumns(), sampleMembers(), SectionDirection::X);
	// X=-4000 / 0 / 2000 は柱と Y 方向の梁が揃う。
	CHECK(x.size() == static_cast<std::size_t>(3));
	CHECK(near(x[0], -4000.0));
	CHECK(near(x[1], 0.0));
	CHECK(near(x[2], 2000.0));

	const std::vector<double> y =
		sectionCutPositions(sampleColumns(), sampleMembers(), SectionDirection::Y);
	// Y=-3000 は柱と X 方向の梁、Y=0 は中間の柱と梁。**Y=3000 は柱だけ（梁が無い）**ので
	// 切断位置にしない。
	CHECK(y.size() == static_cast<std::size_t>(2));
	CHECK(near(y[0], -3000.0));
	CHECK(near(y[1], 0.0));
}

TEST(CutPositionsIgnoreBeamOnlyAndColumnOnlyLines)
{
	// 梁だけ（柱なし）の通りは対象外。
	CHECK(sectionCutPositions({}, sampleMembers(), SectionDirection::X).empty());
	// 柱だけ（梁なし）の通りも対象外。
	CHECK(sectionCutPositions(sampleColumns(), {}, SectionDirection::X).empty());
}

TEST(CutPositionsSkipOobikiAndMoya)
{
	// 柱があっても、その通りに走るのが大引・母屋だけなら切断位置にしない。
	const std::vector<core::ColumnCommand> columns{column(-4000.0, -3000.0),
												   column(-4000.0, 3000.0)};
	const std::vector<core::MemberCommand> members{
		member(-4000.0, -3000.0, -4000.0, 3000.0, parse::CLASS_OOBIKI),
		member(-4000.0, -3000.0, 4000.0, -3000.0, parse::CLASS_MOYA)};
	CHECK(sectionCutPositions(columns, members, SectionDirection::X).empty());
	CHECK(sectionCutPositions(columns, members, SectionDirection::Y).empty());

	// 大引・母屋以外のクラスを持つ横架材は従来どおり梁として扱う。
	const std::vector<core::MemberCommand> beams{
		member(-4000.0, -3000.0, -4000.0, 3000.0, parse::CLASS_KOYABARI)};
	CHECK(sectionCutPositions(columns, beams, SectionDirection::X).size() ==
		  static_cast<std::size_t>(1));
}

TEST(CutPositionsClusterNearbyCoordinates)
{
	// 通り芯上の柱と梁が数 mm ずれていても 1 本の通りにまとまる（許容 kClusterTol）。
	const std::vector<core::ColumnCommand> columns{column(1000.0, 0.0)};
	const std::vector<core::MemberCommand> members{member(1030.0, -3000.0, 1030.0, 3000.0)};
	const std::vector<double> cuts = sectionCutPositions(columns, members, SectionDirection::X);
	CHECK(cuts.size() == static_cast<std::size_t>(1));
	CHECK(near(cuts[0], 1015.0));
}

// --- 通り名 ------------------------------------------------------------------

TEST(NameCutsUsesAxisNamesAndIntermediateForms)
{
	const std::vector<NamedAxis> x{NamedAxis{"X1", -4000.0}, NamedAxis{"X2", 0.0},
								   NamedAxis{"X3", 4000.0}};
	// -4000 / 0 は通り芯そのもの、2000 は X2 の次の中間通り（数字書式なので `'`）。
	const std::vector<std::string> names = nameSectionCuts({-4000.0, 0.0, 2000.0, 3000.0}, x);
	CHECK(names == (std::vector<std::string>{"X1", "X2", "X2'", "X2''"}));

	const std::vector<NamedAxis> y{NamedAxis{"い", -3000.0}, NamedAxis{"ろ", 3000.0}};
	// いろは書式は `又` を前置して増やす。
	CHECK(nameSectionCuts({-3000.0, 0.0, 1000.0}, y) ==
		  (std::vector<std::string>{"い", "又い", "又又い"}));
}

TEST(NameCutsMatchesWithinTolerance)
{
	const std::vector<NamedAxis> axes{NamedAxis{"X1", 0.0}};
	// 許容内のずれは通り芯そのものとみなす。外れれば中間通り。
	CHECK(nameSectionCuts({kAxisMatchTol / 2.0}, axes) == (std::vector<std::string>{"X1"}));
	CHECK(nameSectionCuts({kAxisMatchTol * 4.0}, axes) == (std::vector<std::string>{"X1'"}));
}

TEST(NameCutsWithoutAxesFallsBackToNumbers)
{
	// 名前付き通り芯が 1 本も無ければ連番だけを名前にする（無名の図面でも図番が衝突しない）。
	CHECK(nameSectionCuts({0.0, 1000.0}, {}) == (std::vector<std::string>{"1", "2"}));
}

// --- 表示レイヤ・高さ範囲 ----------------------------------------------------

TEST(SectionLayersListsStoryLayersAndGrid)
{
	const std::vector<std::string> layers = sectionLayers(sampleStories());
	CHECK(layers == (std::vector<std::string>{"1-FL", "1-横架材天端", core::kGridLayer}));
	// ストーリが無ければ映すものが無い（通り芯だけの図は作らない）。
	CHECK(sectionLayers({}).empty());
}

TEST(SectionHeightRangeCoversElementsWithMargin)
{
	core::Document document = sampleDocument();
	double start = 0.0;
	double end = 0.0;
	CHECK(sectionHeightRange(document, start, end));
	// 柱は 0〜3000、横架材は天端 3000・下端 2820。範囲は上下に余白ぶん広い。
	CHECK(near(start, 0.0 - kSectionHeightMargin));
	CHECK(near(end, 3000.0 + kSectionHeightMargin));

	// 高さの分かる要素が 1 つも無ければ範囲は求まらない。
	CHECK(!sectionHeightRange(core::Document{}, start, end));
}

// --- 命令の組み立て ----------------------------------------------------------

TEST(BuildSectionCommandsPlacesCutsAndNames)
{
	Model const model = sampleGridModel();
	const std::vector<SectionCommand> commands = buildSectionCommands(model, sampleDocument());

	// X通り 3 本（X1 / X2 / X2'）→ Y通り 2 本（い / 又い）の順。**既製ビューポートの枚数に
	// 縛られない**（Python 版は 20 枚が上限だった）。
	CHECK(drawingNumbers(commands) == (std::vector<std::string>{"X1", "X2", "X2'", "い", "又い"}));
	CHECK(commands[0].viewport.drawingTitle == std::string("X1") + kSectionTitleSuffix);
	CHECK(commands[4].viewport.drawingTitle == std::string("又い") + kSectionTitleSuffix);

	// シートレイヤは全命令で同じ（軸組図 1 枚に並べる）。
	for (const SectionCommand& command : commands)
	{
		CHECK(command.number == kSectionSheetNumber);
		CHECK(command.title == kSectionSheetTitle);
		CHECK(near(command.depth, kSectionDepth));
		CHECK(command.startHeight < command.endHeight);
		CHECK(command.viewport.layers ==
			  (std::vector<std::string>{"1-FL", "1-横架材天端", core::kGridLayer}));
	}
}

TEST(BuildSectionCommandsOrientsLinesAndViewDirection)
{
	Model const model = sampleGridModel();
	const std::vector<SectionCommand> commands = buildSectionCommands(model, sampleDocument());

	// X通り: 定 X の縦線で、Y は通り芯 bbox（-3000..3000）に余白を足した範囲。
	const SectionCommand& x1 = commands[0];
	CHECK(x1.direction == SectionDirection::X);
	CHECK(near(x1.lineStart.x, -4000.0));
	CHECK(near(x1.lineEnd.x, -4000.0));
	CHECK(near(x1.lineStart.y, -3000.0 - kSectionLineMargin));
	CHECK(near(x1.lineEnd.y, 3000.0 + kSectionLineMargin));
	// 視線は −X 側（＝東から西を見る）。図面の右へ Y が増え、い・ろ が左から右に並ぶ。
	CHECK(x1.viewPoint.x < x1.lineStart.x);
	CHECK(near(x1.viewPoint.y, 0.0));

	// Y通り: 定 Y の横線で、X は bbox（-4000..4000）＋余白。
	const SectionCommand& y1 = commands[3];
	CHECK(y1.direction == SectionDirection::Y);
	CHECK(near(y1.lineStart.y, -3000.0));
	CHECK(near(y1.lineEnd.y, -3000.0));
	CHECK(near(y1.lineStart.x, -4000.0 - kSectionLineMargin));
	CHECK(near(y1.lineEnd.x, 4000.0 + kSectionLineMargin));
	// 視線は +Y 側（＝南から北を見る）。図面の右へ X が増え、X1・X2 が左から右に並ぶ。
	CHECK(y1.viewPoint.y > y1.lineStart.y);
	CHECK(near(y1.viewPoint.x, 0.0));
}

TEST(BuildSectionCommandsNeedsGridStoriesAndHeights)
{
	// 通り芯が無ければ平面の広がりも通り名も決まらない。
	Model const empty = loadIfcFromText("");
	CHECK(buildSectionCommands(empty, sampleDocument()).empty());

	Model const model = sampleGridModel();
	// ストーリが無ければ映すレイヤが無い。
	core::Document noStories = sampleDocument();
	noStories.stories.clear();
	CHECK(buildSectionCommands(model, noStories).empty());

	// 柱も梁も無ければ切断位置が 1 つも無い。
	core::Document noFrame;
	noFrame.stories = sampleStories();
	CHECK(buildSectionCommands(model, noFrame).empty());
}

TEST(BuildSectionCommandsIsDeterministic)
{
	Model const model = sampleGridModel();
	core::Document shuffled = sampleDocument();
	// 命令の並びを逆にしても同じ結果になる（入力順に依存しない）。
	std::ranges::reverse(shuffled.columns);
	std::ranges::reverse(shuffled.members);

	const std::vector<SectionCommand> a = buildSectionCommands(model, sampleDocument());
	const std::vector<SectionCommand> b = buildSectionCommands(model, shuffled);
	CHECK(drawingNumbers(a) == drawingNumbers(b));
	CHECK(a.size() == b.size());
	for (std::size_t i = 0; i < a.size(); ++i)
	{
		CHECK(near(a[i].lineStart.x, b[i].lineStart.x));
		CHECK(near(a[i].lineStart.y, b[i].lineStart.y));
		CHECK(near(a[i].startHeight, b[i].startHeight));
		CHECK(near(a[i].endHeight, b[i].endHeight));
	}
}

TEST(BuildSectionCommandsPassDocumentValidation)
{
	Model const model = sampleGridModel();
	// **section 命令だけ**を空の Document に載せて検証する（試験用の柱・横架材はレイヤ名や
	// クラス名を持たない骨だけの命令なので、そのまま検証へ回すと section とは無関係な
	// 理由で落ちる。Python 版テストも section 以外を空にした文書を検証している）。
	core::Document document;
	document.sections = buildSectionCommands(model, sampleDocument());
	CHECK(!document.sections.empty());
	CHECK(core::validateDocument(document));
}

// --- フィクスチャ通し --------------------------------------------------------

#ifdef HOMESKZ_FIXTURES_DIR

TEST(FixtureSectionsCutRealGridLinesAndShowExistingLayers)
{
	// 実データ 1 件で端から端まで通す（buildDocument は重いので 1 ファイルに絞る）。
	const core::Document document =
		parse::buildDocument(HomeskzIfcTests::fixturePath("サンプル1 (住木邸新築工事).ifc"));
	CHECK(!document.sections.empty());

	// story 命令が作るレイヤ名（軸組図の表示レイヤはここに実在しなければならない）。
	std::set<std::string> storyLayers{core::kGridLayer};
	for (const core::StoryCommand& story : document.stories)
	{
		for (const core::LevelCommand& level : story.levels)
			storyLayers.insert(level.layer);
	}

	std::set<std::string> numbers;
	for (const core::SectionCommand& section : document.sections)
	{
		CHECK(section.number == kSectionSheetNumber);
		CHECK(section.title == kSectionSheetTitle);
		// 図番は通りごとに一意（同じ図番のビューポートが 2 枚できない）。
		CHECK(numbers.insert(section.viewport.drawingNumber).second);
		CHECK(!section.viewport.drawingNumber.empty());
		CHECK(section.viewport.drawingTitle ==
			  section.viewport.drawingNumber + kSectionTitleSuffix);
		// 指示線は縮退しておらず、方向どおり（X通り＝定 X・Y通り＝定 Y）。
		if (section.direction == SectionDirection::X)
		{
			CHECK(near(section.lineStart.x, section.lineEnd.x));
			CHECK(!near(section.lineStart.y, section.lineEnd.y));
		}
		else
		{
			CHECK(near(section.lineStart.y, section.lineEnd.y));
			CHECK(!near(section.lineStart.x, section.lineEnd.x));
		}
		// 高さ範囲は建物を包む（下端は GL 以下・上端は最上階のストーリ高さ以上）。
		CHECK(section.startHeight < section.endHeight);
		CHECK(section.startHeight <= 0.0);
		CHECK(section.endHeight >= document.stories.back().elevation);
		// 映すレイヤはすべて実在する。
		CHECK(!section.viewport.layers.empty());
		for (const std::string& layer : section.viewport.layers)
			CHECK(storyLayers.contains(layer));
	}

	// 命令セット全体が検証を通る（描画フェーズへ渡せる）。
	CHECK(core::validateDocument(document));
}

#endif // HOMESKZ_FIXTURES_DIR

TEST_MAIN();
