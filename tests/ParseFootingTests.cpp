//
//	ParseFootingTests.cpp
//
//	基礎解析（src/parse/Footing）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:
//	core/ parse/ は無 SDK で単体テスト）。Python 版 test_ifc_footing.py の
//	**M9 に相当するケース**（立上り・底盤・基礎ストーリ）を 1 対 1 で写している
//	（地中梁・人通口・壁結合・配筋のケースは M10）。
//
//	検証項目（ROADMAP.md M9）:
//	  * Name による基礎要素の判別（立上り／地中梁／底盤）
//	  * 基礎ストーリ（"基礎" / suffix "F" / GL=0・レベルとレイヤ）
//	  * 底盤天端＝面積最大の天端 Z、立上り下端＝IFC 実形状（呑み込み補正なし）
//	  * 立上りの統合（同一直線・同一断面のみ）と自由端の半壁厚延長（柱芯スナップ）
//	  * 底盤の統合（辺共有・面重なりの連結成分の多角形和。穴・隙間・角接触は統合しない）
//	  * 底盤外周の外面合わせ（辺ごとに沿う立上りの半壁厚だけ外へ）
//	  * 実フィクスチャでの形（レイヤ・クラス・バインド・不変条件）と決定性
//	実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"
#include "Fixtures.h"

#include "core/Document.h"
#include "parse/Context.h"
#include "parse/Footing.h"
#include "parse/Loader.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::SlabCommand;
using HomeskzIfcImport::core::StoryBoundCommand;
using HomeskzIfcImport::core::StoryCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::core::WallCommand;
using HomeskzIfcImport::parse::alignSlabsToWallFaces;
using HomeskzIfcImport::parse::buildFoundationStoryCommand;
using HomeskzIfcImport::parse::buildSlabCommands;
using HomeskzIfcImport::parse::buildWallCommands;
using HomeskzIfcImport::parse::CLASS_FOUNDATION_SLAB;
using HomeskzIfcImport::parse::CLASS_FOUNDATION_WALL;
using HomeskzIfcImport::parse::Context;
using HomeskzIfcImport::parse::extendFreeWallEnds;
using HomeskzIfcImport::parse::foundationSlabStyleName;
using HomeskzIfcImport::parse::foundationWallStyleName;
using HomeskzIfcImport::parse::hasFoundation;
using HomeskzIfcImport::parse::isBaseSlab;
using HomeskzIfcImport::parse::isFoundationWall;
using HomeskzIfcImport::parse::isGroundBeam;
using HomeskzIfcImport::parse::kFoundationSuffix;
using HomeskzIfcImport::parse::kLayerFoundationAnchor;
using HomeskzIfcImport::parse::kLayerFoundationFloorPost;
using HomeskzIfcImport::parse::kLayerFoundationSlab;
using HomeskzIfcImport::parse::kLayerFoundationWall;
using HomeskzIfcImport::parse::kLevelBeamTop;
using HomeskzIfcImport::parse::kLevelFloorPost;
using HomeskzIfcImport::parse::kLevelFoundationTop;
using HomeskzIfcImport::parse::kLevelGL;
using HomeskzIfcImport::parse::kLevelSlabTop;
using HomeskzIfcImport::parse::kStoryFoundation;
using HomeskzIfcImport::parse::mergeSlabCommands;
using HomeskzIfcImport::parse::mergeWallCommands;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::resolveFoundationTopElevation;
using HomeskzIfcImport::parse::resolveSlabTopElevation;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// 合成の立上り命令（Python 版 test_ifc_footing.py の _wall と同じ既定値）。
	WallCommand wall(Vec2 start, Vec2 end, double thickness = 120.0, double bottomOffset = -100.0,
					 double topOffset = -190.0)
	{
		WallCommand cmd;
		cmd.layer = kLayerFoundationWall;
		cmd.drawClass = CLASS_FOUNDATION_WALL;
		cmd.start = start;
		cmd.end = end;
		cmd.thickness = thickness;
		cmd.styleName = foundationWallStyleName(thickness);
		cmd.components = HomeskzIfcImport::parse::foundationWallComponents(thickness);
		cmd.bottomBound = StoryBoundCommand{0, kLevelGL, bottomOffset};
		cmd.topBound = StoryBoundCommand{1, kLevelBeamTop, topOffset};
		return cmd;
	}

	// 合成の底盤命令（Python 版 _slab / _rect_boundary と同じ既定値）。
	SlabCommand slab(std::vector<Vec2> boundary, double thickness = 150.0, double offset = 0.0)
	{
		SlabCommand cmd;
		cmd.layer = kLayerFoundationSlab;
		cmd.drawClass = CLASS_FOUNDATION_SLAB;
		cmd.boundary = std::move(boundary);
		cmd.styleName = foundationSlabStyleName(thickness);
		cmd.components = HomeskzIfcImport::parse::foundationSlabComponents(thickness);
		cmd.thickness = thickness;
		cmd.elevation = 50.0 + offset;
		cmd.bound = StoryBoundCommand{0, kLevelSlabTop, offset};
		return cmd;
	}

	std::vector<Vec2> rect(double x1, double y1, double x2, double y2)
	{
		return {Vec2{x1, y1}, Vec2{x2, y1}, Vec2{x2, y2}, Vec2{x1, y2}};
	}

	// 自由端の終端柱判定用の最小の柱命令（位置しか使わない）。
	core::ColumnCommand column(double x, double y)
	{
		core::ColumnCommand cmd;
		cmd.layer = "1to2-柱";
		cmd.memberId = "105×105 - 管柱";
		cmd.drawClass = HomeskzIfcImport::parse::CLASS_KUDABASHIRA;
		cmd.structuralUse = "4";
		cmd.position = Vec2{x, y};
		cmd.width = 105.0;
		cmd.depth = 105.0;
		cmd.height = 2800.0;
		cmd.bottomBound = StoryBoundCommand{0, kLevelBeamTop, 0.0};
		cmd.topBound = StoryBoundCommand{1, kLevelBeamTop, 0.0};
		return cmd;
	}

	double minX(const std::vector<Vec2>& pts)
	{
		return std::ranges::min_element(pts, {}, &Vec2::x)->x;
	}
	double maxX(const std::vector<Vec2>& pts)
	{
		return std::ranges::max_element(pts, {}, &Vec2::x)->x;
	}
	double minY(const std::vector<Vec2>& pts)
	{
		return std::ranges::min_element(pts, {}, &Vec2::y)->y;
	}
	double maxY(const std::vector<Vec2>& pts)
	{
		return std::ranges::max_element(pts, {}, &Vec2::y)->y;
	}

	// 外形の頂点数が最大の底盤（＝統合された 1 枚）。
	const SlabCommand& biggest(const std::vector<SlabCommand>& slabs)
	{
		return *std::ranges::max_element(slabs, {},
										 [](const SlabCommand& s) { return s.boundary.size(); });
	}
} // namespace

