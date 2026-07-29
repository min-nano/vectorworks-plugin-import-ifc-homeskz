//
//	ParseStoryTests.cpp
//
//	ストーリ解析（src/parse/Story）＋希望レイヤ順の計算（src/core/desiredStoryLayerOrder）の
//	単体テスト。VectorWorks SDK を一切 include せず、無 SDK のテストハーネス
//	（TestFramework.h）で走る（CLAUDE.md「テスト方針」: core/ parse/ は無 SDK で単体
//	テスト）。Python 版 test_ifc_story.py の意図を写す。
//
//	検証項目（ROADMAP.md M3）: ローカル配置 Z 抽出・横架材天端オフセット（列挙順に
//	依存しない最大負値）・ストーリ収集（Elevation 昇順・最上階判定・非 FL 除外）・
//	story 命令の組み立て（一般階=FL＋横架材天端、最上階=軒高。M3 は基本レベルのみ）・
//	希望レイヤ順（"共通" 先頭・最上階→最下階・床は背面）・決定性。実フィクスチャのパスは
//	CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Loader.h"
#include "parse/Story.h"

#include <cmath>
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
using HomeskzIfcImport::parse::resolveBeamTopOffset;
using HomeskzIfcImport::parse::StoryInfo;

namespace
{
	// 2 つの実数が許容誤差内で等しいか。オフセット・高さ比較に使う。
	bool near(double a, double b)
	{
		return std::abs(a - b) < 1e-9;
	}

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

// ---------------------------------------------------------------------------
// getLocalPlacementZ: ローカル配置 Z の抽出
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

// ---------------------------------------------------------------------------
// collectStoryElements: 階に属する要素の列挙
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

// ---------------------------------------------------------------------------
// resolveBeamTopOffset: 横架材天端オフセット
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
	// 列挙順に依らない決定性（Python 版 test_returns_maximum_offset_regardless_of_order）。
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

// ---------------------------------------------------------------------------
// collectStories: Elevation 昇順・最上階判定・非 FL 除外
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

// ---------------------------------------------------------------------------
// buildStoryCommands: 命令の組み立て（M3 は基本レベルのみ）
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

// ---------------------------------------------------------------------------
// desiredStoryLayerOrder: 希望レイヤ順の計算（SDK 非依存部）
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
	Model const model = HomeskzIfcImport::parse::loadIfc(
		std::string(HOMESKZ_FIXTURES_DIR) + "/サンプル1 (住木邸新築工事).ifc", &ok);
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
		CHECK(sameVec(levelTypes(*roof), std::vector<std::string>{"軒高"}));
	}

	// 一般階は FL＋横架材天端の 2 レベル（順序は FL が上）。
	const StoryCommand* first = find(stories, "1階");
	if (first != nullptr)
	{
		const std::vector<std::string> base = {"FL", "横架材天端"};
		CHECK(sameVec(levelTypes(*first), base));
	}
}

TEST_MAIN()
