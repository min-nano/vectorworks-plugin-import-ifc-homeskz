//
//	CoreDocumentTests.cpp
//
//	命令セット（core/Document）の検証規則 validateDocument の単体テスト。併せて、
//	SDK 非依存のライブラリ（core/ + parse/）が無 SDK のテストハーネスから実際に
//	リンク・実行できること——CMake のターゲット分割（HomeskzIfcCore）が正しく通ること
//	——もここで担保する（buildDocument / Vec* のスモークテスト）。
//
//	この翻訳単位は VectorWorks SDK を一切 include せず、core/Document.h・
//	core/Geometry.h・parse/BuildDocument.h だけに依存する。要素ごとの解析そのものは
//	隣の parse モジュールのテスト（ParseGridTests 等）で検証する。
//

#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/BuildDocument.h"

#include <cmath>
#include <cstddef>

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
		floor.components = {core::ComponentCommand{"床仕上げ", 96.0},
							core::ComponentCommand{"床下地", 24.0}};
		floor.elevation = 0.0;
		floor.bound = core::StoryBoundCommand{0, "FL", 0.0};
		return floor;
	}
} // namespace

TEST(floor_datum_defaults_to_top)
{
	// 既定の高さ基準はスラブ天端（床仕上げ上端）。ロフトだけ Bottom（床下地下端）。
	core::FloorCommand const floor;
	CHECK(floor.datum == core::SlabDatum::Top);
}

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
	for (core::ComponentCommand& component : floor.components)
		component.thickness = 0.0;
	document.floors.push_back(floor);
	CHECK(!core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// 横架材の検証（ROADMAP.md M7）
// ---------------------------------------------------------------------------

namespace
{
	// 検証を通る横架材命令（1 本）。個々のテストはここから 1 か所だけ崩す。
	core::MemberCommand validMember()
	{
		core::MemberCommand member;
		member.layer = "1-横架材天端";
		member.memberId = "120×180 - 杉";
		member.drawClass = "04構造-02木造-04梁桁-03床梁";
		member.start = core::Vec2{0.0, 0.0};
		member.end = core::Vec2{3000.0, 0.0};
		member.width = 120.0;
		member.height = 180.0;
		member.elevation = 383.0;
		member.endElevation = 383.0;
		member.startBound = core::StoryBoundCommand{0, core::kLevelBeamTop, 0.0};
		member.endBound = core::StoryBoundCommand{0, core::kLevelBeamTop, 0.0};
		return member;
	}
} // namespace

TEST(validate_accepts_document_with_valid_member)
{
	core::Document document;
	document.members.push_back(validMember());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_member_with_empty_layer)
{
	core::Document document;
	core::MemberCommand member = validMember();
	member.layer = "";
	document.members.push_back(member);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_member_with_empty_class)
{
	core::Document document;
	core::MemberCommand member = validMember();
	member.drawClass = "";
	document.members.push_back(member);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_member_with_empty_member_id)
{
	core::Document document;
	core::MemberCommand member = validMember();
	member.memberId = "";
	document.members.push_back(member);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_member_with_nonpositive_section)
{
	core::Document zeroWidth;
	core::MemberCommand member = validMember();
	member.width = 0.0;
	zeroWidth.members.push_back(member);
	CHECK(!core::validateDocument(zeroWidth));

	core::Document negativeHeight;
	member = validMember();
	member.height = -1.0;
	negativeHeight.members.push_back(member);
	CHECK(!core::validateDocument(negativeHeight));
}

TEST(validate_rejects_degenerate_member)
{
	// 天端中央線が縮退している（始端＝終端）命令は描けない。
	core::Document document;
	core::MemberCommand member = validMember();
	member.end = member.start;
	document.members.push_back(member);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_member_with_empty_bound_level)
{
	// レベル種別が空だと SetObjectStoryBound が解決できず高さがレイヤ基準へ戻る。
	core::Document startEmpty;
	core::MemberCommand member = validMember();
	member.startBound.level = "";
	startEmpty.members.push_back(member);
	CHECK(!core::validateDocument(startEmpty));

	core::Document endEmpty;
	member = validMember();
	member.endBound.level = "";
	endEmpty.members.push_back(member);
	CHECK(!core::validateDocument(endEmpty));
}

// ---------------------------------------------------------------------------
// 柱の検証（ROADMAP.md M8）
// ---------------------------------------------------------------------------

namespace
{
	// 検証を通る柱命令（1 本）。個々のテストはここから 1 か所だけ崩す。
	core::ColumnCommand validColumn()
	{
		core::ColumnCommand column;
		column.layer = "1to2-柱";
		column.memberId = "105×105 - 管柱";
		column.drawClass = "04構造-02木造-03柱-02管柱";
		column.structuralUse = "4";
		column.position = core::Vec2{0.0, 0.0};
		column.width = 105.0;
		column.depth = 105.0;
		column.height = 2844.0;
		column.elevation = 426.0;
		column.bottomBound = core::StoryBoundCommand{0, core::kLevelBeamTop, 0.0};
		column.topBound = core::StoryBoundCommand{1, core::kLevelBeamTop, -150.0};
		return column;
	}
} // namespace

TEST(validate_accepts_document_with_valid_column)
{
	core::Document document;
	document.columns.push_back(validColumn());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_column_with_empty_layer)
{
	core::Document document;
	core::ColumnCommand column = validColumn();
	column.layer = "";
	document.columns.push_back(column);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_column_with_empty_class_or_member_id)
{
	core::Document emptyClass;
	core::ColumnCommand column = validColumn();
	column.drawClass = "";
	emptyClass.columns.push_back(column);
	CHECK(!core::validateDocument(emptyClass));

	core::Document emptyId;
	column = validColumn();
	column.memberId = "";
	emptyId.columns.push_back(column);
	CHECK(!core::validateDocument(emptyId));
}

TEST(validate_rejects_column_with_empty_structural_use)
{
	// 構造用途が空だと VW の既定（自動）になり、小屋束が柱の高さモデルで描かれてしまう。
	core::Document document;
	core::ColumnCommand column = validColumn();
	column.structuralUse = "";
	document.columns.push_back(column);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_column_with_nonpositive_dimensions)
{
	core::Document zeroWidth;
	core::ColumnCommand column = validColumn();
	column.width = 0.0;
	zeroWidth.columns.push_back(column);
	CHECK(!core::validateDocument(zeroWidth));

	core::Document zeroDepth;
	column = validColumn();
	column.depth = 0.0;
	zeroDepth.columns.push_back(column);
	CHECK(!core::validateDocument(zeroDepth));

	core::Document negativeHeight;
	column = validColumn();
	column.height = -1.0;
	negativeHeight.columns.push_back(column);
	CHECK(!core::validateDocument(negativeHeight));
}

TEST(validate_rejects_column_with_empty_bound_level)
{
	// レベル種別が空だと SetObjectStoryBound が解決できず、高さがレイヤ基準へリセットされる。
	core::Document bottomEmpty;
	core::ColumnCommand column = validColumn();
	column.bottomBound.level = "";
	bottomEmpty.columns.push_back(column);
	CHECK(!core::validateDocument(bottomEmpty));

	core::Document topEmpty;
	column = validColumn();
	column.topBound.level = "";
	topEmpty.columns.push_back(column);
	CHECK(!core::validateDocument(topEmpty));
}

// ---------------------------------------------------------------------------
// 垂木・野地板の検証（ROADMAP.md M6）
// ---------------------------------------------------------------------------

namespace
{
	// 検証を通る垂木命令（1 本）。個々のテストはここから 1 か所だけ崩す。
	core::RafterCommand validRafter()
	{
		core::RafterCommand rafter;
		rafter.layer = "R-垂木";
		rafter.drawClass = "04構造-02木造-05小屋組-05垂木";
		rafter.width = 45.0;
		rafter.height = 45.0;
		rafter.start = core::Vec2{0.0, 0.0};
		rafter.end = core::Vec2{0.0, 3000.0};
		rafter.elevation = 6300.0;
		rafter.endElevation = 7300.0;
		rafter.overhang = 500.0;
		rafter.embedment = 52.5;
		rafter.label = "45×45@455";
		return rafter;
	}

	// 検証を通る野地板命令（1 枚）。
	core::RoofCommand validRoof()
	{
		core::RoofCommand roof;
		roof.layer = "R-野地板";
		roof.drawClass = "04構造-02木造-06耐力面材-03屋根";
		roof.boundary = {core::Vec2{0.0, 0.0}, core::Vec2{4000.0, 0.0}, core::Vec2{4000.0, 3000.0},
						 core::Vec2{0.0, 3000.0}};
		roof.axisStart = core::Vec2{0.0, 0.0};
		roof.axisEnd = core::Vec2{4000.0, 0.0};
		roof.upslope = core::Vec2{0.0, 3000.0};
		roof.rise = 0.316;
		roof.run = 0.949;
		roof.thickness = 12.0;
		roof.elevation = 6350.0;
		return roof;
	}
} // namespace

TEST(validate_accepts_document_with_valid_rafter_and_roof)
{
	core::Document document;
	document.rafters.push_back(validRafter());
	document.roofs.push_back(validRoof());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_rafter_with_empty_layer)
{
	core::Document document;
	core::RafterCommand rafter = validRafter();
	rafter.layer = "";
	document.rafters.push_back(rafter);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_rafter_with_empty_class)
{
	core::Document document;
	core::RafterCommand rafter = validRafter();
	rafter.drawClass = "";
	document.rafters.push_back(rafter);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_rafter_with_nonpositive_section)
{
	// 断面（幅・せい）が 0 以下の垂木は描けない。
	core::Document width;
	core::RafterCommand thin = validRafter();
	thin.width = 0.0;
	width.rafters.push_back(thin);
	CHECK(!core::validateDocument(width));

	core::Document height;
	core::RafterCommand flat = validRafter();
	flat.height = -45.0;
	height.rafters.push_back(flat);
	CHECK(!core::validateDocument(height));
}

TEST(validate_rejects_degenerate_rafter)
{
	// 平面上で始点（支持点）と終点（棟側）が同じ垂木は向き・長さが決まらない。
	core::Document document;
	core::RafterCommand rafter = validRafter();
	rafter.end = rafter.start;
	document.rafters.push_back(rafter);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_roof_with_empty_layer)
{
	core::Document document;
	core::RoofCommand roof = validRoof();
	roof.layer = "";
	document.roofs.push_back(roof);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_roof_with_too_few_boundary_points)
{
	// 3 点未満は面にならない（Python 版 _validate_roof と同じ関門）。
	core::Document document;
	core::RoofCommand roof = validRoof();
	roof.boundary = {core::Vec2{0.0, 0.0}, core::Vec2{4000.0, 0.0}};
	document.roofs.push_back(roof);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_roof_with_nonpositive_thickness)
{
	// 厚み 0 の野地板は実体を持たない（厚みは描画側で ΔZ として与える）。
	core::Document document;
	core::RoofCommand roof = validRoof();
	roof.thickness = 0.0;
	document.roofs.push_back(roof);
	CHECK(!core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// 基礎（立上り＝壁・底盤＝スラブ）の検証（ROADMAP.md M9）
// ---------------------------------------------------------------------------

namespace
{
	// 検証を通る立上り命令（1 本）。個々のテストはここから 1 か所だけ崩す。
	core::WallCommand validWall()
	{
		core::WallCommand wall;
		wall.layer = "F-立上り";
		wall.drawClass = "04構造-01基礎-03立ち上がり";
		wall.start = core::Vec2{0.0, 0.0};
		wall.end = core::Vec2{3640.0, 0.0};
		wall.thickness = 120.0;
		wall.styleName = "基礎立上り - コンクリート 120mm";
		wall.components = {core::ComponentCommand{"コンクリート", 120.0}};
		wall.bottomBound = core::StoryBoundCommand{0, core::kLevelGL, -100.0};
		wall.topBound = core::StoryBoundCommand{1, core::kLevelBeamTop, -190.0};
		return wall;
	}

	// 検証を通る底盤命令（1 枚）。
	core::SlabCommand validSlab()
	{
		core::SlabCommand slab;
		slab.layer = "F-底盤";
		slab.drawClass = "04構造-01基礎-02基礎スラブ";
		slab.boundary = {core::Vec2{0.0, 0.0}, core::Vec2{3640.0, 0.0}, core::Vec2{3640.0, 2730.0},
						 core::Vec2{0.0, 2730.0}};
		slab.styleName = "基礎スラブ - コンクリート 150mm / 捨てコン 30mm / 砕石 100mm";
		slab.components = {core::ComponentCommand{"コンクリート", 150.0},
						   core::ComponentCommand{"捨てコン", 30.0},
						   core::ComponentCommand{"砕石", 100.0}};
		slab.datum = core::SlabDatum::Top;
		slab.thickness = 150.0;
		slab.elevation = 50.0;
		slab.bound = core::StoryBoundCommand{0, core::kLevelSlabTop, 0.0};
		return slab;
	}
} // namespace

TEST(validate_accepts_document_with_valid_wall_and_slab)
{
	core::Document document;
	document.walls.push_back(validWall());
	document.slabs.push_back(validSlab());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_wall_with_empty_layer_or_class)
{
	core::Document layer;
	core::WallCommand wall = validWall();
	wall.layer = "";
	layer.walls.push_back(wall);
	CHECK(!core::validateDocument(layer));

	core::Document drawClass;
	wall = validWall();
	wall.drawClass = "";
	drawClass.walls.push_back(wall);
	CHECK(!core::validateDocument(drawClass));
}

TEST(validate_rejects_wall_with_empty_style_or_no_components)
{
	// 壁スタイル名と構成層は描画側が壁厚ごとのスタイルを作るのに要る（底盤と同じ関門）。
	core::Document style;
	core::WallCommand wall = validWall();
	wall.styleName = "";
	style.walls.push_back(wall);
	CHECK(!core::validateDocument(style));

	core::Document components;
	wall = validWall();
	wall.components.clear();
	components.walls.push_back(wall);
	CHECK(!core::validateDocument(components));
}

TEST(validate_rejects_wall_with_nonpositive_thickness)
{
	// 壁厚 0 の立上りは実体を持たない（CreateWall に渡す厚みがそのまま 0 になる）。
	core::Document document;
	core::WallCommand wall = validWall();
	wall.thickness = 0.0;
	document.walls.push_back(wall);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_degenerate_wall)
{
	// 壁芯の始点と終点が同じ立上りは向き・長さが決まらない。
	core::Document document;
	core::WallCommand wall = validWall();
	wall.end = wall.start;
	document.walls.push_back(wall);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_wall_with_empty_bound_level)
{
	// レベル種別が空だと SetWallOverallHeights が解決できず、高さがレイヤの
	// 「壁の高さ」設定に落ちる（構造材・スラブと同じ関門）。
	core::Document bottomEmpty;
	core::WallCommand wall = validWall();
	wall.bottomBound.level = "";
	bottomEmpty.walls.push_back(wall);
	CHECK(!core::validateDocument(bottomEmpty));

	core::Document topEmpty;
	wall = validWall();
	wall.topBound.level = "";
	topEmpty.walls.push_back(wall);
	CHECK(!core::validateDocument(topEmpty));
}

TEST(validate_rejects_slab_with_empty_layer_class_or_style)
{
	core::Document layer;
	core::SlabCommand slab = validSlab();
	slab.layer = "";
	layer.slabs.push_back(slab);
	CHECK(!core::validateDocument(layer));

	core::Document drawClass;
	slab = validSlab();
	slab.drawClass = "";
	drawClass.slabs.push_back(slab);
	CHECK(!core::validateDocument(drawClass));

	core::Document style;
	slab = validSlab();
	slab.styleName = "";
	style.slabs.push_back(slab);
	CHECK(!core::validateDocument(style));
}

TEST(validate_rejects_slab_with_too_few_boundary_points)
{
	// 3 点未満は面にならない（床板と同じ関門）。
	core::Document document;
	core::SlabCommand slab = validSlab();
	slab.boundary = {core::Vec2{0.0, 0.0}, core::Vec2{3640.0, 0.0}};
	document.slabs.push_back(slab);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_slab_with_nonpositive_thickness)
{
	// コンクリート厚 0 の底盤はスラブスタイルを作れない。
	core::Document document;
	core::SlabCommand slab = validSlab();
	slab.thickness = 0.0;
	document.slabs.push_back(slab);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_slab_with_no_components_or_empty_bound_level)
{
	core::Document components;
	core::SlabCommand slab = validSlab();
	slab.components.clear();
	components.slabs.push_back(slab);
	CHECK(!core::validateDocument(components));

	core::Document bound;
	slab = validSlab();
	slab.bound.level = "";
	bound.slabs.push_back(slab);
	CHECK(!core::validateDocument(bound));
}

// ---------------------------------------------------------------------------
// 基礎の高度化（壁結合・地中梁＝底盤のモディファイア）の検証（ROADMAP.md M10）
// ---------------------------------------------------------------------------

namespace
{
	// 検証を通る壁結合命令（walls に 2 本ある前提）。
	core::WallJoinCommand validJoin()
	{
		core::WallJoinCommand join;
		join.a = 0;
		join.b = 1;
		join.point = core::Vec2{3640.0, 0.0};
		join.pickA = core::Vec2{3400.0, 0.0};
		join.pickB = core::Vec2{3640.0, 240.0};
		join.joinType = core::WallJoinType::L;
		join.capped = false;
		return join;
	}

	// 検証を通る地中梁（台形プリズム）。
	core::ModifierCommand validModifier()
	{
		core::ModifierCommand modifier;
		modifier.profile = {core::Vec2{-150.0, 0.0}, core::Vec2{150.0, 0.0},
							core::Vec2{350.0, 140.0}, core::Vec2{-350.0, 140.0}};
		modifier.depth = 2730.0;
		modifier.origin = core::Vec3{0.0, 0.0, -240.0};
		modifier.azimuth = 0.0;
		return modifier;
	}

	// 立上り 2 本＋壁結合 1 件の Document（結合の検証は walls の本数を見るため対で作る）。
	core::Document documentWithJoin(const core::WallJoinCommand& join)
	{
		core::Document document;
		document.walls.push_back(validWall());
		core::WallCommand second = validWall();
		second.start = core::Vec2{3640.0, 0.0};
		second.end = core::Vec2{3640.0, 2730.0};
		document.walls.push_back(second);
		document.wallJoins.push_back(join);
		return document;
	}
} // namespace

TEST(validate_accepts_document_with_valid_wall_join)
{
	CHECK(core::validateDocument(documentWithJoin(validJoin())));
}

TEST(validate_rejects_wall_join_of_a_wall_with_itself)
{
	core::WallJoinCommand join = validJoin();
	join.b = join.a;
	CHECK(!core::validateDocument(documentWithJoin(join)));
}

TEST(validate_rejects_wall_join_pointing_outside_walls)
{
	// 範囲外の添字は描画側で壁ハンドルを引けず、黙って結合されないだけになるので弾く。
	core::WallJoinCommand join = validJoin();
	join.b = 2; // walls は 2 本（添字 0/1）
	CHECK(!core::validateDocument(documentWithJoin(join)));

	core::Document empty;
	empty.wallJoins.push_back(validJoin()); // 立上りが 1 本も無い
	CHECK(!core::validateDocument(empty));
}

TEST(validate_accepts_slab_with_ground_beam_modifiers)
{
	core::Document document;
	core::SlabCommand slab = validSlab();
	slab.modifiers.push_back(validModifier());
	document.slabs.push_back(slab);
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_degenerate_ground_beam_modifier)
{
	// 断面が面にならない（2 点）・押し出し長が 0 のプリズムは描けない。
	core::Document profile;
	core::SlabCommand slab = validSlab();
	core::ModifierCommand modifier = validModifier();
	modifier.profile.resize(2);
	slab.modifiers.push_back(modifier);
	profile.slabs.push_back(slab);
	CHECK(!core::validateDocument(profile));

	core::Document depth;
	slab = validSlab();
	modifier = validModifier();
	modifier.depth = 0.0;
	slab.modifiers.push_back(modifier);
	depth.slabs.push_back(slab);
	CHECK(!core::validateDocument(depth));
}

// ---------------------------------------------------------------------------
// core::raiseModifierTop（地中梁の可視ソリッドを底盤へ呑み込ませる）
// ---------------------------------------------------------------------------

TEST(raise_modifier_top_extends_along_the_slanted_side)
{
	// 台形の天端（最大 v）だけを bite ぶん上げる。側辺は斜めなので、u も勾配ぶんずらして
	// **側面が実形状の斜面の直線延長**になるようにする（真上へ上げると勾配が変わる）。
	// 下端 (±150, 0) → 天端 (±350, 140) の側辺は「v が 140 増える間に u が 200 増える」
	// ので、bite=10 なら u は 200/140 × 10 ≈ 14.2857 ずれる。
	const core::ModifierCommand raised = core::raiseModifierTop(validModifier(), 10.0);
	CHECK_EQ(raised.profile.size(), std::size_t{4});
	if (raised.profile.size() != 4)
		return;
	// 下端の 2 点は動かない。
	CHECK(raised.profile[0].x == -150.0 && raised.profile[0].y == 0.0);
	CHECK(raised.profile[1].x == 150.0 && raised.profile[1].y == 0.0);
	// 天端の 2 点は v が +10、u は斜辺に沿って外側へ（左右対称）。
	const double expected = 350.0 + (200.0 / 140.0 * 10.0);
	CHECK(std::abs(raised.profile[2].x - expected) < 1e-9);
	CHECK(std::abs(raised.profile[2].y - 150.0) < 1e-9);
	CHECK(std::abs(raised.profile[3].x + expected) < 1e-9);
	CHECK(std::abs(raised.profile[3].y - 150.0) < 1e-9);
	// 断面以外（押し出し長・原点・方位角）はそのまま。
	CHECK(raised.depth == validModifier().depth);
	CHECK(raised.origin.z == validModifier().origin.z);
}

TEST(raise_modifier_top_moves_vertical_sides_straight_up)
{
	// 側辺が鉛直な断面（矩形）は u が変わらず、天端だけが真上へ上がる。
	core::ModifierCommand rectangular = validModifier();
	rectangular.profile = {core::Vec2{-150.0, 0.0}, core::Vec2{150.0, 0.0},
						   core::Vec2{150.0, 140.0}, core::Vec2{-150.0, 140.0}};
	const core::ModifierCommand raised = core::raiseModifierTop(rectangular, 10.0);
	CHECK(raised.profile[2].x == 150.0 && raised.profile[2].y == 150.0);
	CHECK(raised.profile[3].x == -150.0 && raised.profile[3].y == 150.0);
}

TEST(raise_modifier_top_is_a_no_op_without_bite)
{
	// 呑み込み量が 0 以下なら実形状のまま（削り取りモディファイアはこちらを使う）。
	const core::ModifierCommand same = core::raiseModifierTop(validModifier(), 0.0);
	CHECK_EQ(same.profile.size(), validModifier().profile.size());
	CHECK(same.profile[2].y == 140.0);
}

// ---------------------------------------------------------------------------
// シンボル置換系（アンカーボルト・床束・火打・仕口。ROADMAP.md M11）
//
// 4 種は同じ命令型（core::SymbolCommand）なので検証規則も 1 つ。関門は「配置先レイヤ名と
// シンボル名が非空」だけで、位置・角度に値域の制限は無い（角度は正規化しない）。
// ---------------------------------------------------------------------------

namespace
{
	core::SymbolCommand validSymbol()
	{
		core::SymbolCommand symbol;
		symbol.layer = "1-横架材天端";
		symbol.symbol = "仕口";
		symbol.position = core::Vec2{100.0, 200.0};
		symbol.angle = 90.0;
		return symbol;
	}
} // namespace

TEST(validate_accepts_document_with_valid_symbols)
{
	core::Document document;
	document.anchorBolts.push_back(validSymbol());
	document.floorPosts.push_back(validSymbol());
	document.fireBraces.push_back(validSymbol());
	document.joints.push_back(validSymbol());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_symbol_with_empty_layer)
{
	// 配置先レイヤ名が空だと描画側がどのレイヤへ置くか決められない。4 種すべてで同じ関門。
	for (int which = 0; which < 4; ++which)
	{
		core::Document document;
		core::SymbolCommand symbol = validSymbol();
		symbol.layer.clear();
		switch (which)
		{
		case 0:
			document.anchorBolts.push_back(symbol);
			break;
		case 1:
			document.floorPosts.push_back(symbol);
			break;
		case 2:
			document.fireBraces.push_back(symbol);
			break;
		default:
			document.joints.push_back(symbol);
			break;
		}
		CHECK(!core::validateDocument(document));
	}
}

TEST(validate_rejects_symbol_with_empty_name)
{
	// シンボル名が空だと置換対象を引けない（名前でシンボル定義を探すため）。
	core::Document document;
	core::SymbolCommand symbol = validSymbol();
	symbol.symbol.clear();
	document.fireBraces.push_back(symbol);
	CHECK(!core::validateDocument(document));
}

TEST(validate_accepts_symbol_with_any_angle)
{
	// 角度は 0〜360 に正規化しない（負値・360 超も VW 側がそのまま受け取る）。
	core::Document document;
	core::SymbolCommand negative = validSymbol();
	negative.angle = -135.0;
	core::SymbolCommand large = validSymbol();
	large.angle = 720.0;
	document.joints.push_back(negative);
	document.joints.push_back(large);
	CHECK(core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// シート（伏図。ROADMAP.md M13）
//
// 関門は「シートレイヤ番号（＝レイヤ名）とタイトルが非空」「ビューポートが非空のレイヤ名を
// 1 つ以上持つ」の 2 つ。図面タイトル・図番は空でも描ける（ラベルが空になるだけ）ので
// 弾かない。
// ---------------------------------------------------------------------------

namespace
{
	core::SheetCommand validSheet()
	{
		core::SheetCommand sheet;
		sheet.number = "2";
		sheet.title = "1階床伏図";
		sheet.viewport.drawingNumber = "2";
		sheet.viewport.drawingTitle = "1階床伏図";
		sheet.viewport.layers = {"1-横架材天端", "1to2-柱", "1-FL", "共通"};
		return sheet;
	}
} // namespace

TEST(validate_accepts_document_with_valid_sheet)
{
	core::Document document;
	document.sheets.push_back(validSheet());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_sheet_without_number_or_title)
{
	// 番号はシートレイヤ名そのもの・タイトルはシートの説明。どちらも空では作れない。
	core::Document byNumber;
	core::SheetCommand noNumber = validSheet();
	noNumber.number.clear();
	byNumber.sheets.push_back(noNumber);
	CHECK(!core::validateDocument(byNumber));

	core::Document byTitle;
	core::SheetCommand noTitle = validSheet();
	noTitle.title.clear();
	byTitle.sheets.push_back(noTitle);
	CHECK(!core::validateDocument(byTitle));
}

TEST(validate_rejects_sheet_with_no_layers)
{
	// 表示レイヤが 0 枚の伏図＝何も映らないビューポートなので作らせない。
	core::Document document;
	core::SheetCommand sheet = validSheet();
	sheet.viewport.layers.clear();
	document.sheets.push_back(sheet);
	CHECK(!core::validateDocument(document));
}

TEST(validate_rejects_sheet_with_empty_layer_name)
{
	// 名前の無いレイヤは引けない（描画側が黙って読み飛ばすだけになる）ので検証で弾く。
	core::Document document;
	core::SheetCommand sheet = validSheet();
	sheet.viewport.layers.emplace_back();
	document.sheets.push_back(sheet);
	CHECK(!core::validateDocument(document));
}

TEST(validate_accepts_sheet_without_drawing_label)
{
	// 図面タイトル・図番は空でも描ける（ラベルが空になるだけ）。
	core::Document document;
	core::SheetCommand sheet = validSheet();
	sheet.viewport.drawingTitle.clear();
	sheet.viewport.drawingNumber.clear();
	document.sheets.push_back(sheet);
	CHECK(core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// parse::buildDocument（読み込めないパスでは空の Document が返る）
// ---------------------------------------------------------------------------

TEST(build_document_skeleton_returns_valid_empty_document)
{
	// 存在しないファイルでも例外を漏らさず、空だが妥当な Document を返す
	// （実 IFC を読んだときの中身は各 parse モジュールのテストで検証する）。
	core::Document const document = parse::buildDocument("dummy.ifc");
	CHECK_EQ(document.version, core::kDocumentVersion);
	CHECK(core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// core::Geometry（型が使えることの確認。数式そのものは GeometryTests で検証する）
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