// ---------------------------------------------------------------------------
// Name による基礎要素の判別（Python 版 _is_wall / _is_ground_beam / _is_base_slab）
// ---------------------------------------------------------------------------

TEST(name_predicates_classify_footing_elements)
{
	// ホームズ君 IFC が実際に出す Name（伏図次郎の IfcFooting / IfcSlab から採取）。
	// 立上りは "基礎梁" 始まり、地中梁・底盤は部分一致で判別する。
	CHECK(isFoundationWall("基礎梁:1"));
	CHECK(!isFoundationWall("地中梁:1"));
	CHECK(!isFoundationWall("基礎底盤:1"));

	CHECK(isGroundBeam("地中梁:1"));
	CHECK(isGroundBeam("部分地中梁:3")); // 部分一致なので接頭辞が付いても地中梁
	CHECK(!isGroundBeam("基礎梁:1"));
	CHECK(!isGroundBeam("基礎底盤:1"));

	CHECK(isBaseSlab("基礎底盤:1"));
	CHECK(isBaseSlab("独立基礎底盤:2"));
	CHECK(!isBaseSlab("独立基礎:1")); // "底盤" を含まない独立基礎そのものは底盤でない
	CHECK(!isBaseSlab("基礎梁:1"));
	CHECK(!isBaseSlab("床版"));

	// 空の Name（Name 未設定の要素）はいずれにも当たらない。
	CHECK(!isFoundationWall(""));
	CHECK(!isGroundBeam(""));
	CHECK(!isBaseSlab(""));
}

// ---------------------------------------------------------------------------
// 立上りの統合（mergeWallCommands）— Python 版 TestMergeWallCommands
// ---------------------------------------------------------------------------

TEST(merge_walls_collinear_touching_into_one)
{
	const std::vector<WallCommand> merged = mergeWallCommands(
		{wall(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}), wall(Vec2{1000.0, 0.0}, Vec2{3000.0, 0.0})});
	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	CHECK(near(merged[0].start.x, 0.0) && near(merged[0].start.y, 0.0));
	CHECK(near(merged[0].end.x, 3000.0) && near(merged[0].end.y, 0.0));
	// 断面・高さ基準は先頭の立上りから引き継ぐ。
	CHECK(near(merged[0].thickness, 120.0));
	CHECK(near(merged[0].bottomBound.offset, -100.0));
	CHECK(near(merged[0].topBound.offset, -190.0));
}

TEST(merge_walls_overlapping_segments)
{
	const std::vector<WallCommand> merged = mergeWallCommands(
		{wall(Vec2{0.0, 0.0}, Vec2{2000.0, 0.0}), wall(Vec2{1500.0, 0.0}, Vec2{3000.0, 0.0})});
	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	CHECK(near(merged[0].start.x, 0.0));
	CHECK(near(merged[0].end.x, 3000.0));
}

TEST(merge_walls_chain_of_three)
{
	const std::vector<WallCommand> merged = mergeWallCommands(
		{wall(Vec2{0.0, 5.0}, Vec2{0.0, 1000.0}), wall(Vec2{0.0, 1000.0}, Vec2{0.0, 2000.0}),
		 wall(Vec2{0.0, 2000.0}, Vec2{0.0, 3000.0})});
	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	CHECK(near(std::min(merged[0].start.y, merged[0].end.y), 5.0));
	CHECK(near(std::max(merged[0].start.y, merged[0].end.y), 3000.0));
	CHECK(near(merged[0].start.x, 0.0) && near(merged[0].end.x, 0.0));
}

