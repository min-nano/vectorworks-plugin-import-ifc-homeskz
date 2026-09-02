//
//	ParseStoryTests.cpp
//
//	ストーリ解析（src/parse/Story）＋希望レイヤ順の計算（src/core/desiredStoryLayerOrder）
//	の単体テスト。VectorWorks SDK を一切 include せず、無 SDK のテストハーネス
//	（TestFramework.h）で走る（CLAUDE.md「テスト方針」: core/ parse/ は無 SDK で単体テスト）。
//	**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M3）: ローカル配置 Z 抽出・横架材天端オフセット（列挙順に
//	依存しない最大負値）・ストーリ収集（Elevation 昇順・最上階判定・非 FL 除外）・
//	story 命令の組み立て（一般階=FL＋横架材天端、最上階=軒高。屋根版のある階は 垂木・野地板 を
//	その直上に積む＝M6。柱のある階は span 柱レベルを最上段に積む＝M8）・
//	span 柱レイヤ名の生成と分解（spanLayerName / parseSpanLayer）・
//	希望レイヤ順（"共通" 先頭・最上階→最下階・床は背面）・決定性。実フィクスチャのパスは
//	CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Loader.h"
#include "parse/Story.h"

#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::desiredStoryLayerOrder;
using HomeskzIfcImport::core::LevelCommand;
using HomeskzIfcImport::core::StoryCommand;
using HomeskzIfcImport::parse::buildStoryCommands;
using HomeskzIfcImport::parse::collectStories;
using HomeskzIfcImport::parse::Entity;
using HomeskzIfcImport::parse::getLocalPlacementZ;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::parseSpanLayer;
using HomeskzIfcImport::parse::resolveBeamTopOffset;
using HomeskzIfcImport::parse::spanLayerName;
using HomeskzIfcImport::parse::StoryInfo;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// #id の IfcColumn（ObjectPlacement 付き）を 1 つだけ持つ最小モデルを作る。
	// z を Location の Z に持つローカル配置。
	Model columnModel(double z)
	{
		const std::string text = "#10=IFCCARTESIANPOINT((0.,0.," + std::to_string(z) +
								 "));\n"
								 "#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
								 "#12=IFCLOCALPLACEMENT($,#11);\n"
								 "#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n";
		return loadIfcFromText(text);
	}

	// name の StoryCommand を探す（無ければ nullptr）。
	const StoryCommand* find(const std::vector<StoryCommand>& stories, const std::string& name)
	{
		for (const StoryCommand& s : stories)
			if (s.name == name)
				return &s;
		return nullptr;
	}

	// レベルの type 名だけを取り出す（並び検証を簡潔にする）。
	std::vector<std::string> levelTypes(const StoryCommand& story)
	{
		std::vector<std::string> types;
		for (const LevelCommand& lv : story.levels)
			types.push_back(lv.type);
		return types;
	}

	// 文字列ベクタの一致（CHECK_EQ は operator<< を要求し vector は非対応のため）。
	bool sameVec(const std::vector<std::string>& a, const std::vector<std::string>& b)
	{
		return a == b;
	}
} // namespace

// --------------------------------------------------------------------------
// - getLocalPlacementZ: ローカル配置 Z の抽出
// ---------------------------------------------------------------------------

TEST(get_local_placement_z_extracts_z)
{
	Model const model = columnModel(-48.0);
	const Entity* column = model.entity(13);
	CHECK(column != nullptr);
	double z = 0.0;
	if (column != nullptr)
	{
		CHECK(getLocalPlacementZ(model, *column, z));
		CHECK(near(z, -48.0));
	}
}

TEST(get_local_placement_z_false_when_placement_missing)
{
	// ObjectPlacement が $（未設定）の柱は Z を取れない。
	Model const model = loadIfcFromText("#13=IFCCOLUMN('c',$,$,$,$,$,$,$);\n");
	const Entity* column = model.entity(13);
	CHECK(column != nullptr);
	double z = 123.0;
	if (column != nullptr)
		CHECK(!getLocalPlacementZ(model, *column, z));
}

TEST(get_local_placement_z_false_when_relative_placement_not_3d)
{
	// RelativePlacement が IfcAxis2Placement3D でない（2D 等）なら取れない。
	Model const model = loadIfcFromText("#11=IFCAXIS2PLACEMENT2D(#10,$);\n"
										"#12=IFCLOCALPLACEMENT($,#11);\n"
										"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n");
	const Entity* column = model.entity(13);
	double z = 0.0;
	CHECK(column != nullptr && !getLocalPlacementZ(model, *column, z));
}

TEST(get_local_placement_z_false_when_location_not_cartesian)
{
	// Location が IfcCartesianPoint でない（IfcDirection 等）なら取れない。
	Model const model = loadIfcFromText("#10=IFCDIRECTION((0.,0.,1.));\n"
										"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
										"#12=IFCLOCALPLACEMENT($,#11);\n"
										"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n");
	const Entity* column = model.entity(13);
	double z = 0.0;
	CHECK(column != nullptr && !getLocalPlacementZ(model, *column, z));
}

