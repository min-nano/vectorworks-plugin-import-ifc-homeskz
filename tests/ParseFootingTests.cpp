//
//	ParseFootingTests.cpp
//
//	基礎解析（src/parse/Footing）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:
//	core/ parse/ は無 SDK で単体テスト）。Python 版 test_ifc_footing.py の
//	**M9 / M10 に相当するケース**（立上り・底盤・基礎ストーリ・人通口・壁結合・地中梁）を
//	1 対 1 で写している（配筋のケースは保留）。
//
//	検証項目（ROADMAP.md M9）:
//	  * Name による基礎要素の判別（立上り／地中梁／底盤）
//	  * 基礎ストーリ（"基礎" / suffix "F" / GL=0・レベルとレイヤ）
//	  * 底盤天端＝面積最大の天端 Z、立上り下端＝IFC 実形状（呑み込み補正なし）
//	  * 立上りの統合（同一直線・同一断面のみ）と自由端の半壁厚延長（柱芯スナップ）
//	  * 底盤の統合（辺共有・面重なりの連結成分の多角形和。穴・隙間・角接触は統合しない）
//	  * 底盤外周の外面合わせ（辺ごとに沿う立上りの半壁厚だけ外へ）
//	  * 人通口（開口の区間で立上りを分割／天端を切り下げる。M10）
//	  * 壁結合（L / T / X の判定・天端差の capped・ピック点の寄せ・平行は対象外。M10）
//	  * 端部のキャップ（自由端は閉じ、同じ天端と取り合う端は開く。M10）
//	  * 地中梁（同一軸線上の統合・掃引外形・底盤への振り分け。M10）
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
using HomeskzIfcImport::parse::applyWallCaps;
using HomeskzIfcImport::parse::applyWallOpenings;
using HomeskzIfcImport::parse::attachGroundBeamModifiers;
using HomeskzIfcImport::parse::buildFoundationStoryCommand;
using HomeskzIfcImport::parse::buildSlabCommands;
using HomeskzIfcImport::parse::buildWallCommands;
using HomeskzIfcImport::parse::buildWallJoinCommands;
using HomeskzIfcImport::parse::CLASS_FOUNDATION_SLAB;
using HomeskzIfcImport::parse::CLASS_FOUNDATION_WALL;
using HomeskzIfcImport::parse::Context;
using HomeskzIfcImport::parse::extendDeeperCollinearEnds;
using HomeskzIfcImport::parse::extendFreeWallEnds;
using HomeskzIfcImport::parse::extendModifierEndsToBoundary;
using HomeskzIfcImport::parse::foundationSlabStyleName;
using HomeskzIfcImport::parse::foundationWallStyleName;
using HomeskzIfcImport::parse::hasFoundation;
using HomeskzIfcImport::parse::isBaseSlab;
using HomeskzIfcImport::parse::isFoundationWall;
using HomeskzIfcImport::parse::isGroundBeam;
using HomeskzIfcImport::parse::kFoundationSuffix;
using HomeskzIfcImport::parse::kLayerFoundationSlab;
using HomeskzIfcImport::parse::kLayerFoundationWall;
using HomeskzIfcImport::parse::kLevelBeamTop;
using HomeskzIfcImport::parse::kLevelGL;
using HomeskzIfcImport::parse::kLevelSlabTop;
using HomeskzIfcImport::parse::kStoryFoundation;
using HomeskzIfcImport::parse::mergeGroundBeamModifiers;
using HomeskzIfcImport::parse::mergeSlabCommands;
using HomeskzIfcImport::parse::mergeWallCommands;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::modifierFootprint;
using HomeskzIfcImport::parse::resolveSlabTopElevation;
using HomeskzIfcImport::parse::WallOpening;
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

	// 合成の地中梁（台形プリズム）。断面は実データに倣った下り梁の台形——下端が幅 300mm・
	// 天端が幅 700mm・せい 140mm で、v=0 が梁下端（origin の Z）。
	core::ModifierCommand groundBeam(core::Vec3 origin, double azimuth, double depth)
	{
		core::ModifierCommand cmd;
		cmd.profile = {Vec2{-150.0, 0.0}, Vec2{150.0, 0.0}, Vec2{350.0, 140.0},
					   Vec2{-350.0, 140.0}};
		cmd.depth = depth;
		cmd.origin = origin;
		cmd.azimuth = azimuth;
		return cmd;
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

TEST(extend_does_not_push_into_a_collinear_neighbour)
{
	// **同一直線上で突き合わせになっている端は自由端ではない**（交点判定は平行な立上りを
	// 除外するので、これを見ないと双方が半壁厚ずつ延びて重なる）。統合できない＝上端／下端の
	// 違う隣どうしで顕在化する（実データで 150mm＝半壁厚 2 つぶんの重なりになっていた）。
	const std::vector<WallCommand> ext =
		extendFreeWallEnds({wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 150.0, -100.0, -125.0),
							wall(Vec2{3000.0, 0.0}, Vec2{5000.0, 0.0}, 150.0, -100.0, -225.0)},
						   {});
	CHECK(near(ext[0].end.x, 3000.0));	 // 突き合わせ側は据え置き
	CHECK(near(ext[1].start.x, 3000.0)); // 相手も据え置き（重ならない）
	// 反対側の自由端は従来どおり半壁厚だけ延びる。
	CHECK(near(ext[0].start.x, -75.0));
	CHECK(near(ext[1].end.x, 5075.0));
}

TEST(extend_still_reaches_out_past_a_distant_collinear_wall)
{
	// 同一直線上でも**離れて**いれば突き合わせではない（自由端のまま延長する）。
	const std::vector<WallCommand> ext = extendFreeWallEnds(
		{wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}), wall(Vec2{5000.0, 0.0}, Vec2{7000.0, 0.0})}, {});
	CHECK(near(ext[0].end.x, 3060.0));
	CHECK(near(ext[1].start.x, 4940.0));
}