TEST(merge_walls_leaves_gap_parallel_and_perpendicular_alone)
{
	// 同一直線でも隙間があるものは橋渡ししない。
	CHECK_EQ(mergeWallCommands({wall(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}),
								wall(Vec2{2000.0, 0.0}, Vec2{3000.0, 0.0})})
				 .size(),
			 std::size_t{2});
	// 平行だが別の線上（直交距離＝壁厚ぶん）の側並びも統合しない。
	CHECK_EQ(mergeWallCommands({wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
								wall(Vec2{0.0, 120.0}, Vec2{3000.0, 120.0})})
				 .size(),
			 std::size_t{2});
	// 直交して接するもの（コーナー）も別の壁のまま。
	CHECK_EQ(mergeWallCommands({wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
								wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0})})
				 .size(),
			 std::size_t{2});
}

TEST(merge_walls_keeps_different_sections_apart)
{
	// 壁厚が違えば同一直線・接触でも別断面として残す。
	CHECK_EQ(mergeWallCommands({wall(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 120.0),
								wall(Vec2{1000.0, 0.0}, Vec2{3000.0, 0.0}, 150.0)})
				 .size(),
			 std::size_t{2});
	// 天端の高さ（topBound.offset）が違う場合も同じ。
	CHECK_EQ(mergeWallCommands({wall(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 120.0, -100.0, -190.0),
								wall(Vec2{1000.0, 0.0}, Vec2{3000.0, 0.0}, 120.0, -100.0, -540.0)})
				 .size(),
			 std::size_t{2});
}

TEST(merge_walls_empty_returns_empty)
{
	CHECK(mergeWallCommands({}).empty());
}

// ---------------------------------------------------------------------------
// 自由端の延長（extendFreeWallEnds）— Python 版 TestExtendFreeWallEnds
// ---------------------------------------------------------------------------

TEST(extend_free_ends_of_isolated_wall)
{
	// どの立上りとも交差しない立上り → 両端を半壁厚（60）ずつ外側へ延長する。
	const std::vector<WallCommand> ext =
		extendFreeWallEnds({wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 120.0)}, {});
	CHECK_EQ(ext.size(), std::size_t{1});
	if (ext.empty())
		return;
	CHECK(near(ext[0].start.x, -60.0) && near(ext[0].start.y, 0.0));
	CHECK(near(ext[0].end.x, 3060.0) && near(ext[0].end.y, 0.0));
}

TEST(extend_follows_wall_axis)
{
	// 延長方向は壁芯（自分の軸）に沿う。始点は始点側・終点は終点側へ。
	const std::vector<WallCommand> ext =
		extendFreeWallEnds({wall(Vec2{0.0, 0.0}, Vec2{0.0, 2000.0}, 150.0)}, {});
	CHECK(near(ext[0].start.y, -75.0));
	CHECK(near(ext[0].end.y, 2075.0));
}

TEST(extend_leaves_joined_corner_ends_alone)
{
	// コーナーで交わる端点は据え置き、反対側（自由端）だけ延長する。
	const std::vector<WallCommand> ext = extendFreeWallEnds(
		{wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}), wall(Vec2{0.0, 0.0}, Vec2{0.0, 3000.0})}, {});
	CHECK_EQ(ext.size(), std::size_t{2});
	if (ext.size() < 2)
		return;
	CHECK(near(ext[0].start.x, 0.0) && near(ext[0].start.y, 0.0));
	CHECK(near(ext[0].end.x, 3060.0));
	CHECK(near(ext[1].start.x, 0.0) && near(ext[1].start.y, 0.0));
	CHECK(near(ext[1].end.y, 3060.0));
}

TEST(extend_t_junction_stem_end_not_extended)
{
	// 通し材に突き当たる stem の端点は据え置き、通し材の両自由端は延長する。
	const std::vector<WallCommand> ext = extendFreeWallEnds(
		{wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}), wall(Vec2{1500.0, 0.0}, Vec2{1500.0, 2000.0})},
		{});
	CHECK(near(ext[0].start.x, -60.0));
	CHECK(near(ext[0].end.x, 3060.0));
	CHECK(near(ext[1].start.y, 0.0));  // 突き当て側（交差）は据え置き
	CHECK(near(ext[1].end.y, 2060.0)); // 反対の自由端は延長
}

TEST(extend_leaves_zero_length_wall_unchanged)
{
	const std::vector<WallCommand> ext =
		extendFreeWallEnds({wall(Vec2{100.0, 100.0}, Vec2{100.0, 100.0})}, {});
	CHECK(near(ext[0].start.x, 100.0) && near(ext[0].start.y, 100.0));
	CHECK(near(ext[0].end.x, 100.0) && near(ext[0].end.y, 100.0));
}