TEST(get_local_placement_z_false_when_coords_2d)
{
	// 座標が 2 要素（Z が無い）なら取れない。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.));\n"
										"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
										"#12=IFCLOCALPLACEMENT($,#11);\n"
										"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n");
	const Entity* column = model.entity(13);
	double z = 0.0;
	CHECK(column != nullptr && !getLocalPlacementZ(model, *column, z));
}

// --------------------------------------------------------------------------
// - collectStoryElements: 階に属する要素の列挙
// ---------------------------------------------------------------------------

TEST(collect_story_elements_lists_contained_elements)
{
	// IfcRelContainedInSpatialStructure の RelatedElements を、rel の #id 昇順・
	// 記述順で返す（列挙順に依存しない決定的な並び）。
	Model const model =
		loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,-48.));\n"
						"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
						"#12=IFCLOCALPLACEMENT($,#11);\n"
						"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n"
						"#14=IFCSLAB('s',$,$,$,$,#12,$,$);\n"
						"#20=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#13,#14),#20);\n");
	const std::vector<int> elements = HomeskzIfcImport::parse::collectStoryElements(model, 20);
	CHECK_EQ(elements.size(), static_cast<std::size_t>(2));
	if (elements.size() == 2)
	{
		CHECK_EQ(elements[0], 13);
		CHECK_EQ(elements[1], 14);
	}
}

TEST(collect_story_elements_empty_when_storey_unreferenced)
{
	// 誰からも参照されていない階（要素を 1 つも持たない階）は空を返す。
	Model const model =
		loadIfcFromText("#20=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n");
	CHECK(HomeskzIfcImport::parse::collectStoryElements(model, 20).empty());
}

// --------------------------------------------------------------------------
// - resolveBeamTopOffset: 横架材天端オフセット
// ---------------------------------------------------------------------------

TEST(beam_top_offset_finds_column)
{
	Model const model =
		loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,-48.));\n"
						"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
						"#12=IFCLOCALPLACEMENT($,#11);\n"
						"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n"
						"#20=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#13),#20);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), -48.0));
}

TEST(beam_top_offset_finds_slab)
{
	Model const model =
		loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,-36.));\n"
						"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
						"#12=IFCLOCALPLACEMENT($,#11);\n"
						"#13=IFCSLAB('s',$,$,$,$,#12,$,$);\n"
						"#20=IFCBUILDINGSTOREY('s',$,'2FL',$,$,$,$,$,.ELEMENT.,3273.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#13),#20);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), -36.0));
}

TEST(beam_top_offset_ignores_non_column_slab)
{
	// IfcBeam の負 Z は横架材天端の基準にしない（柱・床版のみ）。候補無しで 0。
	Model const model =
		loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,-100.));\n"
						"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
						"#12=IFCLOCALPLACEMENT($,#11);\n"
						"#13=IFCBEAM('b',$,$,$,$,#12,$,$);\n"
						"#20=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#13),#20);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), 0.0));
}

TEST(beam_top_offset_zero_when_no_elements)
{
	Model const model =
		loadIfcFromText("#20=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), 0.0));
}

TEST(beam_top_offset_ignores_rel_for_other_structure)
{
	// storey を RelatedElements 側に含むが RelatingStructure が別の階である rel は無視する
	// （その rel はこの階の格納要素を表さない）。候補無しで 0。
	Model const model =
		loadIfcFromText("#20=IFCBUILDINGSTOREY('a',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#21=IFCBUILDINGSTOREY('b',$,'2FL',$,$,$,$,$,.ELEMENT.,3273.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#20),#21);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), 0.0));
}

TEST(beam_top_offset_skips_rel_with_null_related)
{
	// RelatedElements が $（リストでない）rel は読み飛ばす。候補無しで 0。
	Model const model =
		loadIfcFromText("#20=IFCBUILDINGSTOREY('a',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,$,#20);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), 0.0));
}

TEST(beam_top_offset_ignores_non_negative_z)
{
	// ローカル Z が 0 以上の柱は横架材天端の基準にしない（負値のみ）。候補無しで 0。
	Model const model =
		loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,12.));\n"
						"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
						"#12=IFCLOCALPLACEMENT($,#11);\n"
						"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n"
						"#20=IFCBUILDINGSTOREY('a',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#13),#20);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), 0.0));
}

TEST(beam_top_offset_returns_maximum_regardless_of_order)
{
	// 柱 -36 と床版 -48 の両方があるとき、床に最も近接した（0 以下の最大）-36 を返す。
	// 列挙順に依らない決定性。
	Model const model =
		loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,-36.));\n"
						"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
						"#12=IFCLOCALPLACEMENT($,#11);\n"
						"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n"
						"#14=IFCCARTESIANPOINT((0.,0.,-48.));\n"
						"#15=IFCAXIS2PLACEMENT3D(#14,$,$);\n"
						"#16=IFCLOCALPLACEMENT($,#15);\n"
						"#17=IFCSLAB('s',$,$,$,$,#16,$,$);\n"
						"#20=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#13,#17),#20);\n");
	CHECK(near(resolveBeamTopOffset(model, 20), -36.0));
}