TEST(extend_keeps_free_ends_of_a_wall_that_swallows_a_short_neighbour)
{
	// 隣が自分の内側に完全に収まっている（端に届かない）場合、外側の壁の端は自由端のまま。
	const std::vector<WallCommand> ext = extendFreeWallEnds(
		{wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}), wall(Vec2{1000.0, 0.0}, Vec2{2000.0, 0.0})}, {});
	CHECK(near(ext[0].start.x, -60.0) && near(ext[0].end.x, 3060.0));
}

TEST(caps_open_a_collinear_butt_joint_of_the_same_top)
{
	// 統合できない（下端が違う）けれど**天端が同じ**で突き合わせ／重なる立上りは、平面で
	// 1 本に見えるべきなので端部を閉じない。天端が違えば段差が実在するので閉じる。
	std::vector<WallCommand> same = {
		wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 150.0, -100.0, -125.0),
		wall(Vec2{3000.0, 0.0}, Vec2{5000.0, 0.0}, 150.0, -70.0, -125.0)};
	applyWallCaps(same, buildWallJoinCommands(same));
	CHECK(!same[0].capEnd);
	CHECK(!same[1].capStart);
	CHECK(same[0].capStart); // 反対側は自由端
	CHECK(same[1].capEnd);

	std::vector<WallCommand> stepped = {
		wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 150.0, -100.0, -125.0),
		wall(Vec2{3000.0, 0.0}, Vec2{5000.0, 0.0}, 150.0, -100.0, -225.0)};
	applyWallCaps(stepped, buildWallJoinCommands(stepped));
	CHECK(stepped[0].capEnd);
	CHECK(stepped[1].capStart);
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
	// レベルは希望スタック順（上→下）で GL（立上り）→ 底盤天端（底盤）の 2 つ。
	// 基礎天端（アンカーボルト）・床束は M11 で足す（空レイヤを作らない方針）。
	CHECK_EQ(story.levels.size(), std::size_t{2});
	if (story.levels.size() < 2)
		return;
	CHECK_EQ(story.levels[0].type, std::string(kLevelGL));
	CHECK_EQ(story.levels[0].layer, std::string(kLayerFoundationWall));
	CHECK(near(story.levels[0].offset, 0.0));
	CHECK_EQ(story.levels[1].type, std::string(kLevelSlabTop));
	CHECK_EQ(story.levels[1].layer, std::string(kLayerFoundationSlab));
	CHECK(near(story.levels[1].offset, 50.0));
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

// ---------------------------------------------------------------------------
// 人通口（applyWallOpenings）— ROADMAP.md M10
// ---------------------------------------------------------------------------

TEST(opening_below_slab_top_splits_wall_without_middle)
{
	// 開口の下端が底盤天端以下 → その区間に立上りは生じない（両側だけを描く）。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})};
	const WallOpening opening{Vec2{1000.0, 0.0}, Vec2{1600.0, 0.0}, 50.0, 400.0};
	const std::vector<WallCommand> carved = applyWallOpenings(walls, {opening}, 50.0, 400.0);

	CHECK_EQ(carved.size(), std::size_t{2});
	if (carved.size() != 2)
		return;
	CHECK(near(carved[0].start.x, 0.0) && near(carved[0].end.x, 1000.0));
	CHECK(near(carved[1].start.x, 1600.0) && near(carved[1].end.x, 3000.0));
	// 天端は元のまま（切り下げていない）。端は実寸法どおりで延長しない。
	CHECK(near(carved[0].topBound.offset, -190.0));
	CHECK(near(carved[1].topBound.offset, -190.0));
	// 壁厚・下端・レイヤ・クラスは元の立上りを引き継ぐ。
	CHECK(near(carved[0].thickness, 120.0));
	CHECK(near(carved[0].bottomBound.offset, -100.0));
	CHECK(carved[1].layer == kLayerFoundationWall);
	CHECK(carved[1].drawClass == CLASS_FOUNDATION_WALL);
}

TEST(opening_above_slab_top_lowers_middle_segment)
{
	// 開口の下端が底盤天端より高い → その区間だけ天端を開口下端へ切り下げる（3 分割）。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})};
	const WallOpening opening{Vec2{1000.0, 0.0}, Vec2{1600.0, 0.0}, 300.0, 400.0};
	// 横架材天端の絶対 Z を 590 とすると、切り下げた天端の offset は 300 − 590 = −290。
	const std::vector<WallCommand> carved = applyWallOpenings(walls, {opening}, 50.0, 590.0);

	CHECK_EQ(carved.size(), std::size_t{3});
	if (carved.size() != 3)
		return;
	CHECK(near(carved[1].start.x, 1000.0) && near(carved[1].end.x, 1600.0));
	CHECK(near(carved[1].topBound.offset, -290.0));
	// 中間区間もレベル種別・階の相対は元のまま（offset だけが変わる）。
	CHECK(carved[1].topBound.level == kLevelBeamTop);
	CHECK_EQ(carved[1].topBound.storyOffset, 1);
	// 両側は元の天端のまま。
	CHECK(near(carved[0].topBound.offset, -190.0));
	CHECK(near(carved[2].topBound.offset, -190.0));
}

TEST(opening_at_wall_end_drops_the_empty_side)
{
	// 開口が立上りの端から始まる → 残らない側（長さ 0）の区間は作らない。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})};
	const WallOpening opening{Vec2{0.0, 0.0}, Vec2{600.0, 0.0}, 50.0, 400.0};
	const std::vector<WallCommand> carved = applyWallOpenings(walls, {opening}, 50.0, 400.0);

	CHECK_EQ(carved.size(), std::size_t{1});
	if (carved.empty())
		return;
	CHECK(near(carved[0].start.x, 600.0) && near(carved[0].end.x, 3000.0));
}