TEST(extend_ignores_walls_on_other_layers)
{
	// レイヤが違う立上りは交差判定の対象外＝互いに自由端扱いになる。
	WallCommand other = wall(Vec2{0.0, 0.0}, Vec2{0.0, 3000.0});
	other.layer = "F-別レイヤ";
	const std::vector<WallCommand> ext =
		extendFreeWallEnds({wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}), other}, {});
	CHECK(near(ext[0].start.x, -60.0) && near(ext[0].end.x, 3060.0));
	CHECK(near(ext[1].start.y, -60.0) && near(ext[1].end.y, 3060.0));
}

TEST(extend_free_end_snaps_to_terminal_column_center)
{
	// 半島状の自由端が柱芯より外側（土台の半材せいぶん＝52mm）に入力されていても、
	// 終端柱の柱芯 + 半壁厚に揃える。150mm 壁・柱芯 y=5460・自由端 y=5512 →
	// 柱芯へ寄せてから半壁厚（75）延長 → y=5535。
	const std::vector<WallCommand> ext = extendFreeWallEnds(
		{wall(Vec2{0.0, 5512.0}, Vec2{0.0, 4550.0}, 150.0)}, {column(0.0, 5460.0)});
	CHECK(near(ext[0].start.y, 5535.0));
	// end も自由端だが柱が無いので端点から半壁厚（75）延長される。
	CHECK(near(ext[0].end.y, 4475.0));
}

TEST(extend_free_end_at_column_center_is_plain_half_thickness)
{
	// 柱芯 = 自由端なら柱芯へ寄せても位置は変わらず、半壁厚だけ延長される。
	const std::vector<WallCommand> ext =
		extendFreeWallEnds({wall(Vec2{0.0, 0.0}, Vec2{0.0, 3000.0}, 120.0)}, {column(0.0, 3000.0)});
	CHECK(near(ext[0].end.y, 3060.0));
	CHECK(near(ext[0].start.y, -60.0)); // 柱の無い始端は端点から半壁厚
}

TEST(extend_ignores_far_and_offaxis_columns)
{
	// 沿軸許容（150mm）を超えて内側にある柱は終端柱にしない（隣モジュールを拾わない）。
	const std::vector<WallCommand> farColumn = extendFreeWallEnds(
		{wall(Vec2{0.0, 5512.0}, Vec2{0.0, 4550.0}, 120.0)}, {column(0.0, 5000.0)});
	CHECK(near(farColumn[0].start.y, 5572.0)); // 端点 5512 + 半壁厚 60

	// 壁芯線から半壁厚 + 余裕を超えて外れた柱（側並びの別壁の柱等）も使わない。
	const std::vector<WallCommand> offAxis = extendFreeWallEnds(
		{wall(Vec2{0.0, 5512.0}, Vec2{0.0, 4550.0}, 120.0)}, {column(200.0, 5460.0)});
	CHECK(near(offAxis[0].start.y, 5572.0));

	// 柱を渡さなければ端点から半壁厚延長する（後方互換）。
	const std::vector<WallCommand> noColumns =
		extendFreeWallEnds({wall(Vec2{0.0, 5512.0}, Vec2{0.0, 4550.0}, 120.0)}, {});
	CHECK(near(noColumns[0].start.y, 5572.0));
}

TEST(extend_empty_returns_empty)
{
	CHECK(extendFreeWallEnds({}, {}).empty());
}

// ---------------------------------------------------------------------------
// 底盤の統合（mergeSlabCommands）— Python 版 TestMergeSlabCommands
// ---------------------------------------------------------------------------

TEST(merge_slabs_two_adjacent_rects_into_one)
{
	const std::vector<SlabCommand> merged = mergeSlabCommands(
		{slab(rect(0.0, 0.0, 1000.0, 1000.0)), slab(rect(1000.0, 0.0, 2000.0, 1000.0))});
	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	const std::vector<Vec2>& pts = merged[0].boundary;
	CHECK(near(minX(pts), 0.0) && near(maxX(pts), 2000.0));
	CHECK(near(minY(pts), 0.0) && near(maxY(pts), 1000.0));
	CHECK_EQ(pts.size(), std::size_t{4}); // 共線の中間点は落として 1 つの矩形になる
}

TEST(merge_slabs_l_shape_has_six_vertices)
{
	const std::vector<SlabCommand> merged = mergeSlabCommands(
		{slab(rect(0.0, 0.0, 2000.0, 2000.0)), slab(rect(0.0, 2000.0, 1000.0, 3000.0))});
	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	CHECK_EQ(merged[0].boundary.size(), std::size_t{6});
}

TEST(merge_slabs_leaves_gap_and_corner_touch_alone)
{
	// 隙間のある底盤は橋渡ししない。
	CHECK_EQ(mergeSlabCommands(
				 {slab(rect(0.0, 0.0, 1000.0, 1000.0)), slab(rect(1100.0, 0.0, 2000.0, 1000.0))})
				 .size(),
			 std::size_t{2});
	// 角（点）だけで接する底盤は連続とみなさない。
	CHECK_EQ(mergeSlabCommands(
				 {slab(rect(0.0, 0.0, 1000.0, 1000.0)), slab(rect(1000.0, 1000.0, 2000.0, 2000.0))})
				 .size(),
			 std::size_t{2});
}

