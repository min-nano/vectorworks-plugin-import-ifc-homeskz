//
//	tests/RoofSample.h
//
//	屋根まわりのテストが共有する試験用データ。**屋根面（RoofPlane）と最小の屋根版 IFC は
//	ここが唯一の定義**で、垂木（ParseRafterTests）・野地板（ParseRoofTests）・勾配座標系
//	（GeometryTests）はいずれもこれを使う。
//
//	【なぜ 1 か所に置くか】垂木と野地板は同じ屋根面から導かれるので、両者のテストは同じ
//	平面・同じ最小モデルに対する期待値でなければ意味がない。かつては shedPlane が 3 ファイル、
//	minimalRoofText が 2 ファイルに逐語的な複製として置かれており、片方の平面や STEP 断片
//	だけを直すと「同じ屋根版なのに垂木と野地板で前提が違う」テストになり得た（Fixtures.h に
//	フィクスチャ一覧を一本化したのと同じ理由）。
//
//	【無 SDK】ここも他のテストと同じく VectorWorks SDK に触れない（CLAUDE.md「テスト方針」）。
//

#pragma once

#include "core/Geometry.h"
#include "parse/IfcGeometry.h"

#include <cmath>
#include <string>

namespace HomeskzIfcTests
{
	// 試験用の片流れ屋根面: XY 平面上の 4m×3m 矩形が +Y 方向へ立ち上がる。軒（y=0）で z=1000、
	// 棟（y=3000）で z=2000 なので z(x, y) = 1000 + y/3 ⇒ 法線 ∝ (0, −1/3, 1)、上向き単位法線
	// は (0, −1, 3)/√10。
	inline HomeskzIfcImport::parse::RoofPlane shedPlane()
	{
		using HomeskzIfcImport::core::Vec3;
		const double s = std::sqrt(10.0);
		HomeskzIfcImport::parse::RoofPlane plane;
		plane.vertices = {Vec3{0.0, 0.0, 1000.0}, Vec3{4000.0, 0.0, 1000.0},
						  Vec3{4000.0, 3000.0, 2000.0}, Vec3{0.0, 3000.0, 2000.0}};
		plane.normal = Vec3{0.0, -1.0 / s, 3.0 / s};
		return plane;
	}

	// 上の屋根面に対応する最小 STEP モデル。1FL（Elevation 0）と 2FL（Elevation 3000）を
	// 持ち、2FL（最上階＝屋根）に勾配した屋根版 1 枚を含む。屋根版は**要素配置を傾けて**
	// プロファイル平面ごと傾けることで勾配面を作る（ホームズ君の屋根版と同じ表現: 勾配した
	// 平面外形を面法線方向へ押し出す）。
	//   * Axis=(0,−1,3) → 正規化した局所 Z が面法線（上向き）
	//   * RefDirection=(1,0,0) → 局所 X は X 軸のまま（Gram-Schmidt で直交化）
	// slabName を "屋根版" 以外にすると屋根版として拾われないことの確認にも使う。
	inline std::string minimalRoofText(const std::string& slabName)
	{
		return "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
			   "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
			   "#3=IFCLOCALPLACEMENT($,#2);\n"
			   "#10=IFCBUILDINGSTOREY('s1',$,'1FL',$,$,#3,$,$,.ELEMENT.,0.);\n"
			   "#11=IFCBUILDINGSTOREY('s2',$,'2FL',$,$,#3,$,$,.ELEMENT.,3000.);\n"
			   // 屋根版の配置: 原点（ローカル Z=0）で局所 Z を (0,−1,3) へ傾ける。
			   "#20=IFCCARTESIANPOINT((0.,0.,0.));\n"
			   "#21=IFCDIRECTION((0.,-1.,3.));\n"
			   "#22=IFCDIRECTION((1.,0.,0.));\n"
			   "#23=IFCAXIS2PLACEMENT3D(#20,#21,#22);\n"
			   "#24=IFCLOCALPLACEMENT(#3,#23);\n"
			   // 断面（プロファイル座標系の 4000×3000 矩形）と面法線方向への押し出し（厚み 12）
			   "#30=IFCCARTESIANPOINT((0.,0.));\n"
			   "#31=IFCAXIS2PLACEMENT2D(#30,$);\n"
			   "#32=IFCRECTANGLEPROFILEDEF(.AREA.,$,#31,4000.,3000.);\n"
			   "#33=IFCCARTESIANPOINT((0.,0.,0.));\n"
			   "#34=IFCAXIS2PLACEMENT3D(#33,$,$);\n"
			   "#35=IFCDIRECTION((0.,0.,1.));\n"
			   "#36=IFCEXTRUDEDAREASOLID(#32,#34,#35,12.);\n"
			   "#37=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#36));\n"
			   "#38=IFCPRODUCTDEFINITIONSHAPE($,$,(#37));\n"
			   "#40=IFCSLAB('slab',$,'" +
			   slabName +
			   "',$,$,#24,#38,$,$);\n"
			   "#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#40),#11);\n";
	}
} // namespace HomeskzIfcTests