TEST(opening_off_the_wall_is_ignored)
{
	// 壁芯から離れた（側並びの平行壁に乗る）開口・向きの違う開口は当てはめない。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})};
	const WallOpening offset{Vec2{1000.0, 60.0}, Vec2{1600.0, 60.0}, 50.0, 400.0};
	const WallOpening crossing{Vec2{1200.0, -300.0}, Vec2{1200.0, 300.0}, 50.0, 400.0};
	CHECK_EQ(applyWallOpenings(walls, {offset}, 50.0, 400.0).size(), std::size_t{1});
	CHECK_EQ(applyWallOpenings(walls, {crossing}, 50.0, 400.0).size(), std::size_t{1});
}

TEST(multiple_openings_on_one_wall_all_apply)
{
	// 1 本に複数の人通口があっても、更新後の列に順に当てはめるので全部効く。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{6000.0, 0.0})};
	const std::vector<WallOpening> openings = {
		WallOpening{Vec2{1000.0, 0.0}, Vec2{1600.0, 0.0}, 50.0, 400.0},
		WallOpening{Vec2{4000.0, 0.0}, Vec2{4600.0, 0.0}, 50.0, 400.0}};
	const std::vector<WallCommand> carved = applyWallOpenings(walls, openings, 50.0, 400.0);

	CHECK_EQ(carved.size(), std::size_t{3});
	if (carved.size() != 3)
		return;
	CHECK(near(carved[0].end.x, 1000.0));
	CHECK(near(carved[1].start.x, 1600.0) && near(carved[1].end.x, 4000.0));
	CHECK(near(carved[2].start.x, 4600.0));
}

namespace
{
	// 人通口の判定を突くための最小 STEP モデル。**立上り 1 本＋削り取り 4 つ**で、
	// 4 つのうち 1 つだけが人通口として採られる。
	//
	// 立上りの配置は「局所 Z＝押し出し方向＝ワールド +X」「局所 X＝ワールド +Y（＝壁厚）」
	// 「局所 Y＝ワールド +Z（＝壁高）」で、原点 (0,0,250)・断面 120×500 なので、壁芯は
	// (0,0)→(3000,0)・壁厚 120・天端 Z=500・底面 Z=0 になる（ホームズ君の基礎梁と同じ
	// 「鉛直断面を水平に押し出す」表現）。削り取りは Body の差演算を入れ子にして与える
	// （((((素 − v1) − v2) − v3) − v4)）。
	//
	//   v1（採用）  … 天端まで届き底面には届かない Z 帯 [300, 500]＝人通口
	//   v2（不採用）… 底面まで届く全高の削り Z 帯 [0, 500]＝端部が他材で削られたもの
	//   v3（不採用）… 天端に届かない中間帯 Z 帯 [100, 300]
	//   v4（不採用）… 鉛直押し出し（壁芯が水平にならない）
	std::string wallWithOpeningsText()
	{
		return "#1=IFCCARTESIANPOINT((0.,0.,250.));\n"
			   "#2=IFCDIRECTION((1.,0.,0.));\n" // 局所 Z＝押し出し方向（ワールド +X）
			   "#3=IFCDIRECTION((0.,1.,0.));\n" // 局所 X＝壁厚方向（ワールド +Y）
			   "#4=IFCAXIS2PLACEMENT3D(#1,#2,#3);\n"
			   "#5=IFCLOCALPLACEMENT($,#4);\n"
			   // 素の立上り: 断面 120（壁厚）×500（壁高）を 3000 押し出す。
			   "#10=IFCCARTESIANPOINT((0.,0.));\n"
			   "#11=IFCAXIS2PLACEMENT2D(#10,$);\n"
			   "#12=IFCRECTANGLEPROFILEDEF(.AREA.,$,#11,120.,500.);\n"
			   "#13=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
			   "#14=IFCDIRECTION((0.,0.,1.));\n"
			   "#15=IFCEXTRUDEDAREASOLID(#12,$,#14,3000.);\n"
			   // v1: Z 帯 [300, 500]（断面中心を v=+150 へずらす）・区間 x∈[1000, 1600]。
			   "#20=IFCCARTESIANPOINT((0.,150.));\n"
			   "#21=IFCAXIS2PLACEMENT2D(#20,$);\n"
			   "#22=IFCRECTANGLEPROFILEDEF(.AREA.,$,#21,120.,200.);\n"
			   "#23=IFCCARTESIANPOINT((0.,0.,1000.));\n"
			   "#24=IFCAXIS2PLACEMENT3D(#23,$,$);\n"
			   "#25=IFCEXTRUDEDAREASOLID(#22,#24,#14,600.);\n"
			   // v2: 全高 Z 帯 [0, 500]・区間 x∈[2800, 3000]（端部の削り）。
			   "#30=IFCAXIS2PLACEMENT2D(#10,$);\n"
			   "#31=IFCRECTANGLEPROFILEDEF(.AREA.,$,#30,120.,500.);\n"
			   "#32=IFCCARTESIANPOINT((0.,0.,2800.));\n"
			   "#33=IFCAXIS2PLACEMENT3D(#32,$,$);\n"
			   "#34=IFCEXTRUDEDAREASOLID(#31,#33,#14,200.);\n"
			   // v3: 中間帯 Z 帯 [100, 300]（断面中心を v=−50 へ）・区間 x∈[200, 800]。
			   "#40=IFCCARTESIANPOINT((0.,-50.));\n"
			   "#41=IFCAXIS2PLACEMENT2D(#40,$);\n"
			   "#42=IFCRECTANGLEPROFILEDEF(.AREA.,$,#41,120.,200.);\n"
			   "#43=IFCCARTESIANPOINT((0.,0.,200.));\n"
			   "#44=IFCAXIS2PLACEMENT3D(#43,$,$);\n"
			   "#45=IFCEXTRUDEDAREASOLID(#42,#44,#14,600.);\n"
			   // v4: 鉛直押し出し（局所 Z をワールド +Z＝要素の局所 Y へ向ける）。
			   "#50=IFCCARTESIANPOINT((0.,0.,1800.));\n"
			   "#51=IFCDIRECTION((0.,1.,0.));\n"
			   "#52=IFCAXIS2PLACEMENT3D(#50,#51,$);\n"
			   "#53=IFCEXTRUDEDAREASOLID(#22,#52,#14,600.);\n"
			   // Body: ((((素 − v1) − v2) − v3) − v4)
			   "#60=IFCBOOLEANRESULT(.DIFFERENCE.,#15,#25);\n"
			   "#61=IFCBOOLEANRESULT(.DIFFERENCE.,#60,#34);\n"
			   "#62=IFCBOOLEANRESULT(.DIFFERENCE.,#61,#45);\n"
			   "#63=IFCBOOLEANRESULT(.DIFFERENCE.,#62,#53);\n"
			   "#64=IFCSHAPEREPRESENTATION($,'Body','CSG',(#63));\n"
			   "#65=IFCPRODUCTDEFINITIONSHAPE($,$,(#64));\n"
			   "#70=IFCFOOTING('f1',$,'基礎梁:1',$,$,#5,#65,$,$);\n";
	}
} // namespace

