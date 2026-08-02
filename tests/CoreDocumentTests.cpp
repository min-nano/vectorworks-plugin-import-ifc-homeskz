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
	for (core::SlabComponentCommand& component : floor.components)
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
