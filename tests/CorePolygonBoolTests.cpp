//
//	CorePolygonBoolTests.cpp
//
//	平面多角形の集合演算（src/core/PolygonBool）の単体テスト。VectorWorks SDK も STEP も
//	使わない純粋な幾何なので、期待値はすべて手書きで持つ（CLAUDE.md「テスト方針」）。
//
//	検証項目:
//	  * 和（polygonUnion）… 辺を共有する矩形・面で重なる矩形・離れた矩形・穴のできる升目
//	  * 差（polygonDifference）… 帯で 2 つに割る・完全に覆う・穴が開く（時計回りのループ）
//	  * 繋がり（polygonsConnected）… 辺の共有・重なり・角だけの接触・離れている
//	  * 連結成分（polygonComponents）と、畳めるものだけ畳む mergePolygons
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Geometry.h"
#include "core/PolygonBool.h"

#include <algorithm>
#include <cstddef>
#include <vector>

using HomeskzIfcImport::core::PolygonList;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcTests::near;

namespace
{
	std::vector<Vec2> rect(double x1, double y1, double x2, double y2)
	{
		return {Vec2{x1, y1}, Vec2{x2, y1}, Vec2{x2, y2}, Vec2{x1, y2}};
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

	// ループの向きを見る（反時計回り＝外形・時計回り＝穴）。
	std::size_t holesIn(const PolygonList& loops)
	{
		return static_cast<std::size_t>(
			std::ranges::count_if(loops, [](const std::vector<Vec2>& loop)
								  { return HomeskzIfcImport::core::shoelaceSigned(loop) < 0.0; }));
	}
} // namespace

TEST(union_merges_touching_and_overlapping_polygons)
{
	PolygonList out;

	// 辺を共有する 2 枚（右向きに並ぶ）は 1 枚の矩形になり、共有辺は消える。
	CHECK(HomeskzIfcImport::core::polygonUnion(
		{rect(0.0, 0.0, 100.0, 100.0), rect(100.0, 0.0, 200.0, 100.0)}, out));
	CHECK_EQ(out.size(), std::size_t{1});
	if (out.size() != 1)
		return;
	CHECK_EQ(out[0].size(), std::size_t{4}); // 共線の中間点は落とす
	CHECK(near(minX(out[0]), 0.0) && near(maxX(out[0]), 200.0));
	CHECK(near(minY(out[0]), 0.0) && near(maxY(out[0]), 100.0));

	// 面で重なる 2 枚は L 字（入隅を挟んで 8 頂点）。
	CHECK(HomeskzIfcImport::core::polygonUnion(
		{rect(0.0, 0.0, 100.0, 100.0), rect(50.0, 50.0, 200.0, 200.0)}, out));
	CHECK_EQ(out.size(), std::size_t{1});
	if (out.size() != 1)
		return;
	CHECK_EQ(out[0].size(), std::size_t{8});

	// 離れた 2 枚は 2 つのループのまま。
	CHECK(HomeskzIfcImport::core::polygonUnion(
		{rect(0.0, 0.0, 100.0, 100.0), rect(500.0, 0.0, 600.0, 100.0)}, out));
	CHECK_EQ(out.size(), std::size_t{2});

	// 向きは問わない（時計回りで渡しても同じ和になる）。
	std::vector<Vec2> clockwise = rect(100.0, 0.0, 200.0, 100.0);
	std::ranges::reverse(clockwise);
	CHECK(HomeskzIfcImport::core::polygonUnion({rect(0.0, 0.0, 100.0, 100.0), clockwise}, out));
	CHECK_EQ(out.size(), std::size_t{1});
}

TEST(union_of_a_ring_of_bands_keeps_the_hole)
{
	// 部屋を囲む 4 本の帯（布基礎の升目）は、和が外形＋穴の 2 ループになる。**外形だけ
	// 採ってはいけない**（部屋の下までコンクリートで埋めた形になる）ので、呼び出し側が
	// 向きを見て判断できるよう、穴は時計回りのループとして返る。
	PolygonList out;
	CHECK(HomeskzIfcImport::core::polygonUnion(
		{rect(0.0, 0.0, 1000.0, 100.0), rect(0.0, 900.0, 1000.0, 1000.0),
		 rect(0.0, 0.0, 100.0, 1000.0), rect(900.0, 0.0, 1000.0, 1000.0)},
		out));
	CHECK_EQ(out.size(), std::size_t{2});
	CHECK_EQ(holesIn(out), std::size_t{1});
}

TEST(difference_cuts_the_subject_and_reports_holes)
{
	PolygonList out;

	// 横断する帯は 1 枚を 2 枚に割る。
	CHECK(HomeskzIfcImport::core::polygonDifference({rect(0.0, 0.0, 1000.0, 500.0)},
													{rect(400.0, 0.0, 600.0, 500.0)}, out));
	CHECK_EQ(out.size(), std::size_t{2});
	CHECK_EQ(holesIn(out), std::size_t{0});
	std::ranges::sort(out, [](const std::vector<Vec2>& a, const std::vector<Vec2>& b)
					  { return minX(a) < minX(b); });
	CHECK(near(maxX(out[0]), 400.0) && near(minX(out[1]), 600.0));

	// 内側だけを抜くと穴（時計回りのループ）になる。
	CHECK(HomeskzIfcImport::core::polygonDifference({rect(0.0, 0.0, 1000.0, 1000.0)},
													{rect(400.0, 400.0, 600.0, 600.0)}, out));
	CHECK_EQ(out.size(), std::size_t{2});
	CHECK_EQ(holesIn(out), std::size_t{1});

	// 覆い尽くせば何も残らない。
	CHECK(HomeskzIfcImport::core::polygonDifference({rect(0.0, 0.0, 100.0, 100.0)},
													{rect(-10.0, -10.0, 110.0, 110.0)}, out));
	CHECK(out.empty());

	// 引く相手が無ければそのまま。
	CHECK(HomeskzIfcImport::core::polygonDifference({rect(0.0, 0.0, 100.0, 100.0)}, {}, out));
	CHECK_EQ(out.size(), std::size_t{1});
	CHECK(near(maxX(out[0]), 100.0));

	// 触れているだけ（辺を共有）なら削れない。
	CHECK(HomeskzIfcImport::core::polygonDifference({rect(0.0, 0.0, 100.0, 100.0)},
													{rect(100.0, 0.0, 200.0, 100.0)}, out));
	CHECK_EQ(out.size(), std::size_t{1});
	CHECK(near(maxX(out[0]), 100.0));
}

TEST(connected_is_true_for_shared_edges_and_overlaps_but_not_corners)
{
	CHECK(HomeskzIfcImport::core::polygonsConnected(rect(0.0, 0.0, 100.0, 100.0),
													rect(100.0, 0.0, 200.0, 100.0)));
	CHECK(HomeskzIfcImport::core::polygonsConnected(rect(0.0, 0.0, 100.0, 100.0),
													rect(50.0, 50.0, 200.0, 200.0)));
	// 角だけで接する（升目の対角）は繋がっていない。
	CHECK(!HomeskzIfcImport::core::polygonsConnected(rect(0.0, 0.0, 100.0, 100.0),
													 rect(100.0, 100.0, 200.0, 200.0)));
	// 離れている。
	CHECK(!HomeskzIfcImport::core::polygonsConnected(rect(0.0, 0.0, 100.0, 100.0),
													 rect(500.0, 0.0, 600.0, 100.0)));
}

TEST(components_group_only_what_touches)
{
	const PolygonList polys = {rect(0.0, 0.0, 100.0, 100.0), rect(500.0, 0.0, 600.0, 100.0),
							   rect(100.0, 0.0, 200.0, 100.0)};
	const std::vector<std::vector<std::size_t>> comps =
		HomeskzIfcImport::core::polygonComponents(polys);
	CHECK_EQ(comps.size(), std::size_t{2});
	if (comps.size() != 2)
		return;
	// 成分は代表（最小の添字）順・成分内は昇順で決定的。
	CHECK_EQ(comps[0].size(), std::size_t{2});
	CHECK_EQ(comps[0][0], std::size_t{0});
	CHECK_EQ(comps[0][1], std::size_t{2});
	CHECK_EQ(comps[1][0], std::size_t{1});
}

TEST(merge_folds_only_what_becomes_one_hole_free_loop)
{
	// 繋がる 2 枚は 1 枚に、離れた 1 枚はそのまま。
	const PolygonList merged = HomeskzIfcImport::core::mergePolygons(
		{rect(0.0, 0.0, 100.0, 100.0), rect(100.0, 0.0, 200.0, 100.0),
		 rect(500.0, 0.0, 600.0, 100.0)});
	CHECK_EQ(merged.size(), std::size_t{2});
	if (merged.size() != 2)
		return;
	CHECK(near(maxX(merged[0]), 200.0));
	CHECK(near(minX(merged[1]), 500.0));

	// 穴のできる升目は畳まず、元の 4 枚が残る（部屋の下を埋めない）。
	const PolygonList ring = HomeskzIfcImport::core::mergePolygons(
		{rect(0.0, 0.0, 1000.0, 100.0), rect(0.0, 900.0, 1000.0, 1000.0),
		 rect(0.0, 0.0, 100.0, 1000.0), rect(900.0, 0.0, 1000.0, 1000.0)});
	CHECK_EQ(ring.size(), std::size_t{4});

	// 入力が空なら空。
	CHECK(HomeskzIfcImport::core::mergePolygons({}).empty());
}

TEST_MAIN()