TEST(only_top_down_horizontal_cuts_count_as_openings)
{
	// 差演算の第 2 オペランドのうち、**天端まで届き底面には届かない水平押し出し**だけが
	// 人通口。端部が他材で削られた全高の削り・天端に届かない中間帯・鉛直押し出しは
	// 人通口ではない（これらを拾うと立上りを誤って分割・切り下げしてしまう）。
	const Model model = HomeskzIfcImport::parse::loadIfcFromText(wallWithOpeningsText());
	const std::vector<WallOpening> openings =
		HomeskzIfcImport::parse::collectWallOpenings(model, Vec2{0.0, 0.0});

	CHECK_EQ(openings.size(), std::size_t{1});
	if (openings.empty())
		return;
	CHECK(near(openings[0].start.x, 1000.0) && near(openings[0].start.y, 0.0));
	CHECK(near(openings[0].end.x, 1600.0) && near(openings[0].end.y, 0.0));
	CHECK(near(openings[0].zBottom, 300.0));
	CHECK(near(openings[0].zTop, 500.0));

	// この 1 か所を当てはめると、開口下端（300）が底盤天端（50）より高いので
	// **天端を切り下げた中間区間**を挟んだ 3 本になる。
	const std::vector<WallCommand> carved =
		applyWallOpenings({wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})}, openings, 50.0, 590.0);
	CHECK_EQ(carved.size(), std::size_t{3});
	if (carved.size() != 3)
		return;
	CHECK(near(carved[1].topBound.offset, 300.0 - 590.0));
}

TEST(openings_come_from_the_real_fixtures)
{
	// 実フィクスチャの立上りには人通口（差演算の第 2 オペランド）がある。天端まで届き
	// 底面には届かない削りだけを拾うので、Z 帯は必ず「下端 < 上端」で厚みを持つ。
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	Context context(model);
	const std::vector<WallOpening> openings =
		HomeskzIfcImport::parse::collectWallOpenings(model, context.gridCenter());
	CHECK(!openings.empty());
	for (const WallOpening& opening : openings)
	{
		CHECK(opening.zTop > opening.zBottom);
		// 壁芯上の線分として非縮退（水平押し出しだけを拾っている）。
		CHECK(!HomeskzIfcImport::core::samePoint(opening.start, opening.end));
	}

	// このフィクスチャは底盤天端より高い位置に下端のある人通口（z=300、底盤天端 50）を
	// 持つので、**天端を切り下げた区間**が立上りに現れる（＝天端の offset が一様でない）。
	const std::vector<WallCommand> walls = buildWallCommands(model);
	CHECK(!walls.empty());
	std::set<long long> topOffsets;
	for (const WallCommand& command : walls)
		topOffsets.insert(std::llround(command.topBound.offset));
	CHECK(topOffsets.size() >= 2);
}

// ---------------------------------------------------------------------------
// 壁結合（buildWallJoinCommands）— ROADMAP.md M10
// ---------------------------------------------------------------------------

TEST(join_corner_of_two_walls_is_an_L_join)
{
	// 端点どうしで交わるコーナー（同じ天端）→ L 結合・閉じない。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
											wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0})};
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);

	CHECK_EQ(joins.size(), std::size_t{1});
	if (joins.empty())
		return;
	CHECK(joins[0].joinType == core::WallJoinType::L);
	CHECK(!joins[0].capped);
	CHECK(near(joins[0].point.x, 3000.0) && near(joins[0].point.y, 0.0));
	// ピック点は交点そのものではなく「残す側」（交点から遠い端点の方向）へ寄せた点。
	CHECK(joins[0].pickA.x < 3000.0 || joins[0].pickB.y > 0.0);
	CHECK(!HomeskzIfcImport::core::samePoint(joins[0].pickA, joins[0].point));
	CHECK(!HomeskzIfcImport::core::samePoint(joins[0].pickB, joins[0].point));
}

TEST(join_tee_uses_the_stem_as_first_wall)
{
	// 一方の端点が他方の内部で交わる → T 結合。延長される stem（端点側）が a。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{6000.0, 0.0}),
											wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0})};
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);

	CHECK_EQ(joins.size(), std::size_t{1});
	if (joins.empty())
		return;
	CHECK(joins[0].joinType == core::WallJoinType::T);
	CHECK_EQ(joins[0].a, std::size_t{1}); // stem = 端点で突き当たる縦の立上り
	CHECK_EQ(joins[0].b, std::size_t{0}); // through = 通し壁
}

