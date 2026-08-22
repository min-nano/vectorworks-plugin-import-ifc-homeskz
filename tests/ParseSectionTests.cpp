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
//	通り名の並ぶ側を向くこと・映すレイヤがストーリの作るレイヤに実在すること・
//	命令の並び順に依存しない決定性。
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
using HomeskzIfcImport::parse::gridClassFor;
using HomeskzIfcImport::parse::kAxisMatchTol;
using HomeskzIfcImport::parse::kGridClassX;
using HomeskzIfcImport::parse::kGridClassY;
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
using HomeskzIfcImport::parse::sectionLayers;
using HomeskzIfcTests::near;

namespace
{
	// 試験用の通り芯（Python 版テストの _LINES と同じ配置）。X通り（縦線）= X1/X2/X3、
	// Y通り（横線）= い/ろ。bbox は x∈[0,8000]・y∈[0,6000] で中心は (4000,3000) なので、
	// センタリング後は Python 版テストと同じ ±4000 / ±3000 になる。
	//
	// 実データに寄せて、次の 3 つも混ぜてある（いずれも bbox は広げない）:
	//   * **宣言順は座標順ではない**（X3 → X1 → X2）。並べ替えが効いていることを見る。
	//   * **同名で 2 区間に分かれた通り芯**（X1 の短い区間）。1 本にまとめられる。
	//   * **無名の通り芯**（命名に使えないので通り名の候補から外れる）。
	Model sampleGridModel()
	{
		return loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
							   "#11=IFCCARTESIANPOINT((0.,6000.,0.));\n"
							   "#12=IFCCARTESIANPOINT((4000.,0.,0.));\n"
							   "#13=IFCCARTESIANPOINT((4000.,6000.,0.));\n"
							   "#14=IFCCARTESIANPOINT((8000.,0.,0.));\n"
							   "#15=IFCCARTESIANPOINT((8000.,6000.,0.));\n"
							   "#16=IFCCARTESIANPOINT((0.,1000.,0.));\n"
							   "#17=IFCCARTESIANPOINT((0.,5000.,0.));\n"
							   "#18=IFCCARTESIANPOINT((2000.,0.,0.));\n"
							   "#19=IFCCARTESIANPOINT((2000.,6000.,0.));\n"
							   "#20=IFCPOLYLINE((#10,#11));\n"
							   "#21=IFCPOLYLINE((#12,#13));\n"
							   "#22=IFCPOLYLINE((#14,#15));\n"
							   "#23=IFCPOLYLINE((#10,#14));\n"
							   "#24=IFCPOLYLINE((#11,#15));\n"
							   "#25=IFCPOLYLINE((#16,#17));\n"
							   "#26=IFCPOLYLINE((#18,#19));\n"
							   "#30=IFCGRIDAXIS('X3',#22,.T.);\n"
							   "#31=IFCGRIDAXIS('X1',#20,.T.);\n"
							   "#32=IFCGRIDAXIS('X2',#21,.T.);\n"
							   "#33=IFCGRIDAXIS('い',#23,.T.);\n"
							   "#34=IFCGRIDAXIS('ろ',#24,.T.);\n"
							   "#35=IFCGRIDAXIS('X1',#25,.T.);\n"
							   "#36=IFCGRIDAXIS($,#26,.T.);\n");
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

TEST(NamedAxesMergesRepeatsAndSkipsUnnamed)
{
	// 試験用モデルには X1 が 2 区間・無名の通り芯が 1 本ある（sampleGridModel）。
	// 同名は 1 本にまとまり、無名は通り名の候補にならない（座標 2000 の軸は出てこない）。
	Model const model = sampleGridModel();
	const std::vector<parse::GridLine> lines = parse::collectGridLines(model);
	core::Vec2 center;
	CHECK(parse::gridCenterOf(lines, center));

	const std::vector<NamedAxis> x = namedAxes(lines, center, SectionDirection::X);
	CHECK(x.size() == static_cast<std::size_t>(3));
	CHECK(std::ranges::none_of(x, [](const NamedAxis& axis) { return axis.name.empty(); }));
	CHECK(std::ranges::none_of(x, [](const NamedAxis& axis) { return near(axis.coord, -2000.0); }));
}

TEST(NamedAxesSortsTiesByName)
{
	// 同じ座標に名前の違う通り芯が 2 本あるとき（区間が分かれた別名の通り）は名前順にする
	// ——入力の並び順に依存しない決定的な結果にするため。
	const std::vector<parse::GridLine> lines{
		parse::GridLine{"X2", core::Vec2{0.0, 0.0}, core::Vec2{0.0, 1000.0}},
		parse::GridLine{"X1", core::Vec2{0.0, 2000.0}, core::Vec2{0.0, 3000.0}}};
	const std::vector<NamedAxis> axes = namedAxes(lines, core::Vec2{0.0, 0.0}, SectionDirection::X);
	CHECK(axes.size() == static_cast<std::size_t>(2));
	CHECK(axes[0].name == "X1");
	CHECK(axes[1].name == "X2");
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

TEST(NameCutsBeforeFirstAxisUsesTheFirstAsBase)
{
	// 最初の通り芯より手前で切る（まれ）。基準にできる「直前の通り」が無いので、
	// 先頭の通り芯を基準にして連番する。
	const std::vector<NamedAxis> axes{NamedAxis{"X1", 0.0}, NamedAxis{"X2", 4000.0}};
	CHECK(nameSectionCuts({-2000.0, 4000.0}, axes) == (std::vector<std::string>{"X1'", "X2"}));
}

TEST(NameCutsHandleMultiByteAndBrokenAxisNames)
{
	// 通り名は UTF-8 で、いろは文字だけなら「又」書式・それ以外は「'」書式になる。
	// 1〜4 バイトの文字と、壊れた並び（継続バイトが先頭・途中で切れている）を混ぜても
	// 落ちず、いろは以外として扱われる。
	const std::vector<NamedAxis> ascii{NamedAxis{"A1", 0.0}};
	CHECK(nameSectionCuts({1000.0}, ascii) == (std::vector<std::string>{"A1'"}));

	const std::vector<NamedAxis> twoByte{NamedAxis{"é", 0.0}}; // U+00E9（2 バイト）
	CHECK(nameSectionCuts({1000.0}, twoByte) == (std::vector<std::string>{"é'"}));

	const std::vector<NamedAxis> fourByte{NamedAxis{"𠮷", 0.0}}; // U+20BB7（4 バイト）
	CHECK(nameSectionCuts({1000.0}, fourByte) == (std::vector<std::string>{"𠮷'"}));

	const std::vector<NamedAxis> broken{NamedAxis{std::string("\x80X"), 0.0}}; // 継続バイトが先頭
	CHECK(nameSectionCuts({1000.0}, broken).size() == static_cast<std::size_t>(1));

	const std::vector<NamedAxis> truncated{NamedAxis{std::string("\xE3\x81"), 0.0}}; // 途中で切れ
	CHECK(nameSectionCuts({1000.0}, truncated).size() == static_cast<std::size_t>(1));

	// 名前が空の通り芯（IFC の AxisTag が空）。いろは判定は false 側へ倒れ、`'` が付く。
	const std::vector<NamedAxis> empty{NamedAxis{"", 0.0}};
	CHECK(nameSectionCuts({1000.0}, empty) == (std::vector<std::string>{"'"}));
}

// --- 表示レイヤ・高さ範囲 ----------------------------------------------------

TEST(SectionLayersListsStoryLayersAndGrid)
{
	const std::vector<std::string> layers = sectionLayers(sampleStories());
	CHECK(layers == (std::vector<std::string>{"1-FL", "1-横架材天端", core::kGridLayer}));
	// ストーリが無ければ映すものが無い（通り芯だけの図は作らない）。
	CHECK(sectionLayers({}).empty());
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

	// シートレイヤは全命令で同じ（軸組図 1 枚に並べる）。断面の範囲（長さ・高さ・奥行き）は
	// 命令が持たない——軸組図は範囲を限らないので、描画側の定数が受け持つ。
	for (const SectionCommand& command : commands)
	{
		CHECK(command.number == kSectionSheetNumber);
		CHECK(command.title == kSectionSheetTitle);
		CHECK(command.viewport.layers ==
			  (std::vector<std::string>{"1-FL", "1-横架材天端", core::kGridLayer}));
	}
}

TEST(BuildSectionCommandsHidesTheParallelGridClass)
{
	Model const model = sampleGridModel();
	const std::vector<SectionCommand> commands = buildSectionCommands(model, sampleDocument());

	// 軸組図には**切断面と直交する**通り芯だけを出す。切断面に平行な通り芯——とりわけ
	// 切断位置に乗っている 1 本——は紙面に平行な水平線として写り込むので、その図だけ
	// クラスごと隠す（X通りの図では X通り、Y通りの図では Y通り）。
	for (const SectionCommand& command : commands)
	{
		const char* parallel = command.direction == SectionDirection::X ? kGridClassX : kGridClassY;
		const char* perpendicular =
			command.direction == SectionDirection::X ? kGridClassY : kGridClassX;
		CHECK(command.viewport.hiddenClasses == (std::vector<std::string>{parallel}));
		// 直交する側は隠さない（隠すと通り名のバブルまで消える）。
		CHECK(std::ranges::find(command.viewport.hiddenClasses, std::string(perpendicular)) ==
			  command.viewport.hiddenClasses.end());
	}
	// 期待値は手書き（parse/Grid.h の定数と一致すること自体を確かめる）。
	CHECK(std::string(gridClassFor(SectionDirection::X)) == "01作図-01線-01基準線-01通り芯-X通り");
	CHECK(std::string(gridClassFor(SectionDirection::Y)) == "01作図-01線-01基準線-01通り芯-Y通り");
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
	// 空の命令セット＝ストーリも柱梁も無ければ、映すレイヤも切断位置も無い。
	CHECK(buildSectionCommands(model, core::Document{}).empty());

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
		CHECK(near(a[i].viewPoint.x, b[i].viewPoint.x));
		CHECK(near(a[i].viewPoint.y, b[i].viewPoint.y));
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
		// 映すレイヤはすべて実在する。
		CHECK(!section.viewport.layers.empty());
		for (const std::string& layer : section.viewport.layers)
			CHECK(storyLayers.contains(layer));
		// 隠すのは切断面に平行な通り芯のクラス 1 つだけで、**実際に通り芯が身に付けて
		// いるクラス**であること（名前がずれていると何も隠せない）。
		CHECK(section.viewport.hiddenClasses ==
			  (std::vector<std::string>{gridClassFor(section.direction)}));
		CHECK(std::ranges::any_of(document.grids, [&section](const core::GridCommand& grid)
								  { return grid.drawClass == gridClassFor(section.direction); }));
	}

	// 命令セット全体が検証を通る（描画フェーズへ渡せる）。
	CHECK(core::validateDocument(document));
}

#endif // HOMESKZ_FIXTURES_DIR

TEST_MAIN();