TEST(merge_slabs_keeps_different_thickness_or_height_apart)
{
	CHECK_EQ(mergeSlabCommands({slab(rect(0.0, 0.0, 1000.0, 1000.0), 150.0),
								slab(rect(1000.0, 0.0, 2000.0, 1000.0), 180.0)})
				 .size(),
			 std::size_t{2});
	CHECK_EQ(mergeSlabCommands({slab(rect(0.0, 0.0, 1000.0, 1000.0), 150.0, 0.0),
								slab(rect(1000.0, 0.0, 2000.0, 1000.0), 150.0, -100.0)})
				 .size(),
			 std::size_t{2});
}

TEST(merge_slabs_ring_with_hole_is_left_alone)
{
	// 中空（穴）になる連結成分は単一境界で表せないため統合しない（元のまま 4 枚）。
	// ベタで埋めると部屋の下までコンクリートになり誤りになる（布基礎の升目状ラティス）。
	const std::vector<SlabCommand> merged = mergeSlabCommands({
		slab(rect(0.0, 0.0, 3000.0, 500.0)),	 // 下辺
		slab(rect(0.0, 2500.0, 3000.0, 3000.0)), // 上辺
		slab(rect(0.0, 0.0, 500.0, 3000.0)),	 // 左辺
		slab(rect(2500.0, 0.0, 3000.0, 3000.0)), // 右辺
	});
	CHECK_EQ(merged.size(), std::size_t{4});
}

TEST(merge_slabs_single_passes_through_unchanged)
{
	const SlabCommand one = slab(rect(0.0, 0.0, 1000.0, 1000.0));
	const std::vector<SlabCommand> merged = mergeSlabCommands({one});
	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	CHECK_EQ(merged[0].boundary.size(), one.boundary.size());
	CHECK(near(merged[0].boundary[0].x, one.boundary[0].x));
	CHECK(near(merged[0].boundary[0].y, one.boundary[0].y));
}

TEST(merge_slabs_handles_diagonal_edges)
{
	// 斜め辺（45 度取合い）を持つ底盤も連続する矩形底盤と 1 枚に統合される
	// （任意向きの多角形和なので、軸平行以外の辺も扱える）。
	const std::vector<SlabCommand> merged = mergeSlabCommands(
		{slab(rect(0.0, 0.0, 2000.0, 2000.0)), slab(rect(2000.0, 0.0, 4000.0, 2000.0)),
		 slab({Vec2{0.0, 2000.0}, Vec2{2000.0, 2000.0}, Vec2{2000.0, 3000.0},
			   Vec2{1000.0, 3000.0}})});
	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	// 斜め辺（x も y も動く辺）が外形に含まれる。
	const std::vector<Vec2>& pts = merged[0].boundary;
	bool hasDiagonal = false;
	for (std::size_t i = 0; i < pts.size(); ++i)
	{
		const Vec2& a = pts[i];
		const Vec2& b = pts[(i + 1) % pts.size()];
		if (!near(a.x, b.x) && !near(a.y, b.y))
			hasDiagonal = true;
	}
	CHECK(hasDiagonal);
}

TEST(merge_slabs_handles_rotated_grid)
{
	// グリッドごと回転した底盤群（斜めの建物）も連続していれば統合される。
	const double angle = 30.0 * 3.14159265358979323846 / 180.0;
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	const auto rotated = [c, s](const std::vector<Vec2>& pts)
	{
		std::vector<Vec2> out;
		out.reserve(pts.size());
		for (const Vec2& p : pts)
			out.push_back(Vec2{(p.x * c) - (p.y * s), (p.x * s) + (p.y * c)});
		return out;
	};
	const std::vector<SlabCommand> merged =
		mergeSlabCommands({slab(rotated(rect(0.0, 0.0, 2000.0, 1000.0))),
						   slab(rotated(rect(2000.0, 0.0, 4000.0, 1000.0)))});
	CHECK_EQ(merged.size(), std::size_t{1});
}

TEST(merge_slabs_keeps_disjoint_groups_separate)
{
	// 連続する 2 枚 + 離れた 1 枚 → 統合 1 枚 + 単独 1 枚 = 2 枚。
	CHECK_EQ(mergeSlabCommands({slab(rect(0.0, 0.0, 1000.0, 1000.0)),
								slab(rect(1000.0, 0.0, 2000.0, 1000.0)),
								slab(rect(5000.0, 5000.0, 6000.0, 6000.0))})
				 .size(),
			 std::size_t{2});
}

TEST(merge_slabs_empty_returns_empty)
{
	CHECK(mergeSlabCommands({}).empty());
}

// ---------------------------------------------------------------------------
// 外面合わせ（alignSlabsToWallFaces）— Python 版 TestAlignSlabsToWallFaces
// ---------------------------------------------------------------------------