TEST(join_crossing_interiors_is_an_X_join)
{
	// 互いの内部で交わる十字 → X 結合。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{6000.0, 0.0}),
											wall(Vec2{3000.0, -3000.0}, Vec2{3000.0, 3000.0})};
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);

	CHECK_EQ(joins.size(), std::size_t{1});
	if (joins.empty())
		return;
	CHECK(joins[0].joinType == core::WallJoinType::X);
	// **十字は縦横 2 本の壁のまま**にして交差結合（X）で繋ぐ。分割して T 結合 2 つに
	// 置き換えるのは別処理で、モデルとしても誤り（ROADMAP.md M10）。
	// VW の X 結合は **a を交点で 2 本に分割し、b（load bearing wall）を丸ごと残す**ので、
	// **バックボーン（天端が最も高い＝同点なら添字の小さい通し壁）を b にする**。
	CHECK_EQ(joins[0].a, std::size_t{1});
	CHECK_EQ(joins[0].b, std::size_t{0});
	// ピック点は種別に関係なく「残す側」へ寄せた点（Python 版 `_kept_side_pick` と同じ。
	// X 結合では VW が壁を詰めないので寄せは無害）。交点そのものは渡さない。
	CHECK(!HomeskzIfcImport::core::samePoint(joins[0].pickA, joins[0].point));
	CHECK(!HomeskzIfcImport::core::samePoint(joins[0].pickB, joins[0].point));
	// 寄せる先は交点から遠い端点の方向で、それぞれの壁芯上に乗る
	// （a＝縦の壁は x=3000・b＝横の壁は y=0）。
	CHECK(std::abs(joins[0].pickA.x - 3000.0) < 1e-6);
	CHECK(std::abs(joins[0].pickB.y - 0.0) < 1e-6);
}

TEST(a_lone_stem_keeps_the_kept_side_pick_on_the_through_wall)
{
	// 交点に stem が 1 本だけの T 結合は従来どおり「残す側」（交点から遠い端点の方向）。
	// 上のテストの「2 本目は逆側」が既存の T 結合の引数を変えていないことを押さえる。
	const std::vector<WallCommand> walls = {
		wall(Vec2{0.0, 0.0}, Vec2{6000.0, 0.0}, 120.0, -100.0, 0.0), // 通し壁（バックボーン）
		wall(Vec2{4000.0, 0.0}, Vec2{4000.0, 3000.0})};				 // stem
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);

	CHECK_EQ(joins.size(), std::size_t{1});
	if (joins.empty())
		return;
	CHECK(joins[0].joinType == core::WallJoinType::T);
	// 交点 (4000,0) から遠い端点は始点 (0,0) 側なので、寄せ先は交点より小さい x。
	CHECK(joins[0].pickB.x < 4000.0);
	CHECK(near(joins[0].pickB.y, 0.0));
}

TEST(collinear_pair_at_a_corner_makes_the_deeper_wall_the_through_wall)
{
	// 上端が同じで**下端だけ違う**（＝統合できない）立上りが、直交する立上りの位置で
	// 突き合わさる形。そこにコーナーは無いので、**深いほうを通し壁**にして直交する立上りを
	// T 結合する。通し壁は相手の半壁厚だけ伸びて交点を越える（実データの (6370,1820)。
	// parse/Footing.h の extendDeeperCollinearEnds）。
	const std::vector<WallCommand> input = {
		wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 150.0, -100.0),	  // [0] 深い（下端 -100）
		wall(Vec2{3000.0, 0.0}, Vec2{6000.0, 0.0}, 150.0, -70.0), // [1] 浅い・[0] と同一直線
		wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0}, 150.0, -70.0)}; // [2] 直交

	// (1) 深いほう [0] が直交壁 [2] の半壁厚（75mm）だけ伸びる。浅い [1] と [2] は動かない。
	const std::vector<WallCommand> walls = extendDeeperCollinearEnds(input);
	CHECK(near(walls[0].end.x, 3075.0));
	CHECK(near(walls[1].start.x, 3000.0));
	CHECK(near(walls[2].end.y, 3000.0));

	// (2) 結合は「[2] → [0]」の T 1 件だけ（同一直線の [0]-[1] は結合しない）。
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);
	CHECK_EQ(joins.size(), std::size_t{1});
	if (joins.empty())
		return;
	CHECK(joins[0].joinType == core::WallJoinType::T);
	CHECK_EQ(joins[0].a, std::size_t{2}); // stem＝直交する立上り
	CHECK_EQ(joins[0].b, std::size_t{0}); // through＝深いほう
}

TEST(deeper_extension_leaves_a_plain_corner_alone)
{
	// 同一直線の隣が無い普通のコーナーは伸ばさない（伸ばすとコーナーが T になってしまう）。
	const std::vector<WallCommand> input = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
											wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0})};
	const std::vector<WallCommand> walls = extendDeeperCollinearEnds(input);
	CHECK(near(walls[0].end.x, 3000.0));
	CHECK(near(walls[1].start.y, 0.0));
}

