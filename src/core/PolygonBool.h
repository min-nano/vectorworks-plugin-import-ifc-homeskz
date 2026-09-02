//
//	core/PolygonBool.h
//
//	平面多角形の集合演算（和・差）と、面で繋がる多角形の連結成分。**基礎の外形を仕様ごとに
//	まとめる**（同じ厚み・同じ高さの底盤／同じ天端の立上り）ときと、**底盤の砕石から地中梁を
//	抜く**ときの両方がここを通る（docs/DEV-NOTES.md M21）。
//
//	【ひとつの手順で和も差も出す】どちらも
//	  1. 全多角形の辺を、他のすべての辺との交点で細分する
//	  2. 細分した辺のうち「**左が領域の内・右が領域の外**」の向きのものだけを境界として残す
//	  3. 残った有向辺をつないで閉ループにする
//	という同じ手順で、違うのは 2 の「領域」の判定だけ（和＝どれかの内側／差＝引かれる側の
//	内側かつ引く側の外側）。頂点は 1e-4mm に丸めて厳密比較できるようにするので、集合・辞書は
//	素直な std::set / std::map（辞書順比較）で足りる。
//
//	【穴の扱い】結果が穴を持つ（時計回りのループが混じる）ことはある。押し出しソリッドは
//	穴を表せないので、**呼び出し側が向きを見て判断する**（core/Foundation は穴が出たら
//	引き算をあきらめて元の外形を使う）。ここでは向きをそのまま返す——外側のループは反時計回り、
//	穴は時計回り。
//
//	【SDK 非依存】このヘッダは core/Geometry.h と標準ライブラリだけに依存する。
//

#pragma once

#include "core/Geometry.h"

#include <cstddef>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 多角形（閉じた頂点列。末尾に始点を重複させない）の列。
	using PolygonList = std::vector<std::vector<Vec2>>;

	// 集合演算の許容値。順に 距離（mm。同一直線・重なりの判定）・平行判定（sin 角）・
	// 境界辺の「すぐ左右」を見るサンプル距離（mm。部材寸法より十分小さく、頂点の丸め
	// （1e-4mm）より十分大きい）。
	inline constexpr double kPolyDistTol = 1.0;
	inline constexpr double kPolyAngleTol = 1e-3;
	inline constexpr double kPolySideEps = 1e-2;

	// 多角形群の**和**の境界ループ。向きは問わない（内部で揃える）。境界がつながらない
	// （開ループになる）異常な入力では false（out は変更しない）。
	bool polygonUnion(const PolygonList& polys, PolygonList& out);

	// **差**（subject − clip）の境界ループ。subject が空なら out も空で true。
	// 結果が穴を含むことがある（外側のループは反時計回り・穴は時計回り）。
	bool polygonDifference(const PolygonList& subject, const PolygonList& clip, PolygonList& out);

	// 2 つの多角形が「面で繋がる」か（辺を共有する／内部で交差する／一方が他方の内部に
	// 頂点を持つ）。角（点）だけで接する場合は繋がっていないとみなす——升目状に並ぶ布基礎の
	// 底盤を 1 枚のベタに畳んでしまわないため。
	bool polygonsConnected(const std::vector<Vec2>& a, const std::vector<Vec2>& b);

	// 面で繋がる多角形の連結成分（添字の集合）。成分は昇順・成分内も昇順で決定的。
	std::vector<std::vector<std::size_t>> polygonComponents(const PolygonList& polys);

	// 繋がる多角形どうしを和で 1 枚へ畳む。**和が単一の穴なしループになった成分だけ**畳み、
	// そうでない成分（升目状に囲んで穴ができる・複数の外形に分かれる・和の計算に失敗した）は
	// 元の多角形をそのまま残す。穴を無視して外形だけ採ると、部屋の下まで底盤で埋めた形に
	// なってしまう（M9 の実測）。結果の並びは成分順・成分内は入力順で決定的。
	PolygonList mergePolygons(const PolygonList& polys);
} // namespace HomeskzIfcImport::core
