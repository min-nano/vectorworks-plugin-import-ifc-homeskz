//
//	core/Geometry.h
//
//	フェーズ非依存の自前幾何型。parse/ は VectorWorks SDK を include しないため、
//	SDK の幾何型（WorldPt3 / TransformMatrix 等）を使えない。Python 版が
//	ifcopenshell 抜きで手計算している配置行列・断面・押し出しの数式を、この
//	ヘッダの Vec2 / Vec3 / Mat4 の上に移植していく（詳細は ROADMAP.md M2）。
//
//	いまはフォルダ骨組みの一部として最小限の Vec2 / Vec3 だけを置く。実際の
//	行列合成（Mat4）・押し出し・断面解決は M2「幾何の土台」で肉付けする。
//

#pragma once

namespace HomeskzIfcImport::core
{
	// 2 次元ベクトル（平面上の点・方向）。通り芯やプロファイル外形の座標に使う。
	struct Vec2
	{
		double x = 0.0;
		double y = 0.0;
	};

	// 3 次元ベクトル（ワールド座標の点・方向）。配置・押し出しに使う。
	struct Vec3
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	// TODO(M2): Mat4（配置行列合成）と Vec2/Vec3 の演算（加減・内積・外積・
	// 正規化・行列適用）を、Python 版の _get_placement_3d 相当の数式に合わせて追加する。
} // namespace HomeskzIfcImport::core