TEST(x_joins_are_emitted_last)
{
	// **X 結合はすべて最後**（VW の X 結合は a を分割するので、分割された壁のハンドルを
	// 後の結合が使うと片方だけを相手にしてしまう。parse/Footing.cpp の末尾）。
	// 横の通し壁が交差（内部）と両端の T 結合を持つ形で確かめる。
	const std::vector<WallCommand> walls = {
		wall(Vec2{0.0, 0.0}, Vec2{9000.0, 0.0}), // [0] 横の通し壁（交差＋両端 T）
		wall(Vec2{4500.0, -3000.0}, Vec2{4500.0, 3000.0}), // [1] 交差する縦の壁
		wall(Vec2{0.0, 0.0}, Vec2{0.0, 3000.0}),		   // [2] 左端に突き当たる
		wall(Vec2{9000.0, 0.0}, Vec2{9000.0, 3000.0})};	   // [3] 右端に突き当たる
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);

	CHECK(joins.size() >= 2);
	if (joins.size() < 2)
		return;
	// 最後の 1 件だけが X で、それより前に X は無い。
	CHECK(joins.back().joinType == core::WallJoinType::X);
	for (std::size_t i = 0; i + 1 < joins.size(); ++i)
		CHECK(joins[i].joinType != core::WallJoinType::X);
	// 分割されるのは a＝交差する縦の壁で、丸ごと残るのは b＝バックボーンの通し壁。
	CHECK_EQ(joins.back().a, std::size_t{1});
	CHECK_EQ(joins.back().b, std::size_t{0});
}

TEST(join_of_different_top_heights_caps_and_puts_the_lower_first)
{
	// 天端の違う立上りどうしは、低いほうを a にして高いほうへ結合し端部を閉じる。
	const std::vector<WallCommand> walls = {
		wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 120.0, -100.0, -190.0),
		wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0}, 120.0, -100.0, -500.0)};
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);

	CHECK_EQ(joins.size(), std::size_t{1});
	if (joins.empty())
		return;
	CHECK(joins[0].capped);
	CHECK_EQ(joins[0].a, std::size_t{1}); // offset −500 のほうが低い
	CHECK_EQ(joins[0].b, std::size_t{0});
}

TEST(join_ignores_parallel_and_collinear_walls)
{
	// 同一直線上（統合済みのはず）・側並びの平行壁は結合対象にしない。
	const std::vector<WallCommand> collinear = {wall(Vec2{0.0, 0.0}, Vec2{1000.0, 0.0}),
												wall(Vec2{1000.0, 0.0}, Vec2{3000.0, 0.0})};
	CHECK(buildWallJoinCommands(collinear).empty());

	const std::vector<WallCommand> sideBySide = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
												 wall(Vec2{0.0, 120.0}, Vec2{3000.0, 120.0})};
	CHECK(buildWallJoinCommands(sideBySide).empty());
}

TEST(join_three_walls_at_a_corner_emits_L_then_T)
{
	// 3 本が 1 つの端点コーナーに集まる → はじめの 2 本を L、残りを T（バックボーンへ）。
	// **互いに同一直線でない** 3 本にする（同一直線の組があるとそこに線が続いている＝
	// コーナーではないので、深いほうを通し壁にする別の規則が働く。上のテスト参照）。
	const std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
											wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0}),
											wall(Vec2{3000.0, 0.0}, Vec2{5121.0, 2121.0})};
	const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);

	CHECK_EQ(joins.size(), std::size_t{2});
	if (joins.size() != 2)
		return;
	CHECK(joins[0].joinType == core::WallJoinType::L);
	CHECK(joins[1].joinType == core::WallJoinType::T);
	// 3 本とも同じ交点に集まるので、命令はすべて同じ点を指す。
	CHECK(near(joins[1].point.x, 3000.0) && near(joins[1].point.y, 0.0));
}

TEST(joins_reference_valid_walls_in_the_real_fixtures)
{
	// 実フィクスチャの壁結合は必ず walls の範囲内を指し、2 本は別の立上り
	// （＝命令セットの検証を通る）。並びは決定的。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		Model const model = fixture(name, ok);
		CHECK(ok);
		const std::vector<WallCommand> walls = buildWallCommands(model);
		const std::vector<core::WallJoinCommand> joins = buildWallJoinCommands(walls);
		CHECK(!joins.empty()); // どのフィクスチャも交差する立上りを持つ
		for (const core::WallJoinCommand& join : joins)
		{
			CHECK(join.a < walls.size());
			CHECK(join.b < walls.size());
			CHECK(join.a != join.b);
		}

		core::Document document;
		document.walls = walls;
		document.wallJoins = joins;
		CHECK(core::validateDocument(document));

		const std::vector<core::WallJoinCommand> again = buildWallJoinCommands(walls);
		CHECK_EQ(again.size(), joins.size());
		for (std::size_t i = 0; i < joins.size() && i < again.size(); ++i)
		{
			CHECK_EQ(again[i].a, joins[i].a);
			CHECK_EQ(again[i].b, joins[i].b);
			CHECK(again[i].joinType == joins[i].joinType);
		}
	}
}

// ---------------------------------------------------------------------------
// 端部のキャップ（applyWallCaps）— ROADMAP.md M10（ローカル確認で判明した項目）
// ---------------------------------------------------------------------------

TEST(caps_close_free_ends_and_open_joined_ends)
{
	// L コーナー（同じ天端）: 取り合う端は閉じない（コンクリートで一体）。反対側の
	// 自由端は閉じる。
	std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}),
									  wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0})};
	applyWallCaps(walls, buildWallJoinCommands(walls));

	CHECK(walls[0].capStart);  // 自由端
	CHECK(!walls[0].capEnd);   // コーナー
	CHECK(!walls[1].capStart); // コーナー
	CHECK(walls[1].capEnd);	   // 自由端
}

TEST(caps_open_the_stem_end_of_a_tee)
{
	// T 字（同じ天端）: ぶつかる側（stem）の端は閉じない。通し壁は端部で交わらないので
	// 両端とも自由端のまま閉じる。
	std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{6000.0, 0.0}),
									  wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0})};
	applyWallCaps(walls, buildWallJoinCommands(walls));

	CHECK(walls[0].capStart);
	CHECK(walls[0].capEnd);
	CHECK(!walls[1].capStart); // 通し壁へぶつかる端
	CHECK(walls[1].capEnd);
}