// --------------------------------------------------------------------------
// - collectStories: Elevation 昇順・最上階判定・非 FL 除外
// ---------------------------------------------------------------------------

TEST(collect_stories_sorts_and_marks_top)
{
	// わざと Elevation 逆順で宣言（RFL の #id が最小）。結果は昇順・末尾が最上階。
	Model const model =
		loadIfcFromText("#10=IFCBUILDINGSTOREY('r',$,'RFL',$,$,$,$,$,.ELEMENT.,5973.);\n"
						"#20=IFCBUILDINGSTOREY('a',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCBUILDINGSTOREY('b',$,'2FL',$,$,$,$,$,.ELEMENT.,3273.);\n");
	std::vector<StoryInfo> const stories = collectStories(model);

	CHECK_EQ(stories.size(), static_cast<std::size_t>(3));
	if (stories.size() == 3)
	{
		CHECK(near(stories[0].elevation, 473.0));
		CHECK(!stories[0].isTop);
		CHECK(near(stories[1].elevation, 3273.0));
		CHECK(!stories[1].isTop);
		CHECK(near(stories[2].elevation, 5973.0));
		CHECK(stories[2].isTop);
	}
}

TEST(collect_stories_excludes_non_fl)
{
	// "設計GL" 等 "FL" で終わらないストーリは参照高なので除外する。
	Model const model =
		loadIfcFromText("#10=IFCBUILDINGSTOREY('g',$,'設計GL',$,$,$,$,$,.ELEMENT.,0.);\n"
						"#20=IFCBUILDINGSTOREY('a',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#30=IFCBUILDINGSTOREY('r',$,'RFL',$,$,$,$,$,.ELEMENT.,5973.);\n");
	std::vector<StoryInfo> const stories = collectStories(model);

	CHECK_EQ(stories.size(), static_cast<std::size_t>(2));
	if (stories.size() == 2)
	{
		CHECK(near(stories[0].elevation, 473.0));
		CHECK(near(stories[1].elevation, 5973.0));
		CHECK(stories[1].isTop);
	}
}

TEST(collect_stories_excludes_unnamed_storey)
{
	// Name が $（未設定＝空）の階も "FL" で終わらないため除外する（短名の early-out）。
	Model const model =
		loadIfcFromText("#10=IFCBUILDINGSTOREY('x',$,$,$,$,$,$,$,.ELEMENT.,0.);\n"
						"#20=IFCBUILDINGSTOREY('a',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n");
	std::vector<StoryInfo> const stories = collectStories(model);
	CHECK_EQ(stories.size(), static_cast<std::size_t>(1));
	if (stories.size() == 1)
		CHECK(near(stories[0].elevation, 473.0));
}

TEST(collect_stories_empty_returns_empty)
{
	Model const model = loadIfcFromText("");
	CHECK_EQ(collectStories(model).size(), static_cast<std::size_t>(0));
}

// --------------------------------------------------------------------------
// - buildStoryCommands: 命令の組み立て（M3 は基本レベルのみ）
// ---------------------------------------------------------------------------

TEST(build_commands_for_three_stories)
{
	// 1階（柱 -48）・2階（床版 -36）・屋根。M3 は基本レベルのみ（屋根組・span 柱は後続 M）。
	Model const model =
		loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,-48.));\n"
						"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
						"#12=IFCLOCALPLACEMENT($,#11);\n"
						"#13=IFCCOLUMN('c',$,$,$,$,#12,$,$);\n"
						"#20=IFCCARTESIANPOINT((0.,0.,-36.));\n"
						"#21=IFCAXIS2PLACEMENT3D(#20,$,$);\n"
						"#22=IFCLOCALPLACEMENT($,#21);\n"
						"#23=IFCSLAB('s',$,$,$,$,#22,$,$);\n"
						"#30=IFCBUILDINGSTOREY('a',$,'1FL',$,$,$,$,$,.ELEMENT.,473.);\n"
						"#31=IFCBUILDINGSTOREY('b',$,'2FL',$,$,$,$,$,.ELEMENT.,3273.);\n"
						"#32=IFCBUILDINGSTOREY('r',$,'RFL',$,$,$,$,$,.ELEMENT.,5973.);\n"
						"#40=IFCRELCONTAINEDINSPATIALSTRUCTURE('r1',$,$,$,(#13),#30);\n"
						"#41=IFCRELCONTAINEDINSPATIALSTRUCTURE('r2',$,$,$,(#23),#31);\n");
	std::vector<StoryCommand> const stories = buildStoryCommands(model);

	CHECK_EQ(stories.size(), static_cast<std::size_t>(3));
	if (stories.size() != 3)
		return;

	// 1階: suffix "1"、elevation 473、FL(0,1-FL) + 横架材天端(-48,1-横架材天端)。
	CHECK_EQ(stories[0].name, std::string("1階"));
	CHECK_EQ(stories[0].suffix, std::string("1"));
	CHECK(near(stories[0].elevation, 473.0));
	CHECK_EQ(stories[0].levels.size(), static_cast<std::size_t>(2));
	CHECK_EQ(stories[0].levels[0].type, std::string("FL"));
	CHECK_EQ(stories[0].levels[0].layer, std::string("1-FL"));
	CHECK(near(stories[0].levels[0].offset, 0.0));
	CHECK_EQ(stories[0].levels[1].type, std::string("横架材天端"));
	CHECK_EQ(stories[0].levels[1].layer, std::string("1-横架材天端"));
	CHECK(near(stories[0].levels[1].offset, -48.0));

	// 2階: 床版 -36 が横架材天端オフセット。
	CHECK_EQ(stories[1].name, std::string("2階"));
	CHECK_EQ(stories[1].suffix, std::string("2"));
	CHECK(near(stories[1].levels[1].offset, -36.0));
	CHECK_EQ(stories[1].levels[1].layer, std::string("2-横架材天端"));

	// 屋根: suffix "R"、軒高(0,R-軒高) のみ。
	CHECK_EQ(stories[2].name, std::string("屋根"));
	CHECK_EQ(stories[2].suffix, std::string("R"));
	CHECK(near(stories[2].elevation, 5973.0));
	CHECK_EQ(stories[2].levels.size(), static_cast<std::size_t>(1));
	CHECK_EQ(stories[2].levels[0].type, std::string("軒高"));
	CHECK_EQ(stories[2].levels[0].layer, std::string("R-軒高"));
	CHECK(near(stories[2].levels[0].offset, 0.0));
}

