//
//	CoreUnionFindTests.cpp
//
//	連結成分（core/UnionFind）のテスト。立上り・大引・地中梁の統合と壁結合の交点クラスタが
//	共有する骨格なので、**決定性（代表＝最小インデックス・代表昇順・成分内昇順）**を
//	ここで固定する——この並びが崩れると 4 要素すべての出力順が変わる。
//

#include "core/UnionFind.h"

#include "TestFramework.h"

#include <cstddef>
#include <vector>

using HomeskzIfcImport::core::connectedComponents;
using HomeskzIfcImport::core::findRoot;

TEST(find_root_compresses_path)
{
	// 0 <- 1 <- 2 <- 3 の鎖。根は 0 で、経路圧縮後も答えは変わらない。
	std::vector<std::size_t> parent{0, 0, 1, 2};
	CHECK_EQ(findRoot(parent, 3), static_cast<std::size_t>(0));
	CHECK_EQ(findRoot(parent, 3), static_cast<std::size_t>(0)); // 圧縮後も同じ
	CHECK_EQ(findRoot(parent, 0), static_cast<std::size_t>(0));
}

TEST(connected_components_empty_input)
{
	const auto components = connectedComponents(0, [](std::size_t, std::size_t) { return true; });
	CHECK(components.empty());
}

TEST(connected_components_all_isolated)
{
	// どの対もつながらない: 1 要素 1 成分がインデックス昇順で並ぶ。
	const auto components = connectedComponents(3, [](std::size_t, std::size_t) { return false; });
	CHECK_EQ(components.size(), static_cast<std::size_t>(3));
	for (std::size_t i = 0; i < components.size(); ++i)
	{
		CHECK_EQ(components[i].size(), static_cast<std::size_t>(1));
		CHECK_EQ(components[i].front(), i);
	}
}

TEST(connected_components_chain_merges_transitively)
{
	// 0-1, 1-2 だけがつながる（0-2 は直接つながらない）: 推移的に 1 成分になる。
	const auto connected = [](std::size_t i, std::size_t j)
	{ return (i == 0 && j == 1) || (i == 1 && j == 2); };
	const auto components = connectedComponents(4, connected);
	CHECK_EQ(components.size(), static_cast<std::size_t>(2));
	CHECK_EQ(components[0].size(), static_cast<std::size_t>(3));
	// 代表（front）は成分の最小インデックス。成分内はインデックス昇順。
	CHECK_EQ(components[0][0], static_cast<std::size_t>(0));
	CHECK_EQ(components[0][1], static_cast<std::size_t>(1));
	CHECK_EQ(components[0][2], static_cast<std::size_t>(2));
	CHECK_EQ(components[1].front(), static_cast<std::size_t>(3));
}

TEST(connected_components_representative_is_smallest_regardless_of_pair_order)
{
	// 後方の対（2-3）が先に結ばれ、その後に前方（0-3）が合流しても、代表は最小の 0。
	const auto connected = [](std::size_t i, std::size_t j)
	{ return (i == 2 && j == 3) || (i == 0 && j == 3); };
	const auto components = connectedComponents(4, connected);
	CHECK_EQ(components.size(), static_cast<std::size_t>(2));
	CHECK_EQ(components[0].front(), static_cast<std::size_t>(0));
	CHECK_EQ(components[0].size(), static_cast<std::size_t>(3)); // {0, 2, 3}
	CHECK_EQ(components[0][1], static_cast<std::size_t>(2));
	CHECK_EQ(components[0][2], static_cast<std::size_t>(3));
	CHECK_EQ(components[1].front(), static_cast<std::size_t>(1));
}

TEST_MAIN()