TEST(align_offsets_edges_on_wall_centerlines_outward)
{
	// 200mm 厚の立上りが底盤の 4 辺の壁心に沿う → 各辺が半壁厚 100 だけ外へ広がる。
	const std::vector<SlabCommand> result =
		alignSlabsToWallFaces({slab(rect(0.0, 0.0, 1000.0, 1000.0))},
							  {wall(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}, 200.0),
							   wall(Vec2{1000.0, 0.0}, Vec2{1000.0, 1000.0}, 200.0),
							   wall(Vec2{1000.0, 1000.0}, Vec2{0.0, 1000.0}, 200.0),
							   wall(Vec2{0.0, 1000.0}, Vec2{0.0, 0.0}, 200.0)});
	CHECK_EQ(result.size(), std::size_t{1});
	if (result.empty())
		return;
	const std::vector<Vec2>& pts = result[0].boundary;
	CHECK(near(minX(pts), -100.0) && near(maxX(pts), 1100.0));
	CHECK(near(minY(pts), -100.0) && near(maxY(pts), 1100.0));
}

TEST(align_leaves_slab_without_walls_alone)
{
	// 沿う立上りが無い底盤（独立基礎底盤等）は動かさない。
	const SlabCommand one = slab(rect(0.0, 0.0, 1000.0, 1000.0));
	const std::vector<SlabCommand> result =
		alignSlabsToWallFaces({one}, {wall(Vec2{9000.0, 9000.0}, Vec2{9000.0, 10000.0}, 200.0)});
	CHECK_EQ(result[0].boundary.size(), one.boundary.size());
	CHECK(near(minX(result[0].boundary), 0.0) && near(maxX(result[0].boundary), 1000.0));

	// 立上りが 1 本も無ければ無変更。
	const std::vector<SlabCommand> unchanged = alignSlabsToWallFaces({one}, {});
	CHECK(near(maxX(unchanged[0].boundary), 1000.0));
}

TEST(align_uses_per_edge_wall_thickness)
{
	// 上辺だけ 300mm 厚の立上り、他辺は立上り無し → 上辺だけ 150 外へ広がる。
	const std::vector<SlabCommand> result =
		alignSlabsToWallFaces({slab(rect(0.0, 0.0, 1000.0, 1000.0))},
							  {wall(Vec2{0.0, 1000.0}, Vec2{1000.0, 1000.0}, 300.0)});
	CHECK(near(maxY(result[0].boundary), 1150.0)); // 上辺は外面（1000 + 150）へ
	CHECK(near(minY(result[0].boundary), 0.0));	   // 下辺は動かない
}

// ---------------------------------------------------------------------------
// 実フィクスチャからの組み立て（Python 版 TestBuildFromFixture）
// ---------------------------------------------------------------------------

TEST(slab_top_elevation_is_largest_area_height)
{
	// 底盤の大半（基礎底盤 IfcSlab）の天端は 50.0。面積最大の天端 Z を採るので、
	// 独立基礎底盤のような少数の異なる高さは採用されない。
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	double slabTop = 0.0;
	CHECK(resolveSlabTopElevation(model, slabTop));
	CHECK(near(slabTop, 50.0));
}

TEST(foundation_story_command_shape)
{
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	StoryCommand story;
	CHECK(buildFoundationStoryCommand(model, story));
	CHECK_EQ(story.name, std::string(kStoryFoundation));
	CHECK_EQ(story.suffix, std::string(kFoundationSuffix));
	CHECK(near(story.elevation, 0.0)); // GL は常に 0
	// レベルは希望スタック順（上→下）で 基礎天端（アンカーボルト）→ GL（立上り）→
	// 床束 → 底盤天端（底盤）の 4 つ（M9 の 2 つに M11 のシンボル 2 つを足した）。
	CHECK_EQ(story.levels.size(), std::size_t{4});
	if (story.levels.size() < 4)
		return;
	// 基礎天端＝立上りの天端（伏図次郎は GL+400）。アンカーボルトの高さ基準。
	CHECK_EQ(story.levels[0].type, std::string(kLevelFoundationTop));
	CHECK_EQ(story.levels[0].layer, std::string(kLayerFoundationAnchor));
	CHECK(near(story.levels[0].offset, 400.0));
	CHECK_EQ(story.levels[1].type, std::string(kLevelGL));
	CHECK_EQ(story.levels[1].layer, std::string(kLayerFoundationWall));
	CHECK(near(story.levels[1].offset, 0.0));
	// 床束は底盤天端に揃える（レベルは分けるが高さは同じ）。
	CHECK_EQ(story.levels[2].type, std::string(kLevelFloorPost));
	CHECK_EQ(story.levels[2].layer, std::string(kLayerFoundationFloorPost));
	CHECK(near(story.levels[2].offset, 50.0));
	CHECK_EQ(story.levels[3].type, std::string(kLevelSlabTop));
	CHECK_EQ(story.levels[3].layer, std::string(kLayerFoundationSlab));
	CHECK(near(story.levels[3].offset, 50.0));
}

TEST(foundation_top_is_the_highest_wall_top)
{
	// 基礎天端は立上り（基礎梁）の天端の最大値。伏図次郎は GL+400（立上りは −100〜400）。
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	double top = 0.0;
	CHECK(resolveFoundationTopElevation(model, top));
	CHECK(near(top, 400.0));
}

TEST(foundation_top_falls_back_to_slab_top_without_walls)
{
	// 立上りが 1 つも無い基礎（底盤のみ）は基礎天端が定まらないので、ストーリは
	// 基礎天端レベルを底盤天端へフォールバックする（Python 版と同じ）。
	bool ok = false;
	Model const model = fixture("minimal_grid.ifc", ok);
	CHECK(ok);
	double top = 0.0;
	CHECK(!resolveFoundationTopElevation(model, top));
}