TEST(caps_stay_closed_against_a_wall_of_a_different_top)
{
	// 天端の違う立上りとだけ取り合う端は閉じたまま（低いほうの端部が見える＝Python 版の
	// capped=true と同じ判断）。
	std::vector<WallCommand> walls = {
		wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 120.0, -100.0, -190.0),
		wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0}, 120.0, -100.0, -500.0)};
	applyWallCaps(walls, buildWallJoinCommands(walls));

	CHECK(walls[0].capStart);
	CHECK(walls[0].capEnd);
	CHECK(walls[1].capStart);
	CHECK(walls[1].capEnd);
}

TEST(caps_open_where_the_same_height_walls_meet_at_a_crowded_corner)
{
	// 3 本コーナー（同じ天端の 2 本 ＋ 天端の低い 1 本）: 高い 2 本は互いに閉じない結合を
	// 持つので開き、低い 1 本は閉じる結合しか持たないので閉じたまま。
	std::vector<WallCommand> walls = {
		wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0}, 120.0, -100.0, -190.0),
		wall(Vec2{3000.0, 0.0}, Vec2{3000.0, 3000.0}, 120.0, -100.0, -190.0),
		wall(Vec2{3000.0, 0.0}, Vec2{3000.0, -3000.0}, 120.0, -100.0, -500.0)};
	applyWallCaps(walls, buildWallJoinCommands(walls));

	CHECK(!walls[0].capEnd);
	CHECK(!walls[1].capStart);
	CHECK(walls[2].capStart); // 天端が低い立上りの端部は閉じる
}

TEST(caps_default_to_closed_without_joins)
{
	// 結合が 1 つも無ければ全端が閉じる（孤立した立上り）。
	std::vector<WallCommand> walls = {wall(Vec2{0.0, 0.0}, Vec2{3000.0, 0.0})};
	applyWallCaps(walls, {});
	CHECK(walls[0].capStart);
	CHECK(walls[0].capEnd);
}

// ---------------------------------------------------------------------------
// 地中梁（mergeGroundBeamModifiers / modifierFootprint / attachGroundBeamModifiers）
// — ROADMAP.md M10
// ---------------------------------------------------------------------------

TEST(merge_ground_beams_collinear_touching_into_one)
{
	// 同一軸線・同一断面・同一高さで区間が接する地中梁は 1 本に統合する。
	const std::vector<core::ModifierCommand> merged =
		mergeGroundBeamModifiers({groundBeam(core::Vec3{0.0, 0.0, -240.0}, 0.0, 1000.0),
								  groundBeam(core::Vec3{1000.0, 0.0, -240.0}, 0.0, 2000.0)});

	CHECK_EQ(merged.size(), std::size_t{1});
	if (merged.empty())
		return;
	CHECK(near(merged[0].depth, 3000.0));
	CHECK(near(merged[0].origin.x, 0.0) && near(merged[0].origin.y, 0.0));
	CHECK(near(merged[0].origin.z, -240.0));
	CHECK(near(merged[0].azimuth, 0.0));
	CHECK_EQ(merged[0].profile.size(), std::size_t{4});
}

TEST(merge_ground_beams_keeps_gaps_and_differences)
{
	// 隙間がある／別の軸線上／高さが違う／断面が違う／向きが違う地中梁は統合しない。
	const auto pair = [](core::ModifierCommand a, core::ModifierCommand b)
	{ return mergeGroundBeamModifiers({std::move(a), std::move(b)}).size(); };

	const core::ModifierCommand base = groundBeam(core::Vec3{0.0, 0.0, -240.0}, 0.0, 1000.0);
	// 隙間（10mm 空く）
	CHECK_EQ(pair(base, groundBeam(core::Vec3{1010.0, 0.0, -240.0}, 0.0, 1000.0)), std::size_t{2});
	// 別の軸線（直交距離 500mm の平行線）
	CHECK_EQ(pair(base, groundBeam(core::Vec3{0.0, 500.0, -240.0}, 0.0, 1000.0)), std::size_t{2});
	// 高さが違う
	CHECK_EQ(pair(base, groundBeam(core::Vec3{1000.0, 0.0, -300.0}, 0.0, 1000.0)), std::size_t{2});
	// 向き（方位角）が違う
	CHECK_EQ(pair(base, groundBeam(core::Vec3{1000.0, 0.0, -240.0}, 90.0, 1000.0)), std::size_t{2});
	// 断面が違う（幅の広い台形）
	core::ModifierCommand wide = groundBeam(core::Vec3{1000.0, 0.0, -240.0}, 0.0, 1000.0);
	wide.profile = {Vec2{-200.0, 0.0}, Vec2{200.0, 0.0}, Vec2{300.0, 140.0}, Vec2{-300.0, 140.0}};
	CHECK_EQ(pair(base, wide), std::size_t{2});
}

TEST(modifier_footprint_sweeps_the_section_width)
{
	// 平面外形は「断面の u 範囲」を軸方向へ depth 掃引した矩形。方位角 0 なら軸＝+X・
	// 幅軸＝+Y なので、Y 方向の広がりが断面の**最大幅**（天端の −350〜350）になる。
	const std::vector<Vec2> footprint =
		modifierFootprint(groundBeam(core::Vec3{0.0, 0.0, -240.0}, 0.0, 2000.0));
	CHECK_EQ(footprint.size(), std::size_t{4});
	if (footprint.size() != 4)
		return;
	CHECK(near(minX(footprint), 0.0) && near(maxX(footprint), 2000.0));
	CHECK(near(minY(footprint), -350.0) && near(maxY(footprint), 350.0));
}

