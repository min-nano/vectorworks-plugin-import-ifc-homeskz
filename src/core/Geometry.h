//
//	core/Geometry.h
//
//	フェーズ非依存の自前幾何型。parse/ は VectorWorks SDK を include しないため、
//	SDK の幾何型（WorldPt3 / TransformMatrix 等）を使えない。Python 版が
//	ifcopenshell 抜きで手計算している配置行列・断面・押し出しの数式を、この
//	ヘッダの Vec2 / Vec3 / Mat4 の上に移植する（詳細は docs/DEV-NOTES.md M2）。
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

#include <array>
#include <cmath>

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