TEST(foundation_story_levels_are_consistent_across_fixtures)
{
	// 全フィクスチャで 4 レベルが揃い、床束と底盤天端が同じ高さ（床束は底盤上端に立つ）で、
	// 基礎天端が底盤天端以上（立上りは底盤より上へ出る）であること。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		Model const model = fixture(name, ok);
		CHECK(ok);
		StoryCommand story;
		CHECK(buildFoundationStoryCommand(model, story));
		CHECK_EQ(story.levels.size(), std::size_t{4});
		if (story.levels.size() < 4)
			continue;
		CHECK_EQ(story.levels[0].type, std::string(kLevelFoundationTop));
		CHECK_EQ(story.levels[2].type, std::string(kLevelFloorPost));
		CHECK_EQ(story.levels[3].type, std::string(kLevelSlabTop));
		CHECK(near(story.levels[2].offset, story.levels[3].offset));
		CHECK(story.levels[0].offset >= story.levels[3].offset);
	}
}

TEST(no_foundation_story_without_foundation_elements)
{
	// 基礎要素を持たない IFC では基礎ストーリを作らない（空のレイヤを残さない）。
	bool ok = false;
	Model const model = fixture("minimal_grid.ifc", ok);
	CHECK(ok);
	CHECK(!hasFoundation(model));
	StoryCommand story;
	CHECK(!buildFoundationStoryCommand(model, story));
}

TEST(wall_commands_shape)
{
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	const std::vector<WallCommand> walls = buildWallCommands(model);
	CHECK(!walls.empty());
	for (const WallCommand& cmd : walls)
	{
		CHECK_EQ(cmd.layer, std::string(kLayerFoundationWall));
		CHECK_EQ(cmd.drawClass, std::string(CLASS_FOUNDATION_WALL));
		CHECK(cmd.thickness > 0.0);
		CHECK(!core::samePoint(cmd.start, cmd.end));
		// 壁スタイルは壁厚ごとに 1 つで、構成層（コンクリート 1 層）の総厚が壁厚に一致する。
		CHECK_EQ(cmd.styleName, foundationWallStyleName(cmd.thickness));
		CHECK_EQ(cmd.components.size(), std::size_t{1});
		if (cmd.components.size() == 1)
			CHECK(near(cmd.components[0].thickness, cmd.thickness));
		// 下端は基礎（自階）の GL、上端は 1 階（上階）の横架材天端。
		CHECK_EQ(cmd.bottomBound.storyOffset, 0);
		CHECK_EQ(cmd.bottomBound.level, std::string(kLevelGL));
		CHECK_EQ(cmd.topBound.storyOffset, 1);
		CHECK_EQ(cmd.topBound.level, std::string(kLevelBeamTop));
	}
}

TEST(wall_bottom_is_the_ifc_solid_bottom)
{
	// 立上りの下端は IFC のソリッド下端をそのまま使う（呑み込み等の補正はしない。
	// parse/Footing.h「下端は IFC 実形状のまま」）。ホームズ君は基礎梁を**底盤の底面まで**の
	// 全高でモデリングするので、伏図次郎（底盤天端 50・底盤厚 150）では下端が底盤の底面
	// −100 に一致する。
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	double slabTop = 0.0;
	CHECK(resolveSlabTopElevation(model, slabTop));
	const std::vector<SlabCommand> slabs = buildSlabCommands(model);
	CHECK(!slabs.empty());
	if (slabs.empty())
		return;
	const double slabBottom = slabTop - slabs[0].thickness;
	const std::vector<WallCommand> walls = buildWallCommands(model);
	CHECK(!walls.empty());
	for (const WallCommand& cmd : walls)
		CHECK(near(cmd.bottomBound.offset, slabBottom, 1e-3));
}

TEST(wall_bottom_keeps_per_wall_depth_from_the_ifc)
{
	// 深さの違う基礎梁を持つモデルでは、その差が命令にそのまま残ること（底盤天端から
	// 一律に決めると深い基礎が潰れてしまう）。スキップフロアは −100 と −150 が混在する。
	bool ok = false;
	Model const model = fixture("スキップフロア_サンプル.ifc", ok);
	CHECK(ok);
	std::set<long long> depths;
	for (const WallCommand& cmd : buildWallCommands(model))
		depths.insert(std::llround(cmd.bottomBound.offset));
	CHECK(depths.size() >= 2);
	CHECK(depths.contains(-100));
	CHECK(depths.contains(-150));
}

TEST(walls_are_fully_merged)
{
	// 統合の後、残った立上り同士に「同一断面かつ同一直線で連続する」ペアが無い
	// （＝これ以上まとめられない形になっている）。
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	const std::vector<WallCommand> walls = buildWallCommands(model);
	CHECK(!walls.empty());
	// mergeWallCommands をもう一度掛けても本数が減らない＝収束している。
	CHECK_EQ(mergeWallCommands(walls).size(), walls.size());
}