TEST(attach_ground_beams_to_the_overlapping_slab)
{
	// 平面外形が重なる底盤へ振り分ける（重なりが無ければ重心が最も近い底盤へ）。
	std::vector<SlabCommand> slabs = {slab(rect(0.0, 0.0, 2000.0, 2000.0)),
									  slab(rect(5000.0, 0.0, 7000.0, 2000.0))};
	const std::vector<core::ModifierCommand> modifiers = {
		groundBeam(core::Vec3{200.0, 1000.0, -240.0}, 0.0, 1600.0),	 // 1 枚目の中
		groundBeam(core::Vec3{5200.0, 1000.0, -240.0}, 0.0, 1600.0), // 2 枚目の中
		groundBeam(core::Vec3{9000.0, 1000.0, -240.0}, 0.0,
				   500.0)}; // どちらの外でも近いのは 2 枚目
	attachGroundBeamModifiers(slabs, modifiers);

	CHECK_EQ(slabs[0].modifiers.size(), std::size_t{1});
	CHECK_EQ(slabs[1].modifiers.size(), std::size_t{2});
	// 底盤が 1 枚も無ければ付けようがない（落として先へ進む）。
	std::vector<SlabCommand> none;
	attachGroundBeamModifiers(none, modifiers);
	CHECK(none.empty());
}

TEST(ground_beam_ends_snap_to_the_slab_edge)
{
	// 底盤の外形は立上りの**外面**まで広げてあるのに、地中梁の端は立上りの**壁芯**で
	// 止まっているものがあり、角がずれる。端を外形の縁まで伸ばす（parse/Footing.h の
	// extendModifierEndsToBoundary。ROADMAP.md M10）。
	const std::vector<Vec2> boundary = rect(0.0, 0.0, 6000.0, 2000.0);

	// (1) 両端が 75mm 手前で止まっている → どちらも縁まで伸びる。
	const core::ModifierCommand shortBoth = extendModifierEndsToBoundary(
		groundBeam(core::Vec3{75.0, 1000.0, -240.0}, 0.0, 5850.0), boundary);
	CHECK(near(shortBoth.origin.x, 0.0));
	CHECK(near(shortBoth.depth, 6000.0));

	// (2) すでに縁に届いている端は動かさない（伸ばすと底盤の外へはみ出す）。
	const core::ModifierCommand exact = extendModifierEndsToBoundary(
		groundBeam(core::Vec3{0.0, 1000.0, -240.0}, 0.0, 6000.0), boundary);
	CHECK(near(exact.origin.x, 0.0));
	CHECK(near(exact.depth, 6000.0));

	// (3) 底盤の中ほどで終わっている端は動かさない（縁が kGroundBeamEndReach より遠い）。
	const core::ModifierCommand inside = extendModifierEndsToBoundary(
		groundBeam(core::Vec3{2000.0, 1000.0, -240.0}, 0.0, 2000.0), boundary);
	CHECK(near(inside.origin.x, 2000.0));
	CHECK(near(inside.depth, 2000.0));

	// (4) 片方だけ手前で止まっている場合はその端だけ伸びる（-90 度＝-Y 方向）。
	const core::ModifierCommand oneEnd = extendModifierEndsToBoundary(
		groundBeam(core::Vec3{3000.0, 2000.0, -240.0}, -90.0, 1925.0), boundary);
	CHECK(near(oneEnd.origin.y, 2000.0));
	CHECK(near(oneEnd.depth, 2000.0));
}

TEST(ground_beams_of_the_real_fixtures_land_on_slabs)
{
	// 実フィクスチャ: 地中梁は 1 本も取りこぼさず底盤へ付く。断面は下端が v=0 で天端が
	// 正（下り梁）、押し出し長は正。命令セットの検証も通る。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		Model const model = fixture(name, ok);
		CHECK(ok);
		Context context(model);
		const std::vector<WallCommand> walls = buildWallCommands(model);
		const std::vector<core::ModifierCommand> modifiers =
			HomeskzIfcImport::parse::buildGroundBeamModifiers(model, context.gridCenter());
		const std::vector<SlabCommand> slabs = buildSlabCommands(context, walls);

		std::size_t attached = 0;
		for (const SlabCommand& command : slabs)
			attached += command.modifiers.size();
		CHECK_EQ(attached, modifiers.size());

		for (const core::ModifierCommand& modifier : modifiers)
		{
			CHECK(modifier.depth > 0.0);
			CHECK(modifier.profile.size() >= 3);
			double vMin = modifier.profile.front().y;
			double vMax = modifier.profile.front().y;
			for (const Vec2& point : modifier.profile)
			{
				vMin = std::min(vMin, point.y);
				vMax = std::max(vMax, point.y);
			}
			CHECK(near(vMin, 0.0)); // 断面原点＝梁下端
			CHECK(vMax > 0.0);
		}

		core::Document document;
		document.walls = walls;
		document.slabs = slabs;
		CHECK(core::validateDocument(document));
	}
}

TEST(ground_beam_tops_meet_the_slab_bottom)
{
	// 地中梁は底盤からぶら下がる下り梁なので、天端（origin.z + 最大 v）が底盤の底面
	// （底盤天端 − 厚み）に一致する。可視ソリッドの呑み込み（core::raiseModifierTop）が
	// 要るのはこの coplanar のため（draw/Footing.cpp）。
	bool ok = false;
	Model const model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	Context context(model);
	const std::vector<SlabCommand> slabs = buildSlabCommands(context, context.walls());

	std::size_t checked = 0;
	for (const SlabCommand& command : slabs)
	{
		const double slabBottom = command.elevation - command.thickness;
		for (const core::ModifierCommand& modifier : command.modifiers)
		{
			double vMax = modifier.profile.front().y;
			for (const Vec2& point : modifier.profile)
				vMax = std::max(vMax, point.y);
			CHECK(near(modifier.origin.z + vMax, slabBottom, 1.0));
			++checked;
		}
	}
	CHECK(checked > 0);
}

TEST_MAIN()
