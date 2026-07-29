//
//	CoreDocumentTests.cpp
//
//	SDK 非依存の骨組み（core/ + parse/）が、無 SDK のテストハーネスから実際に
//	リンク・実行できることを確かめるスモークテスト。フォルダ骨組みと CMake の
//	ターゲット分割（SDK 非依存ライブラリ HomeskzIfcCore）が正しく通ることを担保する。
//
//	この翻訳単位は VectorWorks SDK を一切 include せず、core/Document.h・
//	core/Geometry.h・parse/BuildDocument.h だけに依存する。要素の移植が進むにつれ、
//	各 parse モジュールの本格的なテスト（ParseGridTests 等）を隣に足していく。
//

#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/BuildDocument.h"

using namespace HomeskzIfcImport;

// ---------------------------------------------------------------------------
// core::Document / validateDocument
// ---------------------------------------------------------------------------

TEST(empty_document_has_current_version)
{
	core::Document const document;
	CHECK_EQ(document.version, core::kDocumentVersion);
}

TEST(validate_accepts_empty_document)
{
	core::Document const document;
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_unknown_version)
{
	core::Document document;
	document.version = core::kDocumentVersion + 1;
	CHECK(!core::validateDocument(document));
}

TEST(validate_accepts_document_with_valid_grid)
{
	// 健全な通り芯（レイヤ名あり・始点≠終点）は検証を通る。
	core::Document document;
	core::GridCommand grid;
	grid.label = "X1";
	grid.drawClass = "通り芯-X";
	grid.start = core::Vec2{0.0, 0.0};
	grid.end = core::Vec2{0.0, 1000.0};
	document.grids.push_back(grid);
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_degenerate_grid)
{
	// 始点と終点が同じ（縮退した）通り芯は不正 → 描画しない。
	core::Document document;
	core::GridCommand grid;
	grid.start = core::Vec2{5.0, 5.0};
	grid.end = core::Vec2{5.0, 5.0};
	document.grids.push_back(grid);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_grid_with_empty_layer)
{
	// 配置先レイヤ名が空の通り芯は不正 → 描画しない。
	core::Document document;
	core::GridCommand grid;
	grid.layer = "";
	grid.start = core::Vec2{0.0, 0.0};
	grid.end = core::Vec2{0.0, 1000.0};
	document.grids.push_back(grid);
	CHECK(!core::validateDocument(document));
}

namespace
{
	// 検証を通る最小のストーリ命令（名前・接尾辞・1 レベル）。各テストで一部を壊す。
	core::StoryCommand validStory()
	{
		core::StoryCommand story;
		story.name = "1階";
		story.suffix = "1";
		story.elevation = 473.0;
		story.levels.push_back(core::LevelCommand{"FL", 0.0, "1-FL"});
		return story;
	}
} // namespace

TEST(validate_accepts_document_with_valid_story)
{
	// 名前・接尾辞・各レベルの種別/レイヤ名が非空なら通る（isValidStory / isValidLevel）。
	core::Document document;
	document.stories.push_back(validStory());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_story_with_empty_name)
{
	core::Document document;
	core::StoryCommand story = validStory();
	story.name = "";
	document.stories.push_back(story);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_story_with_empty_suffix)
{
	// 空 suffix は VW 2026 で 2 回目以降の CreateStory が失敗するため不正。
	core::Document document;
	core::StoryCommand story = validStory();
	story.suffix = "";
	document.stories.push_back(story);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_story_level_with_empty_type)
{
	core::Document document;
	core::StoryCommand story = validStory();
	story.levels.push_back(core::LevelCommand{"", 0.0, "1-横架材天端"});
	document.stories.push_back(story);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_story_level_with_empty_layer)
{
	core::Document document;
	core::StoryCommand story = validStory();
	story.levels.push_back(core::LevelCommand{"横架材天端", 0.0, ""});
	document.stories.push_back(story);
	CHECK(!core::validateDocument(document));
}

namespace
{
	// 検証を通る最小の床命令（レイヤ・クラス・3 点以上の外形・構成層・高さ基準）。
	core::FloorCommand validFloor()
	{
		core::FloorCommand floor;
		floor.layer = "1-FL";
		floor.drawClass = "04構造-02木造-06耐力面材-02床";
		floor.boundary = {core::Vec2{0.0, 0.0}, core::Vec2{1000.0, 0.0}, core::Vec2{1000.0, 2000.0},
						  core::Vec2{0.0, 2000.0}};
		floor.styleName = "1F-床スタイル";
		floor.components = {core::SlabComponentCommand{"床仕上げ", 96.0},
							core::SlabComponentCommand{"床下地", 24.0}};
		floor.elevation = 0.0;
		floor.bound = core::StoryBoundCommand{0, "FL", 0.0};
		return floor;
	}
} // namespace

TEST(validate_accepts_document_with_valid_floor)
{
	core::Document document;
	document.floors.push_back(validFloor());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_floor_with_empty_layer)
{
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.layer = "";
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_floor_with_empty_class)
{
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.drawClass = "";
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_floor_with_too_few_boundary_points)
{
	// 3 点未満は面にならない（Python 版 _validate_floor と同じ関門）。
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.boundary = {core::Vec2{0.0, 0.0}, core::Vec2{1000.0, 0.0}};
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_floor_with_empty_bound_level)
{
	// レベル種別が空だと SetObjectStoryBound がバインド先を決められない。
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.bound.level = "";
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_floor_with_empty_style_name)
{
	// スラブスタイル名が空だと構成を持つスタイルを用意できない。
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.styleName = "";
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_floor_without_components)
{
	// 構成層が無いスラブは厚みを持てない。
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.components.clear();
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_floor_component_with_empty_name)
{
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.components[0].name = "";
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_floor_component_with_negative_thickness)
{
	// 負の層は作れない（parse 側は 0 に丸める）。
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.components[0].thickness = -1.0;
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

TEST(validate_accepts_floor_with_zero_thickness_finish)
{
	// 床仕上げが 0（FL − 横架材天端 が床下地厚以下）の床も、総厚が正なら妥当。
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.components[0].thickness = 0.0;
	document.floors.push_back(floor);
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_floor_with_zero_total_thickness)
{
	// 総厚 0 のスラブは実体を持たない。
	core::Document document;
	core::FloorCommand floor = validFloor();
	for (core::SlabComponentCommand& component : floor.components)
		component.thickness = 0.0;
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// parse::buildDocument（骨組み: いまは空の Document を返すだけ）
// ---------------------------------------------------------------------------

TEST(build_document_skeleton_returns_valid_empty_document)
{
	core::Document const document = parse::buildDocument("dummy.ifc");
	CHECK_EQ(document.version, core::kDocumentVersion);
	CHECK(core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// core::Geometry（骨組みの最小型が使えることの確認）
// ---------------------------------------------------------------------------

TEST(geometry_vectors_default_to_origin)
{
	core::Vec2 const p2;
	core::Vec3 const p3;
	CHECK_EQ(p2.x, 0.0);
	CHECK_EQ(p2.y, 0.0);
	CHECK_EQ(p3.x, 0.0);
	CHECK_EQ(p3.y, 0.0);
	CHECK_EQ(p3.z, 0.0);
}

TEST_MAIN();
