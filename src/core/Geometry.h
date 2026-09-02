//
//	core/Geometry.h
//
//	フェーズ非依存の自前幾何型。parse/ は VectorWorks SDK を include しないため、SDK
//	の幾何型（WorldPt3 / TransformMatrix 等）を使えない。配置行列・断面・押し出しの数式は、
//	このヘッダの Vec2 / Vec3 / Mat4 の上に自前で組む（詳細は docs/DEV-NOTES.md M2）。
//
//	提供する型:
//	  * Vec2 / Vec3 … 平面／空間ベクトル（点・方向の双方に使う）と基本演算。
//	  * Mat4       … 4x4 同次変換行列。IfcLocalPlacement / IfcAxis2Placement の
//	                 合成（回転＋平行移動）を表す。剪断・スケールは持たない
//	                 （ホームズ君 IFC の配置は剛体変換のみ）。
//
//	【SDK 非依存】このヘッダは標準 C++（<array>/<cmath>）だけに依存し、SDK も
//	STEP も知らない。純粋な数値計算なので無 SDK テスト（GeometryTests）で検証する。
//

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 座標比較・正規化のゼロ割り回避に使う微小値（mm 単位系での実用的な下限）。
	inline constexpr double kGeomEps = 1e-9;

	// 平面座標が「同じ点」とみなせる許容（mm）。IFC 由来の座標は丸め誤差を含むので
	// 厳密一致では比べない。kGeomEps（ゼロ割り回避の下限）より粗い、座標としての同値判定。
	inline constexpr double kPointEps = 1e-6;

	// 2 次元ベクトル（平面上の点・方向）。通り芯やプロファイル外形の座標に使う。
	struct Vec2
	{
		double x = 0.0;
		double y = 0.0;
	};

	// 2 つの平面座標が実質同一か（通り芯の重複線除去と縮退判定に使う）。**この述語と
	// 閾値はここに 1 つだけ置く**: かつて core/Document.cpp の isDegenerate と
	// parse/Grid.cpp の samePoint が同じ式・同じ 1e-6 を各々持っており、片方の閾値を
	// 直すと「重複として畳んだ線が検証では非縮退」のような食い違いが起こり得た。
	inline bool samePoint(const Vec2& a, const Vec2& b, double tol = kPointEps)
	{
		return std::abs(a.x - b.x) < tol && std::abs(a.y - b.y) < tol;
	}

	// 多角形の符号付き面積（CCW で正・CW で負。閉じた頂点列で、末尾に始点を重複させない）。
	// **向きの判定と面積の重み付けはここに 1 つだけ置く**——底盤の外面合わせ（parse/Footing）と
	// 基礎の部品（core/Foundation）が同じ式を各々持っていた。
	double shoelaceSigned(const std::vector<Vec2>& polygon);

	// 面積（絶対値）。代表値を面積で重み付けするときに使う。
	inline double polygonArea(const std::vector<Vec2>& polygon)
	{
		return std::abs(shoelaceSigned(polygon));
	}

	// 点が多角形の内側か（水平レイキャストの偶奇判定）。辺の上は「どちらか」に落ちる
	// （境界そのものを問う用途では使わない）。地中梁を底盤へ振り分ける・辺が外周かを見る、
	// といった「どの外形の中か」の判定が共有する。
	bool pointInPolygon(const Vec2& point, const std::vector<Vec2>& polygon);

	// 点 p・方向 d の 2 直線の交点（平行なら false）。辺をオフセットした線どうしのマイター
	// （下記 offsetPolygon）と、床付けの帯の頂点計算が共有する。
	bool lineIntersection(const Vec2& p1, const Vec2& d1, const Vec2& p2, const Vec2& d2,
						  Vec2& out);

	// **CCW 多角形の辺 i を外向きへ dists[i] だけ動かした頂点列**。隣り合う移動後の辺（直線）の
	// 交点を新しい頂点にするので、凸角は外側へ伸び、凹角（入隅）は詰まる。辺ごとに距離を変えられる
	// ので、「立上りに沿う辺だけ半壁厚だけ広げる」「外周部の辺だけ張り出す」の両方に使える。
	// 平行な連続辺（同一直線の分割）はマイターが求まらないので、法線方向へずらした点で代用する。
	// **入力は CCW 前提**（呼び出し側で shoelaceSigned を見て揃える）。dists の数が辺の数
	// （＝頂点の数）と違う／面にならない入力は、動かさずそのまま返す。
	std::vector<Vec2> offsetPolygon(const std::vector<Vec2>& polygon,
									const std::vector<double>& dists);

	// 凸多角形を軸並行の矩形 [min, max] で切り取る（Sutherland–Hodgman）。頂点列は閉じた
	// ポリゴン（末尾に始点を重複させない）で、周り方向は入力のまま保たれる。矩形の外へ
	// 完全に出ている多角形は空を返す。
	//
	// 【何に使うか】耐力壁の筋かいは「軸組内法の対角線に沿った帯」で、その帯は内法の
	// 矩形からはみ出す（帯の角が柱・横架材へ食い込む）。実物も内法へ切り詰めて納まるので、
	// 描くときも矩形で切る（core::shearWallBracePolygon。docs/DEV-NOTES.md M19）。
	//
	// 凹多角形には使わない（Sutherland–Hodgman は凹の切り口で退化した辺を残す）。用途は
	// いまのところ帯＝凸なのでこれで足りる。
	std::vector<Vec2> clipPolygonToRect(const std::vector<Vec2>& polygon, const Vec2& min,
										const Vec2& max);

	// 3 次元ベクトル（ワールド座標の点・方向）。配置・押し出しに使う。
	struct Vec3
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	// --- Vec2 演算 -----------------------------------------------------------

	inline Vec2 operator+(const Vec2& a, const Vec2& b)
	{
		return Vec2{a.x + b.x, a.y + b.y};
	}

	inline Vec2 operator-(const Vec2& a, const Vec2& b)
	{
		return Vec2{a.x - b.x, a.y - b.y};
	}

	inline Vec2 operator*(const Vec2& v, double s)
	{
		return Vec2{v.x * s, v.y * s};
	}

	// 内積。芯線に沿った射影（along = dot(相対位置, 単位方向)）に使う。
	inline double dot(const Vec2& a, const Vec2& b)
	{
		return (a.x * b.x) + (a.y * b.y);
	}

	// スカラー外積（3 次元外積の z 成分）。符号は a から b への回転向き（正＝反時計回り）。
	// 芯線からの直交距離（perp = cross(単位方向, 相対位置)）と平行判定に使う。**引数順で
	// 符号が反転する**ので、符号に意味を持たせる呼び出し側（自由端の内外判定など）は
	// 既存の式と同じ順で渡すこと。
	inline double cross(const Vec2& a, const Vec2& b)
	{
		return (a.x * b.y) - (a.y * b.x);
	}

	// ユークリッドノルム。std::hypot（中間の桁あふれ・アンダーフローに強い）で計算する。
	// 各所の手書き std::hypot(v.x, v.y) と数値的に同一——sqrt(dot(v,v)) に変えないこと。
	inline double length(const Vec2& v)
	{
		return std::hypot(v.x, v.y);
	}

	// 2 点間の距離。
	inline double distance(const Vec2& a, const Vec2& b)
	{
		return length(b - a);
	}

	// 同一直線上とみなした線分成分（start / end メンバを持つ構造体の並び）を、先頭要素の
	// 芯線方向へ射影して [最小, 最大] 区間の 1 本ぶんの両端点にする。**この射影はここに
	// 1 つだけ置く**——立上りの統合（parse/Footing）と大引の継手統合（parse/FloorPost）が
	// 同じ計算を各々書いていた。
	//
	// 前提: 先頭要素（indices.front() ＝連結成分の代表）は非縮退であること。長さ 0 の線分は
	// 同一直線判定の述語が必ず false を返すため単独成分に残り、この関数へは来ない——という
	// 呼び出し側の不変条件に依っている（core/UnionFind.h の connectedComponents 参照）。
	template <class T>
	void collinearSpan(const std::vector<T>& items, const std::vector<std::size_t>& indices,
					   Vec2& outStart, Vec2& outEnd)
	{
		const T& head = items[indices.front()];
		const Vec2 d = head.end - head.start;
		const double len = length(d);
		const Vec2 u{d.x / len, d.y / len};

		double lo = 0.0;
		double hi = 0.0;
		bool first = true;
		for (const std::size_t index : indices)
		{
			for (const Vec2& point : {items[index].start, items[index].end})
			{
				const double t = dot(u, point - head.start);
				if (first)
				{
					lo = hi = t;
					first = false;
				}
				else
				{
					lo = std::min(lo, t);
					hi = std::max(hi, t);
				}
			}
		}
		outStart = head.start + (u * lo);
		outEnd = head.start + (u * hi);
	}

	// --- Vec3 演算 -----------------------------------------------------------

	inline Vec3 operator+(const Vec3& a, const Vec3& b)
	{
		return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
	}

	inline Vec3 operator-(const Vec3& a, const Vec3& b)
	{
		return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
	}

	inline Vec3 operator*(const Vec3& v, double s)
	{
		return Vec3{v.x * s, v.y * s, v.z * s};
	}

	// 内積。
	inline double dot(const Vec3& a, const Vec3& b)
	{
		return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
	}

	// 外積（右手系）。配置行列の直交基底（Y = Z × X）を作るのに使う。
	inline Vec3 cross(const Vec3& a, const Vec3& b)
	{
		return Vec3{(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z),
					(a.x * b.y) - (a.y * b.x)};
	}

	// ユークリッドノルム。
	inline double length(const Vec3& v)
	{
		return std::sqrt(dot(v, v));
	}

	// 正規化。長さがほぼ 0 のベクトルはゼロベクトルを返す（ゼロ割り回避）。
	// IFC の IfcDirection は単位ベクトルでない場合があり、Axis2Placement の
	// 基底計算で必ず正規化してから使う。
	inline Vec3 normalized(const Vec3& v)
	{
		const double len = length(v);
		if (len < kGeomEps)
			return Vec3{0.0, 0.0, 0.0};
		return Vec3{v.x / len, v.y / len, v.z / len};
	}

	// --- Mat4（4x4 同次変換行列）--------------------------------------------
	//
	//	行優先（m[row][col]）で保持する。点 p の変換は p' = M·[x,y,z,1]^T。
	//	上位 3x3 が回転（基底ベクトル）、第 4 列が平行移動、最下行は (0,0,0,1)。
	//	fromAxes で列に基底ベクトル X/Y/Z を並べるので、ローカル座標 (1,0,0) は
	//	X 軸へ、(0,1,0) は Y 軸へ写る（IfcAxis2Placement の定義と一致）。
	struct Mat4
	{
		std::array<std::array<double, 4>, 4> m{};

		// 単位行列。
		static Mat4 identity();

		// 平行移動のみの行列。
		static Mat4 translation(const Vec3& t);

		// 基底ベクトル（列 0/1/2 = X/Y/Z 軸）と原点（列 3）から剛体変換を作る。
		// x/y/z は正規直交である前提（呼び出し側で Gram-Schmidt 済み）。
		static Mat4 fromAxes(const Vec3& xAxis, const Vec3& yAxis, const Vec3& zAxis,
							 const Vec3& origin);

		// 点を変換する（回転＋平行移動、w=1）。
		Vec3 transformPoint(const Vec3& p) const;

		// 方向ベクトルを変換する（回転のみ、平行移動を無視）。押し出しベクトル等に使う。
		Vec3 transformDirection(const Vec3& d) const;
	};

	// 行列積（this が左）。ワールド = 親配置 × 相対配置 の合成に使う。
	Mat4 operator*(const Mat4& a, const Mat4& b);
} // namespace HomeskzIfcImport::core
