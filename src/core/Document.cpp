//
//	core/Document.cpp
//
//	validateDocument の実装。Python 版 document.py の validateDocument に対応する。
//	SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//
//	骨組みの現状では Document は「バージョン＋空の器」なので、検証はバージョンの
//	妥当性だけを見る。各命令リスト（grids / stories / members …）が追加されるたびに、
//	対応する検証規則（必須フィールドの有無・参照整合性・値域）をここへ足していく。
//

#include "core/Document.h"

#include <algorithm>
#include <cmath>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 2 点が実質同一か（縮退した通り芯＝始点と終点が同じ、を弾く判定に使う）。
		// 座標は mm。Python 版のように厳密一致ではなく微小許容で見る（丸め耐性）。
		bool isDegenerate(const Vec2& a, const Vec2& b)
		{
			constexpr double kEps = 1e-6;
			return std::abs(a.x - b.x) < kEps && std::abs(a.y - b.y) < kEps;
		}
	} // namespace

	bool validateDocument(const Document& document)
	{
		if (document.version != kDocumentVersion)
			return false;

		// 通り芯: 配置先レイヤ名が空でなく、始点と終点が異なる（縮退していない）こと。
		// クラス名は空でもよい（無クラス＝既定クラスへ）。1 本でも不正なら描画しない
		// （Python 版 validateDocument と同じ関門。ROADMAP.md M1）。
		//
		// TODO: 命令リストが増えたら、要素ごとの all_of を && で連ねてここに積む
		// （story / member … の検証。ROADMAP.md）。
		return std::ranges::all_of(
			document.grids, [](const GridCommand& grid)
			{ return !grid.layer.empty() && !isDegenerate(grid.start, grid.end); });
	}
} // namespace HomeskzIfcImport::core
