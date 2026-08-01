//
//	CoreRegionTests.cpp
//
//	平面領域の合成（src/core/Region の filledUnionOutlines）の単体テスト。
//	VectorWorks SDK も STEP も使わない純粋な幾何計算なので、無 SDK のテスト
//	ハーネス（TestFramework.h）だけで完結する（CLAUDE.md「テスト方針」）。
//
//	検証項目: 骨組みが囲む領域の外形（囲まれた空隙＋それを囲う部品）、囲んでいない
//	部品（突き出し・孤立）の除外、連結成分の分離、共線点の除去、縮退入力の扱い。
//	用途はロフト（屋根階の床）の外形合成（parse/Floor。ROADMAP.md M5）。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Region.h"

#include <algorithm>
#include <cstddef>
#include <vector>

using HomeskzIfcImport::core::filledUnionOutlines;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcTests::near;

namespace
{
	// 軸平行の矩形（中心・幅・高さ）を反時計回りの 4 点で作る。
	std::vector<Vec2> rect(double cx, double cy, double dx, double dy)
	{
		const double hx = dx * 0.5;
		const double hy = dy * 0.5;
		return {Vec2{cx - hx, cy - hy}, Vec2{cx + hx, cy - hy}, Vec2{cx + hx, cy + hy},
				Vec2{cx - hx, cy + hy}};
	}

	// 外形 4 本で閉じた矩形リング（内側が空隙）。外周は ±550、部材幅は 100。
	std::vector<std::vector<Vec2>> ring()
	{
		return {rect(0.0, -500.0, 1100.0, 100.0), rect(0.0, 500.0, 1100.0, 100.0),
				rect(-500.0, 0.0, 100.0, 1100.0), rect(500.0, 0.0, 100.0, 1100.0)};
	}

	// 外形のバウンディングボックス。
	struct Box
	{
		double minX = 0.0;
		double maxX = 0.0;
		double minY = 0.0;
		double maxY = 0.0;
	};

	Box boxOf(const std::vector<Vec2>& outline)
	{
		Box b{outline[0].x, outline[0].x, outline[0].y, outline[0].y};
		for (const Vec2& p : outline)
		{
			b.minX = std::min(b.minX, p.x);
			b.maxX = std::max(b.maxX, p.x);
			b.minY = std::min(b.minY, p.y);
			b.maxY = std::max(b.maxY, p.y);
		}
		return b;
	}

	// 多角形の符号付き面積（反時計回りで正）。
	double area(const std::vector<Vec2>& outline)
	{
		double sum = 0.0;
		const std::size_t n = outline.size();
		for (std::size_t i = 0, j = n - 1; i < n; j = i++)
			sum += (outline[j].x * outline[i].y) - (outline[i].x * outline[j].y);
		return sum * 0.5;
	}
} // namespace

TEST(closed_ring_becomes_one_filled_outline)
{
	// 4 本で閉じた骨組み → 内側の空隙が埋まり、外形は外周の矩形 1 つ（共線点は落ちる）。
	const std::vector<std::vector<Vec2>> outlines = filledUnionOutlines(ring());
	CHECK_EQ(outlines.size(), static_cast<std::size_t>(1));
	if (outlines.empty())
		return;
	CHECK_EQ(outlines[0].size(), static_cast<std::size_t>(4));
	const Box box = boxOf(outlines[0]);
	CHECK(near(box.minX, -550.0));
	CHECK(near(box.maxX, 550.0));
	CHECK(near(box.minY, -550.0));
	CHECK(near(box.maxY, 550.0));
	// 向きは反時計回り（面積が正）。
	CHECK(area(outlines[0]) > 0.0);
	CHECK(near(area(outlines[0]), 1100.0 * 1100.0));
}

TEST(inner_joists_do_not_change_the_outline)
{
	// リングの内側に根太を渡しても、囲まれた空隙が埋まるので外形は変わらない。
	std::vector<std::vector<Vec2>> parts = ring();
	parts.push_back(rect(0.0, 0.0, 100.0, 900.0));
	parts.push_back(rect(-200.0, 0.0, 100.0, 900.0));

	const std::vector<std::vector<Vec2>> outlines = filledUnionOutlines(parts);
	CHECK_EQ(outlines.size(), static_cast<std::size_t>(1));
	if (outlines.empty())
		return;
	CHECK_EQ(outlines[0].size(), static_cast<std::size_t>(4));
	CHECK(near(area(outlines[0]), 1100.0 * 1100.0));
}

TEST(protruding_member_is_excluded)
{
	// 骨組みから外へ突き出しただけの部材は、どの空隙も囲っていないので床にならない
	// （外形に部材 1 本分の幅のヒゲが生えない）。
	std::vector<std::vector<Vec2>> parts = ring();
	parts.push_back(rect(1000.0, 0.0, 900.0, 100.0)); // 右へ 1450 まで突き出す

	const std::vector<std::vector<Vec2>> outlines = filledUnionOutlines(parts);
	CHECK_EQ(outlines.size(), static_cast<std::size_t>(1));
	if (outlines.empty())
		return;
	const Box box = boxOf(outlines[0]);
	CHECK(near(box.maxX, 550.0)); // 突き出しは含まれない
	CHECK(near(area(outlines[0]), 1100.0 * 1100.0));
}

TEST(isolated_members_make_no_region)
{
	// 何も囲っていない単独の部材だけなら領域はできない（床にしない）。
	CHECK(filledUnionOutlines({rect(0.0, 0.0, 1000.0, 100.0)}).empty());
	// 平行な 2 本（閉じていない）も同じ。
	CHECK(filledUnionOutlines({rect(0.0, -500.0, 1000.0, 100.0), rect(0.0, 500.0, 1000.0, 100.0)})
			  .empty());
}

TEST(separate_rings_become_separate_outlines)
{
	// 離れた 2 つの骨組みは別々の床になる。
	std::vector<std::vector<Vec2>> parts = ring();
	for (const std::vector<Vec2>& part : ring())
	{
		std::vector<Vec2> moved = part;
		for (Vec2& p : moved)
			p.x += 3000.0;
		parts.push_back(moved);
	}

	const std::vector<std::vector<Vec2>> outlines = filledUnionOutlines(parts);
	CHECK_EQ(outlines.size(), static_cast<std::size_t>(2));
	if (outlines.size() != 2)
		return;
	// 並びはセル走査順（Y 昇順 → X 昇順）なので、左（X が小さい方）が先。
	CHECK(near(boxOf(outlines[0]).minX, -550.0));
	CHECK(near(boxOf(outlines[1]).minX, 2450.0));
}

TEST(degenerate_input_is_empty)
{
	// 部品なし・面を持たない部品（2 点以下）・面積ゼロ（全点が一直線）は空を返す。
	CHECK(filledUnionOutlines({}).empty());
	CHECK(filledUnionOutlines({{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}}}).empty());
	CHECK(filledUnionOutlines({{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}, Vec2{200.0, 0.0}}}).empty());
}

TEST_MAIN();