TEST(single_story_treated_as_roof)
{
	Model const model =
		loadIfcFromText("#10=IFCBUILDINGSTOREY('r',$,'RFL',$,$,$,$,$,.ELEMENT.,0.);\n");
	std::vector<StoryCommand> const stories = buildStoryCommands(model);

	CHECK_EQ(stories.size(), static_cast<std::size_t>(1));
	if (stories.size() == 1)
	{
		CHECK_EQ(stories[0].name, std::string("屋根"));
		CHECK_EQ(stories[0].suffix, std::string("R"));
		CHECK(sameVec(levelTypes(stories[0]), std::vector<std::string>{"軒高"}));
	}
}

TEST(build_commands_empty_returns_empty)
{
	Model const model = loadIfcFromText("");
	CHECK_EQ(buildStoryCommands(model).size(), static_cast<std::size_t>(0));
}

// --------------------------------------------------------------------------
// - desiredStoryLayerOrder: 希望レイヤ順の計算（SDK 非依存部）
// ---------------------------------------------------------------------------

TEST(desired_layer_order_grid_top_then_stories_top_down)
{
	// 1階・2階・屋根の基本命令。希望順は "共通" → 最上階→最下階、床(FL)は背面。
	std::vector<StoryCommand> stories = {
		{"1階", "1", 473.0, {{"FL", 0.0, "1-FL"}, {"横架材天端", -48.0, "1-横架材天端"}}},
		{"2階", "2", 3273.0, {{"FL", 0.0, "2-FL"}, {"横架材天端", -36.0, "2-横架材天端"}}},
		{"屋根", "R", 5973.0, {{"軒高", 0.0, "R-軒高"}}},
	};
	std::vector<std::string> const order = desiredStoryLayerOrder(stories);

	const std::vector<std::string> expected = {"共通",		   "R-軒高", "2-横架材天端",
											   "1-横架材天端", "2-FL",	 "1-FL"};
	CHECK_EQ(order.size(), expected.size());
	if (order.size() == expected.size())
	{
		for (std::size_t i = 0; i < expected.size(); ++i)
			CHECK_EQ(order[i], expected[i]);
	}
}

TEST(desired_layer_order_brings_shear_wall_layers_to_the_front)
{
	// 耐力壁レイヤは階をまたいで集めて "共通"／topLayers の直後（最前面群）へ回す。
	// 伏図へ出るのは注記（筋かいの三角・面材の丸）なので、横架材や柱の絵に隠されると
	// 読めない（core::desiredStoryLayerOrder）。階の並び（最上階→最下階）は崩さない。
	std::vector<StoryCommand> stories = {
		{"1階",
		 "1",
		 0.0,
		 {{"FL", 0.0, "1-FL"},
		  {"耐力壁", -48.0, "1-耐力壁"},
		  {"横架材天端", -48.0, "1-横架材天端"}}},
		{"2階",
		 "2",
		 3000.0,
		 {{"FL", 0.0, "2-FL"},
		  {"耐力壁", -36.0, "2-耐力壁"},
		  {"横架材天端", -36.0, "2-横架材天端"}}},
	};
	std::vector<std::string> const order = desiredStoryLayerOrder(stories, {"2-柱伏図記号"});

	const std::vector<std::string> expected = {"共通",	   "2-柱伏図記号", "2-耐力壁",
											   "1-耐力壁", "2-横架材天端", "1-横架材天端",
											   "2-FL",	   "1-FL"};
	CHECK_EQ(order.size(), expected.size());
	if (order.size() == expected.size())
	{
		for (std::size_t i = 0; i < expected.size(); ++i)
			CHECK_EQ(order[i], expected[i]);
	}
}

