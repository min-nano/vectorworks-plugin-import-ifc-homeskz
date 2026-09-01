//
//	core/UnionFind.h
//
//	ペア述語による連結成分（Union-Find）。**この骨格はここに 1 つだけ置く**——立上りの
//	同一直線統合・壁結合の交点クラスタ・地中梁の同一軸線統合（いずれも parse/Footing）と
//	大引の継手統合（parse/FloorPost）が同じ「経路圧縮つき Union-Find ＋ 代表昇順の成分
//	収集」を各々書いており、決定性の担保（代表＝最小インデックス）が 4 か所に分散していた。
//
//	何を「つながっている」とみなすか（同一直線・距離・すき間の許容値）は**呼び出し側の
//	述語が持つ**。要素ごとに意味の違う許容値をここへ統合しないこと。
//
//	【SDK 非依存】標準 C++ のみに依存（CLAUDE.md「Phase 1」。core/ は VectorWorks SDK を
//	include しない）。純粋な計算なので無 SDK テストで検証する。
//

#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <vector>

namespace HomeskzIfcImport::core
{
	// Union-Find の根を返す（経路圧縮つき）。
	inline std::size_t findRoot(std::vector<std::size_t>& parent, std::size_t index)
	{
		while (parent[index] != index)
		{
			parent[index] = parent[parent[index]];
			index = parent[index];
		}
		return index;
	}

	// インデックス [0, count) を対称な述語 connected(i, j) で結んだ連結成分を返す。
	// 成分の代表は最小インデックスで、外側は代表昇順・成分内はインデックス昇順
	// ＝入力順に依存しない決定的な並び（先頭要素 front() が常に代表になる）。
	// 述語は i < j の組に対して 1 回だけ呼ばれる（O(n²)。要素数は部材数程度の前提。
	// 何度も呼ぶので転送参照ではなく const 参照で受ける）。
	template <class Connected>
	std::vector<std::vector<std::size_t>> connectedComponents(std::size_t count,
															  const Connected& connected)
	{
		std::vector<std::size_t> parent(count);
		for (std::size_t i = 0; i < count; ++i)
			parent[i] = i;

		for (std::size_t i = 0; i < count; ++i)
		{
			for (std::size_t j = i + 1; j < count; ++j)
			{
				if (!connected(i, j))
					continue;
				const std::size_t ri = findRoot(parent, i);
				const std::size_t rj = findRoot(parent, j);
				// 代表は常に小さい方のインデックス＝入力順に依存しない決定的なまとめ方。
				if (ri != rj)
					parent[std::max(ri, rj)] = std::min(ri, rj);
			}
		}

		// std::map なので代表インデックス昇順で走る（出力の並びが決定的になる）。
		std::map<std::size_t, std::vector<std::size_t>> byRoot;
		for (std::size_t i = 0; i < count; ++i)
			byRoot[findRoot(parent, i)].push_back(i);

		std::vector<std::vector<std::size_t>> components;
		components.reserve(byRoot.size());
		for (auto& [root, indices] : byRoot)
			components.push_back(std::move(indices));
		return components;
	}
} // namespace HomeskzIfcImport::core