TEST(slab_commands_shape)
{
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	double slabTop = 0.0;
	CHECK(resolveSlabTopElevation(model, slabTop));
	const std::vector<SlabCommand> slabs = buildSlabCommands(model);
	CHECK(!slabs.empty());
	bool sawMainSlab = false;
	for (const SlabCommand& cmd : slabs)
	{
		CHECK_EQ(cmd.layer, std::string(kLayerFoundationSlab));
		CHECK_EQ(cmd.drawClass, std::string(CLASS_FOUNDATION_SLAB));
		CHECK(cmd.boundary.size() >= 3);
		CHECK_EQ(cmd.bound.storyOffset, 0);
		CHECK_EQ(cmd.bound.level, std::string(kLevelSlabTop));
		// elevation は天端の絶対 Z、bound.offset は底盤天端（絶対）との差。
		CHECK(near(cmd.elevation, slabTop + cmd.bound.offset, 1e-6));
		// 厚みは正の整数 mm（スラブスタイル名と構成層のコンクリート層に一致する）。
		CHECK(cmd.thickness > 0.0);
		CHECK(near(cmd.thickness, std::round(cmd.thickness)));
		CHECK_EQ(cmd.styleName, foundationSlabStyleName(cmd.thickness));
		CHECK_EQ(cmd.components.size(), std::size_t{3});
		if (cmd.components.size() == 3)
			CHECK(near(cmd.components[0].thickness, cmd.thickness));
		CHECK(cmd.datum == core::SlabDatum::Top);
		if (near(cmd.bound.offset, 0.0, 0.1))
			sawMainSlab = true;
	}
	// 主たる底盤は天端＝底盤天端（offset ≈ 0）。
	CHECK(sawMainSlab);
}

TEST(base_slab_outer_boundary_matches_wall_outer_face)
{
	// 底盤外形は立上りの壁心にあるため、外面（壁心 + 半壁厚）まで広がる。伏図次郎の
	// 外周立上りは全て 120mm 厚なので、統合底盤の外周が半壁厚（60mm）外へ動く。
	bool ok = false;
	Model const model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);
	Context context(model);
	const std::vector<WallCommand> walls = buildWallCommands(model);
	const std::vector<SlabCommand> withFace = buildSlabCommands(context, walls);
	// 外面合わせを掛けない場合（壁心のまま）の統合底盤。
	const std::vector<SlabCommand> centerline = buildSlabCommands(context, {});
	CHECK(!withFace.empty() && !centerline.empty());
	if (withFace.empty() || centerline.empty())
		return;
	const double faceMaxX = maxX(biggest(withFace).boundary);
	const double centerMaxX = maxX(biggest(centerline).boundary);
	CHECK(near(faceMaxX - centerMaxX, 60.0, 0.5));
}

TEST(all_fixtures_parse_without_error)
{
	// 全フィクスチャで立上り・底盤・基礎ストーリが例外なく組み立てられ、命令が命令セットの
	// 検証を通ること（ROADMAP.md の完了条件 1）。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		Model const model = fixture(name, ok);
		CHECK(ok);
		Context context(model);
		const std::vector<WallCommand> walls = buildWallCommands(model);
		const std::vector<SlabCommand> slabs = buildSlabCommands(context, walls);
		// 検証済みフィクスチャはいずれも基礎（立上り・底盤）を持つ。
		CHECK(!walls.empty());
		CHECK(!slabs.empty());

		core::Document document;
		document.walls = walls;
		document.slabs = slabs;
		CHECK(core::validateDocument(document));

		StoryCommand story;
		CHECK(buildFoundationStoryCommand(model, story));
		CHECK(!story.levels.empty());
	}
}

TEST(is_deterministic)
{
	// 同じ入力からは同じ命令列（順序・値）が得られる（エンティティ列挙順に依存しない）。
	bool ok = false;
	Model const model = fixture("スキップフロア_サンプル.ifc", ok);
	CHECK(ok);
	const std::vector<WallCommand> firstWalls = buildWallCommands(model);
	const std::vector<WallCommand> secondWalls = buildWallCommands(model);
	CHECK_EQ(firstWalls.size(), secondWalls.size());
	for (std::size_t i = 0; i < firstWalls.size() && i < secondWalls.size(); ++i)
	{
		CHECK(near(firstWalls[i].start.x, secondWalls[i].start.x));
		CHECK(near(firstWalls[i].end.y, secondWalls[i].end.y));
		CHECK(near(firstWalls[i].thickness, secondWalls[i].thickness));
	}

	const std::vector<SlabCommand> firstSlabs = buildSlabCommands(model);
	const std::vector<SlabCommand> secondSlabs = buildSlabCommands(model);
	CHECK_EQ(firstSlabs.size(), secondSlabs.size());
	for (std::size_t i = 0; i < firstSlabs.size() && i < secondSlabs.size(); ++i)
	{
		CHECK_EQ(firstSlabs[i].boundary.size(), secondSlabs[i].boundary.size());
		CHECK(near(firstSlabs[i].elevation, secondSlabs[i].elevation));
		CHECK(near(firstSlabs[i].thickness, secondSlabs[i].thickness));
	}
}

TEST_MAIN()