TEST(desired_layer_order_prepends_top_layers)
{
	// topLayers（伏図記号レイヤ等・ストーリ非依存）は "共通" の直下に積む。
	std::vector<StoryCommand> stories = {
		{"屋根", "R", 0.0, {{"軒高", 0.0, "R-軒高"}}},
	};
	std::vector<std::string> const order = desiredStoryLayerOrder(stories, {"2-柱伏図記号"});

	const std::vector<std::string> expected = {"共通", "2-柱伏図記号", "R-軒高"};
	CHECK_EQ(order.size(), expected.size());
	if (order.size() == expected.size())
	{
		for (std::size_t i = 0; i < expected.size(); ++i)
			CHECK_EQ(order[i], expected[i]);
	}
}

// ---------------------------------------------------------------------------
// 実フィクスチャ（サンプル1）: 3 ストーリ（1階・2階・屋根）を検出する
// ---------------------------------------------------------------------------

TEST(reads_sample_house_fixture)
{
	// サンプル1 のストーリは 設計GL(除外)/1FL/2FL/RFL。→ 1階・2階・屋根 の 3 命令。
	bool ok = false;
	const Model& model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	std::vector<StoryCommand> const stories = buildStoryCommands(model);

	CHECK_EQ(stories.size(), static_cast<std::size_t>(3));
	CHECK(find(stories, "1階") != nullptr);
	CHECK(find(stories, "2階") != nullptr);

	const StoryCommand* roof = find(stories, "屋根");
	CHECK(roof != nullptr);
	if (roof != nullptr)
	{
		CHECK_EQ(roof->suffix, std::string("R"));
		CHECK(near(roof->elevation, 6300.0));
		// 床版の無い屋根に FL は付かない（ロフトの床がある屋根には FL が加わる。下のテスト）。
		// 屋根版（屋根面）はあるので 垂木・野地板 レベルが軒高の直上に積まれ（M6）、母屋・棟木の
		// 命令があるので 母屋 レベルがその下（軒高の直上）に積まれる（M7）。このモデルに登り梁は
		// 無いので 登り梁 レベルは作らない（空レイヤを作らない）。最上段は span 柱レベル
		// （M8）で、屋根階に立つ主屋根の小屋束が "3to3.5-柱"（屋根面で止まる半整数）に入る。
		CHECK(sameVec(levelTypes(*roof),
					  std::vector<std::string>{"3to3.5-柱", "野地板", "垂木", "母屋", "軒高"}));
	}

	// 一般階は FL＋横架材天端の 2 レベル（順序は FL が上）。その上に span 柱レベル（M8）が
	// (from, to) 昇順で積まれる: 1 階には 2 階で止まる管柱（"1to2-柱"）と、3 階床まで届く
	// 通し柱（"1to3-柱"）がある。耐力壁（M19）の命令もあるので、横架材天端の直上に
	// 耐力壁レベルが挟まる。
	const StoryCommand* first = find(stories, "1階");
	if (first != nullptr)
	{
		const std::vector<std::string> base = {"1to2-柱", "1to3-柱", "FL", "耐力壁", "横架材天端"};
		CHECK(sameVec(levelTypes(*first), base));
	}
}

// ---------------------------------------------------------------------------
// 母屋レベル（母屋・棟木の命令がある階にだけ足す。M7）
// ---------------------------------------------------------------------------

TEST(moya_level_only_on_stories_with_moya_member)
{
	// 母屋（"木梁:母屋:…"）を 1 本だけ持つ最上階。母屋レベルが軒高の直上に積まれ、レイヤ
	// "R-母屋" ができる。判定は**組み立てた横架材命令の配置先レイヤ**で、名前判定ではない
	// （parse/Story.cpp の「レベルを足す条件」）。
	const std::string moya = "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
							 "#2=IFCDIRECTION((1.,0.,0.));\n"
							 "#3=IFCAXIS2PLACEMENT3D(#1,#2,$);\n"
							 "#4=IFCLOCALPLACEMENT($,#3);\n"
							 "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,105.,105.);\n"
							 "#6=IFCDIRECTION((1.,0.,0.));\n"
							 "#7=IFCEXTRUDEDAREASOLID(#5,$,#6,1000.);\n"
							 "#8=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#7));\n"
							 "#9=IFCPRODUCTDEFINITIONSHAPE($,$,(#8));\n"
							 "#10=IFCBEAM('b',$,'木梁:母屋:1_1',$,$,#4,#9,$);\n"
							 "#11=IFCBUILDINGSTOREY('s',$,'RFL',$,$,$,$,$,.ELEMENT.,3000.);\n"
							 "#12=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#10),#11);\n";
	const std::vector<StoryCommand> withMoya = buildStoryCommands(loadIfcFromText(moya));
	CHECK_EQ(withMoya.size(), static_cast<std::size_t>(1));
	if (withMoya.size() == 1)
	{
		CHECK(sameVec(levelTypes(withMoya[0]), std::vector<std::string>{"母屋", "軒高"}));
		CHECK_EQ(withMoya[0].levels.front().layer, std::string("R-母屋"));
		// 高さは軒高に揃える（最上階のオフセットは 0）。
		CHECK(near(withMoya[0].levels.front().offset, 0.0));
	}

	// 同じモデルから母屋を外す（軒桁にする）と、母屋レベルは作らない＝空レイヤを作らない。
	std::string girder = moya;
	const std::string::size_type at = girder.find("木梁:母屋:1_1");
	CHECK(at != std::string::npos);
	if (at != std::string::npos)
		girder.replace(at, std::string("木梁:母屋:1_1").size(), "木梁:軒桁:1_1");
	const std::vector<StoryCommand> withoutMoya = buildStoryCommands(loadIfcFromText(girder));
	CHECK_EQ(withoutMoya.size(), static_cast<std::size_t>(1));
	if (withoutMoya.size() == 1)
		CHECK(sameVec(levelTypes(withoutMoya[0]), std::vector<std::string>{"軒高"}));
}

