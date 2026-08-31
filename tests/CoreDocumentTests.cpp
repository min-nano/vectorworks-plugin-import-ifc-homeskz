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

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/BuildDocument.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcTests::near;

// --------------------------------------------------------------------------
// - core::Document / validateDocument
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
		floor.components = {core::ComponentCommand{"床仕上げ", "z構成要素-フローリング", 96.0},
							core::ComponentCommand{"床下地", "z構成要素-合板", 24.0}};
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
	// 3 点未満は面にならない。
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

TEST(validate_rejects_floor_component_with_empty_class)
{
	// 層のクラスは描画属性をクラス属性に従わせる唯一の手掛かりなので、空は通さない。
	core::Document document;
	core::FloorCommand floor = validFloor();
	floor.components[0].drawClass = "";
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
// 横架材の検証（docs/DEV-NOTES.md M7）
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
// 柱の検証（docs/DEV-NOTES.md M8）
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
// 垂木・野地板の検証（docs/DEV-NOTES.md M6）
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
		// 高さ基準は垂木レベル。支持点は横架材天端ちょうど（offset 0）で、棟側は勾配ぶん高い。
		rafter.startBound = core::StoryBoundCommand{0, core::kLevelTaruki, 0.0};
		rafter.endBound = core::StoryBoundCommand{0, core::kLevelTaruki, 1000.0};
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

TEST(validate_rejects_rafter_without_bound_levels)
{
	// 構造材ツールは両端をストーリレベルへバインドして高さを決める（draw/Rafter）。
	// レベル種別が空だと高さが崩れるので、横架材・柱と同じく命令の段階で弾く。
	core::Document start;
	core::RafterCommand noStart = validRafter();
	noStart.startBound.level = "";
	start.rafters.push_back(noStart);
	CHECK(!core::validateDocument(start));

	core::Document end;
	core::RafterCommand noEnd = validRafter();
	noEnd.endBound.level = "";
	end.rafters.push_back(noEnd);
	CHECK(!core::validateDocument(end));
}

// ---------------------------------------------------------------------------
// - core::rafterEaveEnd（垂木の軒先側の材端＝構造材ツールのパスの始端）
// ---------------------------------------------------------------------------

TEST(rafter_eave_end_extends_beyond_the_support_point)
{
	// 支持点 (0, 0, 6300) から棟 (0, 3000, 7300) へ 3000mm で 1000mm 上る勾配。軒側へは
	// 差し込み 52.5 ＋ 軒の出 500 ＝ 552.5mm 出るので、軒先は Y が −552.5、Z は勾配
	// 1000/3000 ぶん下がって 6300 − 184.1666… になる。offset も同じだけ下がる。
	const core::RafterEaveEnd eave = core::rafterEaveEnd(validRafter());
	const double drop = 1000.0 / 3000.0 * 552.5;
	CHECK(std::abs(eave.point.x - 0.0) < 1e-9);
	CHECK(std::abs(eave.point.y + 552.5) < 1e-9);
	CHECK(std::abs(eave.z - (6300.0 - drop)) < 1e-9);
	CHECK(std::abs(eave.offset - (0.0 - drop)) < 1e-9);
}

TEST(rafter_eave_end_is_the_support_point_without_reach)
{
	// 軒桁に乗らない垂木は差し込みも軒の出も 0（start がそのまま軒先。parse/Rafter）。
	core::RafterCommand rafter = validRafter();
	rafter.overhang = 0.0;
	rafter.embedment = 0.0;
	const core::RafterEaveEnd eave = core::rafterEaveEnd(rafter);
	CHECK(eave.point.x == rafter.start.x && eave.point.y == rafter.start.y);
	CHECK(eave.z == rafter.elevation);
	CHECK(eave.offset == rafter.startBound.offset);
}

TEST(rafter_eave_end_keeps_the_offset_relative_to_the_support_point)
{
	// startBound.offset が 0 でない（軒桁に乗らず軒先が基準レベルより下にある）垂木でも、
	// 返る offset は「支持点の offset − 下がったぶん」。レベルは共通なので差だけで足りる。
	core::RafterCommand rafter = validRafter();
	rafter.startBound.offset = -120.0;
	const core::RafterEaveEnd eave = core::rafterEaveEnd(rafter);
	const double drop = 1000.0 / 3000.0 * 552.5;
	CHECK(std::abs(eave.offset - (-120.0 - drop)) < 1e-9);
	// 下面 Z（絶対値）は startBound とは独立に elevation から下がる。
	CHECK(std::abs(eave.z - (6300.0 - drop)) < 1e-9);
}

TEST(rafter_eave_end_follows_the_plan_direction)
{
	// 軒先は「棟へ向かう向きの逆」。X 方向に架かる垂木では X が減る側へ出る。
	core::RafterCommand rafter = validRafter();
	rafter.start = core::Vec2{1000.0, 500.0};
	rafter.end = core::Vec2{5000.0, 500.0};
	rafter.elevation = 3000.0;
	rafter.endElevation = 3000.0; // 水平（勾配 0）なら Z は下がらない
	const core::RafterEaveEnd eave = core::rafterEaveEnd(rafter);
	CHECK(std::abs(eave.point.x - (1000.0 - 552.5)) < 1e-9);
	CHECK(std::abs(eave.point.y - 500.0) < 1e-9);
	CHECK(std::abs(eave.z - 3000.0) < 1e-9);
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
	// 3 点未満は面にならない。
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
// 基礎（立上り＝壁・底盤＝スラブ）の検証（docs/DEV-NOTES.md M9）
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
		wall.components = {core::ComponentCommand{"コンクリート", "z構成要素-コンクリート", 120.0}};
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
		// 底盤の下は砕石 1 層（厚みは捨てコン + 砕石ぶん。M17）。
		slab.components = {core::ComponentCommand{"コンクリート", "z構成要素-コンクリート", 150.0},
						   core::ComponentCommand{"砕石", "z構成要素-砕石", 130.0}};
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

TEST(validate_rejects_wall_without_components)
{
	// 構成層は描画側が壁へ直接組む（合計＝壁厚）ので、無い壁は描けない（底盤と同じ関門）。
	core::Document components;
	core::WallCommand wall = validWall();
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

TEST(validate_rejects_slab_with_empty_layer_or_class)
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
	// コンクリート厚 0 の底盤は構成層のコンクリート層を持てない。
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
// 基礎の高度化（壁結合・地中梁＝底盤のモディファイア）の検証（docs/DEV-NOTES.md M10）
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

TEST(validate_checks_the_ground_beam_bedding)
{
	// 床付け（捨てコン・砕石）は地中梁と押し出しの向き・断面の座標系を共有するので、断面が
	// 面になること・素材クラス名が非空であること・押し出し長が正であることだけを見る。
	core::Document document;
	core::SlabCommand slab = validSlab();
	core::ModifierCommand modifier = validModifier();
	modifier.beddings.push_back(
		core::BeddingCommand{{core::Vec2{-150.0, -130.0}, core::Vec2{150.0, -130.0},
							  core::Vec2{150.0, 0.0}, core::Vec2{-150.0, 0.0}},
							 "z構成要素-砕石",
							 0.0,
							 modifier.depth});
	slab.modifiers.push_back(modifier);
	document.slabs.push_back(slab);
	CHECK(core::validateDocument(document));

	core::Document noClass;
	core::ModifierCommand bad = modifier;
	bad.beddings[0].drawClass = "";
	slab = validSlab();
	slab.modifiers.push_back(bad);
	noClass.slabs.push_back(slab);
	CHECK(!core::validateDocument(noClass));

	core::Document degenerate;
	bad = modifier;
	bad.beddings[0].profile.resize(2);
	slab = validSlab();
	slab.modifiers.push_back(bad);
	degenerate.slabs.push_back(slab);
	CHECK(!core::validateDocument(degenerate));

	core::Document noDepth;
	bad = modifier;
	bad.beddings[0].depth = 0.0;
	slab = validSlab();
	slab.modifiers.push_back(bad);
	noDepth.slabs.push_back(slab);
	CHECK(!core::validateDocument(noDepth));
}

// --------------------------------------------------------------------------
// - core::raiseModifierTop（地中梁の可視ソリッドを底盤へ呑み込ませる）
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
// シンボル置換系（アンカーボルト・床束・火打・仕口。docs/DEV-NOTES.md M11）
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
// シート（伏図。docs/DEV-NOTES.md M13）
//
// 関門は「シートレイヤ番号（＝レイヤ名）とタイトルが非空」「ビューポートが非空のレイヤ名を
// 1 つ以上持つ」の 2 つ。図面タイトル・図番は空でも描ける（ラベルが空になるだけ）ので弾かな
// い。**グラフィック凡例は載せるか載せないかしか持たない**（スタイル名を持たない＝スタイル
// 無しで置く。core/Document.h の LegendCommand）ので、凡例に関門は無い。
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

TEST(validate_accepts_sheet_with_legend)
{
	// グラフィック凡例（M13）は配置点しか持たない（スタイル無しで置き、並ぶ中身は凡例
	// オブジェクト自身のソース定義が決めるので命令には現れない。core/Document.h の
	// LegendCommand）。したがって既定構築の凡例を載せた伏図も妥当。
	core::Document document;
	core::SheetCommand sheet = validSheet();
	sheet.legend = core::LegendCommand{};
	document.sheets.push_back(sheet);
	CHECK(core::validateDocument(document));
}

TEST(validate_accepts_sheet_without_legend)
{
	// 凡例を載せない伏図（アンカーボルトを 1 本も置かなかった基礎伏図）も妥当。
	core::Document document;
	core::SheetCommand sheet = validSheet();
	CHECK(!sheet.legend.has_value());
	document.sheets.push_back(sheet);
	CHECK(core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// 断面ビューポート（軸組図。docs/DEV-NOTES.md M14）
//
// 非空の表示レイヤに加えて、**指示線が縮退していない**ことを見る（縮退した線からは切断面の
// 向きが決まらない）。断面の範囲（長さ・高さ・奥行き）は命令が持たない——軸組図は範囲を
// 限らないので、描画側の定数が受け持つ。**配置先のシートレイヤも命令は持たない**（M18）
// ——何枚の用紙に分かれるかは用紙の大きさと縮尺が決めるので、文書に 1 つの
// SectionSheetCommand（番号の始まり・タイトルの基）だけを見る。
// ---------------------------------------------------------------------------

namespace
{
	core::SectionCommand validSection()
	{
		core::SectionCommand section;
		section.direction = core::SectionDirection::X;
		section.lineStart = core::Vec2{1000.0, -4000.0};
		section.lineEnd = core::Vec2{1000.0, 4000.0};
		section.viewPoint = core::Vec2{0.0, 0.0};
		section.viewport.drawingNumber = "X1";
		section.viewport.drawingTitle = "X1通り";
		section.viewport.layers = {"1-横架材天端", "1-FL", "共通"};
		return section;
	}
} // namespace

namespace
{
	// 軸組図を 1 枚持つ妥当な文書（シートレイヤの通し方まで埋めたもの）。
	core::Document documentWithSection()
	{
		core::Document document;
		document.sections.push_back(validSection());
		document.sectionSheet.startNumber = 8;
		document.sectionSheet.title = "軸組図";
		return document;
	}
} // namespace

TEST(validate_accepts_document_with_valid_section)
{
	CHECK(core::validateDocument(documentWithSection()));
}

TEST(validate_rejects_section_without_layers)
{
	// 表示レイヤが 0 枚の軸組図は「何も映らないビューポート」なので作らせない（伏図と同じ）。
	core::Document byLayers = documentWithSection();
	byLayers.sections.front().viewport.layers.clear();
	CHECK(!core::validateDocument(byLayers));
}

TEST(validate_rejects_section_sheet_without_number_or_title)
{
	// 軸組図があるなら、配置先シートレイヤの通し方（番号の始まり・タイトルの基）も
	// 埋まっていること（M18）。
	core::Document byNumber = documentWithSection();
	byNumber.sectionSheet.startNumber = 0;
	CHECK(!core::validateDocument(byNumber));

	core::Document byTitle = documentWithSection();
	byTitle.sectionSheet.title.clear();
	CHECK(!core::validateDocument(byTitle));

	// **軸組図が 1 枚も無ければ見ない**（使わない値なので空のままでも妥当）。
	core::Document empty;
	CHECK(core::validateDocument(empty));
}

TEST(validate_rejects_section_with_degenerate_line)
{
	// 始点＝終点では切断面の向きが決まらない。
	core::Document document = documentWithSection();
	document.sections.front().lineEnd = document.sections.front().lineStart;
	CHECK(!core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// 断面寸法データタグ（ビューポート注釈。docs/DEV-NOTES.md M13）
//
// タグはビューポート命令の中に住む（伏図・軸組図で同じ形）。関門は「スタイル名が非空」と
// 「関連付け先の横架材が members の範囲内」の 2 つ——範囲外の添字はどの部材にも付かない
// タグ＝寸法の出ない空のタグが図面に残るので描かせない。
// ---------------------------------------------------------------------------

namespace
{
	// 横架材 1 本だけを持つ文書（タグの関連付け先。添字 0 だけが有効になる）。
	core::Document documentWithOneMember()
	{
		core::Document document;
		core::MemberCommand member;
		member.layer = "1-横架材天端";
		member.memberId = "120×180";
		member.drawClass = "04構造-02木造-04床組-03床梁";
		member.start = core::Vec2{0.0, 0.0};
		member.end = core::Vec2{2000.0, 0.0};
		member.width = 120.0;
		member.height = 180.0;
		member.elevation = 3000.0;
		member.endElevation = 3000.0;
		member.startBound.level = "横架材天端";
		member.endBound.level = "横架材天端";
		document.members.push_back(member);
		return document;
	}

	core::TagCommand validTag()
	{
		core::TagCommand tag;
		tag.memberIndex = 0;
		tag.position = core::Vec2{1000.0, 60.0};
		tag.angle = 0.0;
		return tag;
	}
} // namespace

TEST(validate_accepts_viewport_tags_on_sheets_and_sections)
{
	core::Document document = documentWithOneMember();
	core::SheetCommand sheet = validSheet();
	sheet.viewport.tags.push_back(validTag());
	document.sheets.push_back(sheet);

	core::SectionCommand section = validSection();
	section.viewport.tags.push_back(validTag());
	document.sections.push_back(section);
	// 軸組図があるならシートレイヤの通し方も埋まっていること（M18）。
	document.sectionSheet.startNumber = 8;
	document.sectionSheet.title = "軸組図";

	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_tag_pointing_past_the_members)
{
	// 伏図でも軸組図でも、範囲外の添字は同じ規則で弾く。
	core::Document bySheet = documentWithOneMember();
	core::SheetCommand sheet = validSheet();
	core::TagCommand outOfRange = validTag();
	outOfRange.memberIndex = 1; // 横架材は 1 本（有効な添字は 0 だけ）
	sheet.viewport.tags.push_back(outOfRange);
	bySheet.sheets.push_back(sheet);
	CHECK(!core::validateDocument(bySheet));

	core::Document bySection = documentWithOneMember();
	core::SectionCommand section = validSection();
	section.viewport.tags.push_back(outOfRange);
	bySection.sections.push_back(section);
	CHECK(!core::validateDocument(bySection));
}

// --------------------------------------------------------------------------
// - sectionHeightRange（軸組図の高さ範囲。docs/DEV-NOTES.md M14）
//
// 断面ビューポートの高さ範囲は CreateSectionViewport の引数でしか与えられず、SDK に
// 「無限」を指定する手段が無い。そこで取り込んだ要素の Z から建物を包む範囲を求める。
// ---------------------------------------------------------------------------

TEST(section_height_range_wraps_elements_with_margin)
{
	core::Document document;
	// 柱 0〜3000、横架材の天端 3000（せい 180 なので下端 2820）。範囲は上下に余白ぶん広い。
	core::ColumnCommand column;
	column.elevation = 0.0;
	column.height = 3000.0;
	document.columns.push_back(column);

	core::MemberCommand member;
	member.elevation = 3000.0;
	member.endElevation = 3000.0;
	member.height = 180.0;
	document.members.push_back(member);

	double start = 0.0;
	double end = 0.0;
	CHECK(core::sectionHeightRange(document, start, end));
	CHECK(std::abs(start - (0.0 - core::kSectionHeightMargin)) < 1e-6);
	CHECK(std::abs(end - (3000.0 + core::kSectionHeightMargin)) < 1e-6);
}

TEST(section_height_range_covers_floors_roofs_slabs_and_stories)
{
	core::Document document;
	// 基礎の底盤（天端 50・厚 150 → 下端 −100）と屋根（軒 6000）で上下が決まる。
	core::SlabCommand slab;
	slab.elevation = 50.0;
	slab.thickness = 150.0;
	document.slabs.push_back(slab);

	core::RoofCommand roof;
	roof.elevation = 6000.0;
	document.roofs.push_back(roof);

	// ストーリ高さも範囲に入る（要素の無い階を切り落とさない）。
	core::StoryCommand story;
	story.elevation = 3000.0;
	document.stories.push_back(story);

	double start = 0.0;
	double end = 0.0;
	CHECK(core::sectionHeightRange(document, start, end));
	CHECK(std::abs(start - (-100.0 - core::kSectionHeightMargin)) < 1e-6);
	CHECK(std::abs(end - (6000.0 + core::kSectionHeightMargin)) < 1e-6);
}

TEST(section_height_range_reaches_ground_beam_bottom)
{
	core::Document document;
	// 底盤（天端 50・厚 150 → 下端 −100）に、そこから更に深く垂れ下がる地中梁を 1 本。
	// **モデルの最深部は底盤の下端ではなく地中梁の下端**なので、範囲はそこまで届く。
	core::SlabCommand slab;
	slab.elevation = 50.0;
	slab.thickness = 150.0;

	// 台形断面（v=0 が梁下端）。origin.z＝梁下端の絶対 Z なので、下端 −700・上端 −100。
	core::ModifierCommand modifier;
	modifier.origin = core::Vec3{0.0, 0.0, -700.0};
	modifier.depth = 2000.0;
	modifier.profile = {core::Vec2{-75.0, 0.0}, core::Vec2{75.0, 0.0}, core::Vec2{150.0, 600.0},
						core::Vec2{-150.0, 600.0}};
	slab.modifiers.push_back(modifier);
	document.slabs.push_back(slab);

	double start = 0.0;
	double end = 0.0;
	CHECK(core::sectionHeightRange(document, start, end));
	CHECK(std::abs(start - (-700.0 - core::kSectionHeightMargin)) < 1e-6);
	CHECK(std::abs(end - (50.0 + core::kSectionHeightMargin)) < 1e-6);

	// 床付け（捨てコン・砕石）を敷くと**最深部はその下端**になる（梁下端から更に 130mm 下）。
	document.slabs.front().modifiers.front().beddings.push_back(
		core::BeddingCommand{{core::Vec2{-75.0, -130.0}, core::Vec2{75.0, -130.0},
							  core::Vec2{75.0, 0.0}, core::Vec2{-75.0, 0.0}},
							 "z構成要素-砕石",
							 0.0,
							 2000.0});
	CHECK(core::sectionHeightRange(document, start, end));
	CHECK(std::abs(start - (-830.0 - core::kSectionHeightMargin)) < 1e-6);
}

TEST(section_height_range_covers_floors_and_rafters)
{
	core::Document document;
	// 床は**基準面と構成層の合計だけ下がった下端**の両方を見る（合計 12+150=162 なので
	// 基準面 3000 の床の下端は 2838）。ここが範囲の下端になる。
	core::FloorCommand floor;
	floor.elevation = 3000.0;
	floor.components.push_back(core::ComponentCommand{"仕上げ", "z構成要素-フローリング", 12.0});
	floor.components.push_back(core::ComponentCommand{"床下地", "z構成要素-合板", 150.0});
	document.floors.push_back(floor);

	// 垂木は勾配があるので**両端**を見る（軒 5000・棟 7000）。棟が範囲の上端になる。
	core::RafterCommand rafter;
	rafter.elevation = 5000.0;
	rafter.endElevation = 7000.0;
	document.rafters.push_back(rafter);

	double start = 0.0;
	double end = 0.0;
	CHECK(core::sectionHeightRange(document, start, end));
	CHECK(std::abs(start - (2838.0 - core::kSectionHeightMargin)) < 1e-6);
	CHECK(std::abs(end - (7000.0 + core::kSectionHeightMargin)) < 1e-6);
}

TEST(section_height_range_fails_without_elements)
{
	// 高さの分かる要素が 1 つも無ければ範囲は求まらない（out は触らない）。
	double start = -1.0;
	double end = -2.0;
	CHECK(!core::sectionHeightRange(core::Document{}, start, end));
	CHECK(std::abs(start - (-1.0)) < 1e-6);
	CHECK(std::abs(end - (-2.0)) < 1e-6);
}

// ---------------------------------------------------------------------------
// 平面の広がり（伏図の縮尺と位置を決めるのに使う。docs/DEV-NOTES.md M18）
//
// planContentBounds は「図に映るもの」を包む矩形を返す。layers を渡すとそのレイヤに載る
// 命令だけを見る（伏図 1 枚が映す範囲）。sectionContentSize は軸組図 1 枚ぶんの広がり
// （幅＝平面の広がりの大きい方・高さ＝断面の高さ範囲）。
// ---------------------------------------------------------------------------

TEST(plan_content_bounds_wraps_every_command_with_margin)
{
	core::Document document;
	// 通り芯（−4000〜4000）と、その外へ出る野地板の軒（5000）。
	core::GridCommand grid;
	grid.layer = core::kGridLayer;
	grid.start = core::Vec2{-4000.0, -3000.0};
	grid.end = core::Vec2{4000.0, -3000.0};
	document.grids.push_back(grid);

	core::RoofCommand roof;
	roof.layer = "1-野地板";
	roof.drawClass = "野地板";
	roof.boundary = {core::Vec2{-5000.0, -4000.0}, core::Vec2{5000.0, -4000.0},
					 core::Vec2{5000.0, 4000.0}, core::Vec2{-5000.0, 4000.0}};
	document.roofs.push_back(roof);

	core::Vec2 min;
	core::Vec2 max;
	CHECK(core::planContentBounds(document, {}, min, max));
	CHECK(near(min.x, -5000.0 - core::kPlanContentMargin));
	CHECK(near(max.x, 5000.0 + core::kPlanContentMargin));
	CHECK(near(min.y, -4000.0 - core::kPlanContentMargin));
	CHECK(near(max.y, 4000.0 + core::kPlanContentMargin));

	// レイヤを指定すると、そのレイヤに載る命令だけ（＝その伏図に映る範囲）。
	CHECK(core::planContentBounds(document, {core::kGridLayer}, min, max));
	CHECK(near(min.x, -4000.0 - core::kPlanContentMargin));
	CHECK(near(max.x, 4000.0 + core::kPlanContentMargin));
	CHECK(near(min.y, -3000.0 - core::kPlanContentMargin));
	CHECK(near(max.y, -3000.0 + core::kPlanContentMargin));
}

TEST(plan_content_bounds_sees_every_kind_of_command)
{
	// **座標を持つ命令はどれも広がりに効く**ことを、種類ごとに 1 つずつ載せて確かめる。
	// 命令リストを 1 本足したときにここへ足し忘れると、その要素だけが図からはみ出す
	// （縮尺の見積もりに入らない）ので、種類の網羅そのものが検証項目になる。
	core::Document document;

	const auto boundaryAt = [](double half)
	{
		return std::vector<core::Vec2>{core::Vec2{-half, -half}, core::Vec2{half, -half},
									   core::Vec2{half, half}, core::Vec2{-half, half}};
	};

	core::FloorCommand floor;
	floor.layer = "1-FL";
	floor.boundary = boundaryAt(1000.0);
	document.floors.push_back(floor);

	core::SlabCommand slab;
	slab.layer = "F-底盤";
	slab.boundary = boundaryAt(1100.0);
	document.slabs.push_back(slab);

	core::RoofCommand roof;
	roof.layer = "R-野地板";
	roof.boundary = boundaryAt(1200.0);
	document.roofs.push_back(roof);

	core::MemberCommand member;
	member.layer = "1-横架材天端";
	member.start = core::Vec2{-1300.0, -1300.0};
	member.end = core::Vec2{1300.0, 1300.0};
	document.members.push_back(member);

	core::WallCommand wall;
	wall.layer = "F-立上り";
	wall.start = core::Vec2{-1400.0, 0.0};
	wall.end = core::Vec2{1400.0, 0.0};
	document.walls.push_back(wall);

	core::RafterCommand rafter;
	rafter.layer = "R-垂木";
	rafter.start = core::Vec2{-1500.0, 0.0};
	rafter.end = core::Vec2{1500.0, 0.0};
	document.rafters.push_back(rafter);

	core::ColumnCommand column;
	column.layer = "1to2-柱";
	column.position = core::Vec2{1600.0, 0.0};
	document.columns.push_back(column);

	core::ColumnMarkCommand mark;
	mark.layer = "2-柱伏図記号";
	mark.position = core::Vec2{-1700.0, 0.0};
	document.columnMarks.push_back(mark);

	core::ShearWallCommand shear;
	shear.layer = "1-耐力壁";
	shear.start = core::Vec2{-1750.0, 0.0};
	shear.end = core::Vec2{1750.0, 0.0};
	document.shearWalls.push_back(shear);

	// シンボル置換系 4 種（同じ命令型なので 4 本のリストすべてを見ていることを確かめる）。
	const auto symbolAt = [](const char* layer, const core::Vec2& position)
	{
		core::SymbolCommand symbol;
		symbol.layer = layer;
		symbol.position = position;
		return symbol;
	};
	document.anchorBolts.push_back(symbolAt("F-アンカーボルト", core::Vec2{0.0, 1800.0}));
	document.floorPosts.push_back(symbolAt("F-床束", core::Vec2{0.0, -1900.0}));
	document.fireBraces.push_back(symbolAt("1-横架材天端", core::Vec2{2000.0, 0.0}));
	document.joints.push_back(symbolAt("1-横架材天端", core::Vec2{-2100.0, 0.0}));

	core::GridCommand grid;
	grid.layer = core::kGridLayer;
	grid.start = core::Vec2{-2200.0, -2200.0};
	grid.end = core::Vec2{2200.0, 2200.0};
	document.grids.push_back(grid);

	core::Vec2 min;
	core::Vec2 max;
	CHECK(core::planContentBounds(document, {}, min, max));
	CHECK(near(min.x, -2200.0 - core::kPlanContentMargin));
	CHECK(near(max.x, 2200.0 + core::kPlanContentMargin));
	CHECK(near(min.y, -2200.0 - core::kPlanContentMargin));
	CHECK(near(max.y, 2200.0 + core::kPlanContentMargin));

	// レイヤで絞ると、線分・外形・点のどれも同じ規則で外れる（立上りだけが残る）。
	CHECK(core::planContentBounds(document, {"F-立上り"}, min, max));
	CHECK(near(min.x, -1400.0 - core::kPlanContentMargin));
	CHECK(near(max.x, 1400.0 + core::kPlanContentMargin));
	CHECK(near(min.y, 0.0 - core::kPlanContentMargin));
	CHECK(near(max.y, 0.0 + core::kPlanContentMargin));

	// 点だけのレイヤ（アンカーボルト）でも同じ。
	CHECK(core::planContentBounds(document, {"F-アンカーボルト"}, min, max));
	CHECK(near(min.y, 1800.0 - core::kPlanContentMargin));
	CHECK(near(max.y, 1800.0 + core::kPlanContentMargin));
}

TEST(plan_content_bounds_fails_without_coordinates)
{
	// 座標を持つ命令が 1 つも無ければ広がりは求まらない（out は触らない）。
	core::Vec2 min{1.0, 2.0};
	core::Vec2 max{3.0, 4.0};
	CHECK(!core::planContentBounds(core::Document{}, {}, min, max));
	CHECK(near(min.x, 1.0));
	CHECK(near(max.y, 4.0));

	// 指定したレイヤに何も載っていないときも同じ（伏図の表示レイヤが 1 つも描かれなかった）。
	core::Document document;
	core::ColumnCommand column;
	column.layer = "1to2-柱";
	column.position = core::Vec2{0.0, 0.0};
	document.columns.push_back(column);
	CHECK(!core::planContentBounds(document, {"2to3-柱"}, min, max));
}

TEST(section_content_size_pairs_plan_width_with_section_height)
{
	core::Document document;
	// 平面は 8m（X）× 12m（Y）。幅は**大きい方**＝ Y の広がりを採る（X通りの軸組図が
	// 映すのは Y 方向の広がりなので、両方向が同じマスに収まるように揃える）。
	core::MemberCommand member;
	member.layer = "1-横架材天端";
	member.start = core::Vec2{-4000.0, -6000.0};
	member.end = core::Vec2{4000.0, 6000.0};
	member.elevation = 3000.0;
	member.endElevation = 3000.0;
	member.height = 180.0;
	document.members.push_back(member);

	core::Vec2 size;
	CHECK(core::sectionContentSize(document, size));
	CHECK(near(size.x, 12000.0 + (2.0 * core::kPlanContentMargin)));
	// 高さは断面の高さ範囲（天端 3000・下端 2820 に上下の余白）。
	CHECK(near(size.y, (3000.0 - 2820.0) + (2.0 * core::kSectionHeightMargin)));

	// 何も無い文書では求まらない。
	core::Vec2 untouched{7.0, 8.0};
	CHECK(!core::sectionContentSize(core::Document{}, untouched));
	CHECK(near(untouched.x, 7.0));

	// 平面が決まっても**高さの分かる要素が 1 つも無ければ**求まらない（通り芯だけの文書）。
	core::Document flat;
	core::GridCommand grid;
	grid.layer = core::kGridLayer;
	grid.start = core::Vec2{-1000.0, 0.0};
	grid.end = core::Vec2{1000.0, 0.0};
	flat.grids.push_back(grid);
	CHECK(!core::sectionContentSize(flat, untouched));
	CHECK(near(untouched.x, 7.0));
}

// --------------------------------------------------------------------------
// - parse::buildDocument（読み込めないパスでは空の Document が返る）
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
// 断面記号・伏図記号（M12）の検証
// ---------------------------------------------------------------------------

namespace
{
	core::ColumnMarkCommand validSectionMark()
	{
		core::ColumnMarkCommand mark;
		mark.layer = "1to2-柱";
		mark.drawClass = "01作図-01線-02実線-01極細線";
		mark.targetLayer = "1to2-柱";
		mark.style = core::ColumnMarkStyle::Section;
		return mark;
	}

	core::ColumnMarkCommand validPlanMark()
	{
		core::ColumnMarkCommand mark;
		mark.layer = "2-柱伏図記号";
		mark.drawClass = "01作図-04記号-04構造-一般";
		mark.targetLayer = "1to2-柱";
		mark.style = core::ColumnMarkStyle::Plan;
		mark.symbol = "柱伏図記号";
		return mark;
	}
} // namespace

TEST(validate_accepts_document_with_valid_column_marks)
{
	core::Document document;
	document.columnMarks.push_back(validSectionMark());
	document.columnMarks.push_back(validPlanMark());
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_column_mark_without_layer_class_or_target)
{
	// PIO を置くレイヤ・作図クラス・**検索対象レイヤ**はどれも欠かせない。とくに
	// 検索対象が空だと PIO は何も見つけられず、記号 0 個の空オブジェクトが残る。
	for (int which = 0; which < 3; ++which)
	{
		core::Document document;
		core::ColumnMarkCommand mark = validSectionMark();
		if (which == 0)
			mark.layer.clear();
		else if (which == 1)
			mark.drawClass.clear();
		else
			mark.targetLayer.clear();
		document.columnMarks.push_back(mark);
		CHECK(!core::validateDocument(document));
	}
}

TEST(validate_rejects_plan_mark_without_symbol)
{
	// 伏図記号はシンボルを置くだけの記号なので、名前が無ければ何も描けない。
	core::Document document;
	core::ColumnMarkCommand mark = validPlanMark();
	mark.symbol.clear();
	document.columnMarks.push_back(mark);
	CHECK(!core::validateDocument(document));
}

TEST(validate_accepts_section_mark_without_symbol)
{
	// 断面記号は実断面から描くのでシンボルを使わない（空が正常）。対象クラスも
	// **空が正常**＝全クラス。
	core::Document document;
	core::ColumnMarkCommand mark = validSectionMark();
	CHECK(mark.symbol.empty() && mark.targetClass.empty());
	document.columnMarks.push_back(mark);
	CHECK(core::validateDocument(document));
}

// --------------------------------------------------------------------------
// - core::Geometry（型が使えることの確認。数式そのものは GeometryTests で検証する）
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

// ---------------------------------------------------------------------------
// - 耐力壁の検証（core::validateDocument の isValidShearWall）。M19。
// ---------------------------------------------------------------------------

namespace
{
	// 妥当な耐力壁 1 枚（筋かい）。各ケースはここから 1 か所だけ壊す。
	core::ShearWallCommand validShearWall()
	{
		core::ShearWallCommand wall;
		wall.layer = "1-耐力壁";
		wall.drawClass = "04構造-02木造-07筋かい";
		wall.targetLayers = "1to2-柱";
		wall.start = core::Vec2{0.0, 0.0};
		wall.end = core::Vec2{910.0, 0.0};
		wall.kind = core::ShearWallKind::Brace;
		wall.width = 90.0;
		wall.thickness = 45.0;
		wall.clearSpan = 805.0;
		wall.bottomHeight = 0.0;
		wall.topHeight = 2800.0;
		return wall;
	}

	// 耐力壁 1 枚だけを載せた文書が検証を通るか。
	bool acceptsShearWall(const core::ShearWallCommand& wall)
	{
		core::Document document;
		document.shearWalls.push_back(wall);
		return core::validateDocument(document);
	}
} // namespace

TEST(validate_accepts_a_valid_shear_wall)
{
	CHECK(acceptsShearWall(validShearWall()));

	// 柱の無い階でも描けなければならないので、探索先レイヤは空でも妥当
	// （PIO は控えの内法で描く。core/Document.cpp の isValidShearWall）。
	core::ShearWallCommand noColumns = validShearWall();
	noColumns.targetLayers.clear();
	CHECK(acceptsShearWall(noColumns));

	// 面材は見付け幅を使わないので 0 のままで妥当。
	core::ShearWallCommand panel = validShearWall();
	panel.kind = core::ShearWallKind::Panel;
	panel.width = 0.0;
	CHECK(acceptsShearWall(panel));
}

TEST(validate_rejects_a_broken_shear_wall)
{
	// 描けない値を 1 つずつ入れて、そのたびに文書ごと弾かれること。
	core::ShearWallCommand noLayer = validShearWall();
	noLayer.layer.clear();
	CHECK(!acceptsShearWall(noLayer));

	core::ShearWallCommand noClass = validShearWall();
	noClass.drawClass.clear();
	CHECK(!acceptsShearWall(noClass));

	core::ShearWallCommand degenerate = validShearWall();
	degenerate.end = degenerate.start; // 軸が縮退＝向きも長さも決まらない
	CHECK(!acceptsShearWall(degenerate));

	core::ShearWallCommand noThickness = validShearWall();
	noThickness.thickness = 0.0;
	CHECK(!acceptsShearWall(noThickness));

	core::ShearWallCommand flat = validShearWall();
	flat.topHeight = flat.bottomHeight; // 軸組内法の高さが無い
	CHECK(!acceptsShearWall(flat));

	core::ShearWallCommand noSpan = validShearWall();
	noSpan.clearSpan = 0.0; // 控えの内法が無い
	CHECK(!acceptsShearWall(noSpan));

	core::ShearWallCommand noWidth = validShearWall();
	noWidth.width = 0.0; // 筋かいは見付け幅が要る（幅 0 の帯は描けない）
	CHECK(!acceptsShearWall(noWidth));
}

// ---------------------------------------------------------------------------
// - 耐力壁の筋かいの形（core::shearWallBracePolygon）。M19。
// ---------------------------------------------------------------------------

TEST(shear_wall_brace_polygon_is_clipped_to_the_frame)
{
	// 内法 3000×2400 に幅 100 の帯。帯の 4 つの角はそれぞれ内法の別の辺の外へ出るので、
	// どの角も 2 頂点に切り分けられて八角形（端が斜めに落ちた形）になる。頂点はすべて
	// 内法の中に収まる。
	const std::vector<core::Vec2> brace =
		core::shearWallBracePolygon(0.0, 3000.0, 0.0, 2400.0, 100.0, true);
	CHECK_EQ(brace.size(), std::size_t{8});
	for (const core::Vec2& point : brace)
	{
		CHECK(point.x >= -1e-9 && point.x <= 3000.0 + 1e-9);
		CHECK(point.y >= -1e-9 && point.y <= 2400.0 + 1e-9);
	}
}

TEST(shear_wall_brace_polygon_follows_the_rise_direction)
{
	// 傾きの向きで、下端に接する側が入れ替わる。始点側が下（risesToEnd=true）なら
	// 内法の左下隅の近くに頂点があり、逆向きなら右下隅の近くにある。
	const auto lowestX = [](const std::vector<core::Vec2>& poly)
	{
		double best = poly.front().x;
		double bestY = poly.front().y;
		for (const core::Vec2& point : poly)
		{
			if (point.y < bestY)
			{
				bestY = point.y;
				best = point.x;
			}
		}
		return best;
	};
	const std::vector<core::Vec2> up =
		core::shearWallBracePolygon(0.0, 3000.0, 0.0, 2400.0, 100.0, true);
	const std::vector<core::Vec2> down =
		core::shearWallBracePolygon(0.0, 3000.0, 0.0, 2400.0, 100.0, false);
	CHECK(lowestX(up) < 1500.0);
	CHECK(lowestX(down) > 1500.0);
}

TEST(shear_wall_brace_polygon_rejects_a_degenerate_frame)
{
	// 内法が潰れている・幅が無いときは描けない（空を返す）。
	CHECK(core::shearWallBracePolygon(0.0, 0.0, 0.0, 2400.0, 100.0, true).empty());
	CHECK(core::shearWallBracePolygon(0.0, 3000.0, 2400.0, 2400.0, 100.0, true).empty());
	CHECK(core::shearWallBracePolygon(0.0, 3000.0, 0.0, 2400.0, 0.0, true).empty());
}

TEST_MAIN();