// ---------------------------------------------------------------------------
// 屋根階のロフト床レベル（床版がある屋根にだけ FL を足す）
// ---------------------------------------------------------------------------

TEST(roof_story_gets_fl_level_only_with_floor_slab)
{
	// 屋根階に床版（ロフト＝小屋裏収納の床）があれば、軒高の上に FL（軒高 +36mm）を
	// 足してレイヤ "R-FL" を作る。床版が無ければ軒高だけ（空の FL レイヤを作らない）。
	const std::string base = "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
							 "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
							 "#3=IFCLOCALPLACEMENT($,#2);\n"
							 "#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
							 "#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n";

	Model const without = loadIfcFromText(base);
	std::vector<StoryCommand> const bare = buildStoryCommands(without);
	const StoryCommand* bareRoof = find(bare, "屋根");
	CHECK(bareRoof != nullptr);
	if (bareRoof != nullptr)
		CHECK(sameVec(levelTypes(*bareRoof), std::vector<std::string>{"軒高"}));

	Model const withLoft =
		loadIfcFromText(base + "#40=IFCSLAB('slab',$,'床版',$,$,#3,$,$,$);\n"
							   "#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#40),#11);\n");
	std::vector<StoryCommand> const loft = buildStoryCommands(withLoft);
	const StoryCommand* loftRoof = find(loft, "屋根");
	CHECK(loftRoof != nullptr);
	if (loftRoof != nullptr)
	{
		CHECK(sameVec(levelTypes(*loftRoof), std::vector<std::string>{"FL", "軒高"}));
		CHECK_EQ(loftRoof->levels.front().layer, std::string("R-FL"));
		CHECK(near(loftRoof->levels.front().offset, 36.0));
	}
}

TEST(roof_story_gets_fl_level_from_synthesised_loft_floor)
{
	// ホームズ君はロフトの床版を出力しないので、屋根階の床梁が囲む領域から床を合成する
	// （parse/Floor）。その合成床がある屋根にも FL レベル／レイヤ "R-FL" を作る。
	// 床梁は矩形リング 4 本（外周 ±550・部材幅 100・厚み 100 の鉛直押し出し）。
	std::string text = "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
					   "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
					   "#3=IFCLOCALPLACEMENT($,#2);\n"
					   "#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
					   "#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n"
					   "#33=IFCCARTESIANPOINT((0.,0.,0.));\n"
					   "#34=IFCAXIS2PLACEMENT3D(#33,$,$);\n"
					   "#35=IFCDIRECTION((0.,0.,1.));\n";
	const double bars[4][4] = {{0.0, -500.0, 1100.0, 100.0},
							   {0.0, 500.0, 1100.0, 100.0},
							   {-500.0, 0.0, 100.0, 1100.0},
							   {500.0, 0.0, 100.0, 1100.0}};
	std::string contained;
	int id = 100;
	for (const auto& bar : bars)
	{
		const std::string base = std::to_string(id);
		text += "#" + base + "=IFCCARTESIANPOINT((" + std::to_string(bar[0]) + "," +
				std::to_string(bar[1]) + "));\n";
		text += "#" + std::to_string(id + 1) + "=IFCAXIS2PLACEMENT2D(#" + base + ",$);\n";
		text += "#" + std::to_string(id + 2) + "=IFCRECTANGLEPROFILEDEF(.AREA.,$,#" +
				std::to_string(id + 1) + "," + std::to_string(bar[2]) + "," +
				std::to_string(bar[3]) + ");\n";
		text += "#" + std::to_string(id + 3) + "=IFCEXTRUDEDAREASOLID(#" + std::to_string(id + 2) +
				",#34,#35,100.);\n";
		text += "#" + std::to_string(id + 4) + "=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#" +
				std::to_string(id + 3) + "));\n";
		text += "#" + std::to_string(id + 5) + "=IFCPRODUCTDEFINITIONSHAPE($,$,(#" +
				std::to_string(id + 4) + "));\n";
		text += "#" + std::to_string(id + 6) + "=IFCBEAM('b',$,'木梁:床大梁:1',$,$,#3,#" +
				std::to_string(id + 5) + ",$);\n";
		contained += (contained.empty() ? "#" : ",#") + std::to_string(id + 6);
		id += 10;
	}
	text += "#200=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(" + contained + "),#11);\n";

	std::vector<StoryCommand> const stories = buildStoryCommands(loadIfcFromText(text));
	const StoryCommand* roof = find(stories, "屋根");
	CHECK(roof != nullptr);
	if (roof != nullptr)
	{
		CHECK(sameVec(levelTypes(*roof), std::vector<std::string>{"FL", "軒高"}));
		CHECK_EQ(roof->levels.front().layer, std::string("R-FL"));
	}
}

// ---------------------------------------------------------------------------
// 屋根組（垂木・野地板）レベル: 屋根版のある階にだけ、横架材天端／軒高の直上へ積む（M6）
// ---------------------------------------------------------------------------

namespace
{
	// 1FL / 2FL の 2 階建て。屋根版（Name が "屋根版" 始まりの IfcSlab）を storeyRef
	// （"#10" or "#11"）の階に 1 枚だけ含める。形状表現は不要（レベル追加の判定は名前と
	// 収容関係だけを見る）。
	std::string roofSlabStoryText(const std::string& storeyRef)
	{
		return "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
			   "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
			   "#3=IFCLOCALPLACEMENT($,#2);\n"
			   "#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
			   "#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n"
			   "#40=IFCSLAB('slab',$,'屋根版:1',$,$,#3,$,$,$);\n"
			   "#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#40)," +
			   storeyRef + ");\n";
	}
} // namespace

TEST(roof_frame_levels_only_on_stories_with_roof_slab)
{
	// 屋根版が無い階に 垂木・野地板 レベルは作らない（空レイヤを作らない）。
	Model const without =
		loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
						"#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
						"#3=IFCLOCALPLACEMENT($,#2);\n"
						"#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
						"#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n");
	std::vector<StoryCommand> const bare = buildStoryCommands(without);
	const StoryCommand* bareFirst = find(bare, "1階");
	const StoryCommand* bareRoof = find(bare, "屋根");
	CHECK(bareFirst != nullptr);
	CHECK(bareRoof != nullptr);
	if (bareFirst != nullptr)
		CHECK(sameVec(levelTypes(*bareFirst), std::vector<std::string>{"FL", "横架材天端"}));
	if (bareRoof != nullptr)
		CHECK(sameVec(levelTypes(*bareRoof), std::vector<std::string>{"軒高"}));

	// 最上階（屋根）の主屋根: 軒高の直上に 垂木 → その上に 野地板（スタックは上ほど上段）。
	Model const mainRoof = loadIfcFromText(roofSlabStoryText("#11"));
	std::vector<StoryCommand> const withMain = buildStoryCommands(mainRoof);
	const StoryCommand* roof = find(withMain, "屋根");
	CHECK(roof != nullptr);
	if (roof != nullptr)
	{
		CHECK(sameVec(levelTypes(*roof), std::vector<std::string>{"野地板", "垂木", "軒高"}));
		CHECK_EQ(roof->levels.front().layer, std::string("R-野地板"));
		CHECK_EQ(roof->levels[1].layer, std::string("R-垂木"));
		// 最上階の高さ基準は軒高（オフセット 0）。実描画の Z は要素が絶対値で持つ。
		CHECK(near(roof->levels.front().offset, 0.0));
		CHECK(near(roof->levels[1].offset, 0.0));
	}
}

TEST(roof_frame_levels_on_intermediate_story_shed_roof)
{
	// 中間階に架かる下屋根（下屋）も、その階の 垂木・野地板 レベルを持つ（母屋の有無に
	// 依らず屋根版の有無で判定する）。挿入位置は横架材天端の直上（FL の下）。
	Model const shed = loadIfcFromText(roofSlabStoryText("#10"));
	std::vector<StoryCommand> const stories = buildStoryCommands(shed);
	const StoryCommand* first = find(stories, "1階");
	const StoryCommand* roof = find(stories, "屋根");
	CHECK(first != nullptr);
	if (first != nullptr)
	{
		CHECK(sameVec(levelTypes(*first),
					  std::vector<std::string>{"FL", "野地板", "垂木", "横架材天端"}));
		CHECK_EQ(first->levels[1].layer, std::string("1-野地板"));
		CHECK_EQ(first->levels[2].layer, std::string("1-垂木"));
	}
	// 屋根版を持たない最上階には作らない。
	CHECK(roof != nullptr);
	if (roof != nullptr)
		CHECK(sameVec(levelTypes(*roof), std::vector<std::string>{"軒高"}));
}

TEST(desired_layer_order_sends_roof_sheathing_to_background)
{
	// 野地板レイヤは床（FL）と同じく全ストーリ分をまとめてスタック最下段（背面）へ回す
	// （伏図ビューポートで柱・梁を覆い隠さないため。core::desiredStoryLayerOrder）。
	Model const model = loadIfcFromText(roofSlabStoryText("#11"));
	std::vector<std::string> const order = desiredStoryLayerOrder(buildStoryCommands(model));

	CHECK(!order.empty());
	if (order.empty())
		return;
	CHECK_EQ(order.front(), std::string("共通"));
	// 背面は 野地板 → 床（FL）の順に集まる（最上階→最下階で集めるため R-野地板 が先）。
	CHECK(order.back() == std::string("1-FL"));
	bool sheathingBeforeTaruki = false;
	for (std::size_t i = 0; i < order.size(); ++i)
	{
		if (order[i] == "R-野地板")
		{
			// 野地板は背面（垂木より後ろ）にある。
			for (std::size_t j = 0; j < i; ++j)
			{
				if (order[j] == "R-垂木")
					sheathingBeforeTaruki = true;
			}
		}
	}
	CHECK(sheathingBeforeTaruki);
}

// ---------------------------------------------------------------------------
// span 柱レイヤ名（"{from}to{to}-柱"。組み立てと分解の往復。M8）
// ---------------------------------------------------------------------------

TEST(span_layer_name_integer_levels_have_no_decimal)
{
	CHECK_EQ(spanLayerName(1.0, 2.0), std::string("1to2-柱"));
	CHECK_EQ(spanLayerName(1.0, 3.0), std::string("1to3-柱"));
}

TEST(span_layer_name_half_level_keeps_point_five)
{
	CHECK_EQ(spanLayerName(2.0, 2.5), std::string("2to2.5-柱"));
	CHECK_EQ(spanLayerName(3.0, 3.5), std::string("3to3.5-柱"));
}

TEST(parse_span_layer_round_trips_integer_and_half)
{
	double from = 0.0;
	double to = 0.0;
	CHECK(parseSpanLayer("1to2-柱", from, to));
	CHECK(near(from, 1.0) && near(to, 2.0));
	CHECK(parseSpanLayer("2to2.5-柱", from, to));
	CHECK(near(from, 2.0) && near(to, 2.5));
}

TEST(parse_span_layer_rejects_non_span_layers)
{
	// 柱以外のレイヤ・通り芯・横架材は span レイヤでない。
	double from = 0.0;
	double to = 0.0;
	CHECK(!parseSpanLayer("R-軒高", from, to));
	CHECK(!parseSpanLayer("共通", from, to));
	CHECK(!parseSpanLayer("1to2-梁", from, to));
}

TEST(parse_span_layer_rejects_malformed_core)
{
	// "to" が無い／数値でない core は分解できない。
	double from = 0.0;
	double to = 0.0;
	CHECK(!parseSpanLayer("foo-柱", from, to));
	CHECK(!parseSpanLayer("atob-柱", from, to));
	CHECK(!parseSpanLayer("-柱", from, to));
	// 片側が空（"to" が先頭・末尾）。
	CHECK(!parseSpanLayer("to2-柱", from, to));
	CHECK(!parseSpanLayer("1to-柱", from, to));
	// 数値の後ろに余りがある（数値として全部は読めない）。
	CHECK(!parseSpanLayer("1ato2-柱", from, to));
	CHECK(!parseSpanLayer("1to2x-柱", from, to));
	// "to" が 2 つ以上あると (from, to) に分解できない。
	CHECK(!parseSpanLayer("1to2to3-柱", from, to));
}

// --------------------------------------------------------------------------
// - span 柱レベル（柱の命令がある階にだけ足す。M8）
// ---------------------------------------------------------------------------

TEST(span_column_levels_are_stacked_on_top)
{
	// 1FL に柱（高さ 2844 → 上端 3444。2FL の梁が無いので梁下端は天端＝2FL 高さで代用され、
	// 3444 < 3500 なので直上階へ届かず屋根束扱いの 1to1.5）と、RFL に小屋束を置く。
	// span レベルは levels の**先頭**（FL／軒高の直上＝スタック最上段）に積まれ、レベル種別は
	// レイヤ名そのもの。柱の無い階には span レベルを作らない（空レイヤを作らない）。
	Model const model =
		loadIfcFromText("#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
						"#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
						"#3=IFCLOCALPLACEMENT($,#2);\n"
						"#4=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,105.,105.);\n"
						"#5=IFCDIRECTION((0.,0.,1.));\n"
						"#6=IFCEXTRUDEDAREASOLID(#4,$,#5,2844.);\n"
						"#7=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#6));\n"
						"#8=IFCPRODUCTDEFINITIONSHAPE($,$,(#7));\n"
						"#9=IFCCOLUMN('c',$,$,$,$,#3,#8,$);\n"
						"#10=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,600.);\n"
						"#11=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#9),#10);\n"
						"#12=IFCBUILDINGSTOREY('s',$,'2FL',$,$,$,$,$,.ELEMENT.,3500.);\n");

	std::vector<StoryCommand> const stories = buildStoryCommands(model);
	CHECK_EQ(stories.size(), static_cast<std::size_t>(2));
	if (stories.size() != 2)
		return;
	// 1 階（柱あり）: span レベルが先頭に積まれる。
	CHECK(
		sameVec(levelTypes(stories[0]), std::vector<std::string>{"1to1.5-柱", "FL", "横架材天端"}));
	CHECK_EQ(stories[0].levels.front().layer, std::string("1to1.5-柱"));
	// 2 階（最上階・柱なし）: span レベルは作らない。
	CHECK(sameVec(levelTypes(stories[1]), std::vector<std::string>{"軒高"}));
}

TEST_MAIN()
