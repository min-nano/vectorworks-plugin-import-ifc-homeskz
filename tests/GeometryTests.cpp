//
//	GeometryTests.cpp
//
//	幾何の土台（src/core/Geometry ＋ src/parse/IfcGeometry）の単体テスト。
//	VectorWorks SDK を一切 include せず、無 SDK のテストハーネス（TestFramework.h）で
//	走る（CLAUDE.md「テスト方針」: core/ parse/ は無 SDK で単体テスト）。ROADMAP.md M2
//	「幾何の土台」の検証にあたる。M3 以降のほぼ全要素がここを共有するため、後工程へ
//	ズレを持ち越さないよう、数式を手計算値と許容誤差で突き合わせる。
//
//	検証項目:
//	  * Vec2/Vec3 演算（内積・外積・正規化・ゼロ割り回避）と Mat4（生成・積・点/方向適用）。
//	  * IfcDirection / IfcCartesianPoint の解決（2D/3D、欠損スキップ）。
//	  * IfcAxis2Placement3D（既定・回転・Gram-Schmidt・縮退フォールバック）。
//	  * IfcObjectPlacement（要素自身の RelativePlacement のみ＝★親 PlacementRelTo 非合成。
//	    Python 版と一致。階高の二重計上を防ぐ最重要のパリティ）。
//	  * IfcRectangleProfileDef（Position は平行移動のみ・RefDirection 回転は無視）/
//	    IfcArbitraryClosedProfileDef の外形。
//	  * IfcExtrudedAreaSolid（鉛直押し出し・水平押し出し・要素配置との合成、WorldSolid の基底）。
//	  * IfcBooleanResult の第 1 オペランド辿り（入れ子・非 boolean）。
//	  * 実フィクスチャで例外なく解決できること。
//

#include "TestFramework.h"

#include "core/Geometry.h"
#include "parse/IfcGeometry.h"
#include "parse/Loader.h"

#include <cmath>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using core::Mat4;
using core::Vec2;
using core::Vec3;
using parse::loadIfcFromText;
using parse::Model;
using parse::Profile;
using parse::WorldSolid;

namespace
{
	// 2 つの実数が許容誤差内で等しいか。座標・寸法比較に使う（mm 単位系）。
	bool near(double a, double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	// 2 つの Vec3 が許容誤差内で等しいか。
	bool nearVec(const Vec3& a, const Vec3& b)
	{
		return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
	}

	// outer の中に (x,y) にほぼ一致する頂点があるか（頂点の並び順に依存せず存在を確認）。
	bool hasVertex(const std::vector<Vec2>& outer, double x, double y)
	{
		for (const Vec2& v : outer)
			if (near(v.x, x) && near(v.y, y))
				return true;
		return false;
	}
} // namespace

// ---------------------------------------------------------------------------
// Vec2 / Vec3 演算
// ---------------------------------------------------------------------------

TEST(vec3_arithmetic_and_products)
{
	const Vec3 a{1.0, 2.0, 3.0};
	const Vec3 b{4.0, 5.0, 6.0};

	CHECK(nearVec(a + b, Vec3{5.0, 7.0, 9.0}));
	CHECK(nearVec(b - a, Vec3{3.0, 3.0, 3.0}));
	CHECK(nearVec(a * 2.0, Vec3{2.0, 4.0, 6.0}));

	// 内積 1*4 + 2*5 + 3*6 = 32。
	CHECK(near(core::dot(a, b), 32.0));

	// 外積 x×y = z（右手系）。
	const Vec3 x{1.0, 0.0, 0.0};
	const Vec3 y{0.0, 1.0, 0.0};
	CHECK(nearVec(core::cross(x, y), Vec3{0.0, 0.0, 1.0}));
	// 反交換律: y×x = −z。
	CHECK(nearVec(core::cross(y, x), Vec3{0.0, 0.0, -1.0}));
}

TEST(vec3_length_and_normalize)
{
	CHECK(near(core::length(Vec3{3.0, 4.0, 0.0}), 5.0));

	const Vec3 n = core::normalized(Vec3{0.0, 0.0, 2.0});
	CHECK(nearVec(n, Vec3{0.0, 0.0, 1.0}));

	// ゼロベクトルの正規化はゼロ（ゼロ割り回避）。
	CHECK(nearVec(core::normalized(Vec3{0.0, 0.0, 0.0}), Vec3{0.0, 0.0, 0.0}));
}

// ---------------------------------------------------------------------------
// Mat4
// ---------------------------------------------------------------------------

TEST(mat4_identity_and_translation)
{
	const Mat4 id = Mat4::identity();
	const Vec3 p{7.0, -3.0, 2.0};
	CHECK(nearVec(id.transformPoint(p), p));
	CHECK(nearVec(id.transformDirection(p), p));

	const Mat4 t = Mat4::translation(Vec3{10.0, 20.0, 30.0});
	// 点は平行移動を受ける。
	CHECK(nearVec(t.transformPoint(Vec3{1.0, 2.0, 3.0}), Vec3{11.0, 22.0, 33.0}));
	// 方向は平行移動を受けない。
	CHECK(nearVec(t.transformDirection(Vec3{1.0, 2.0, 3.0}), Vec3{1.0, 2.0, 3.0}));
}

TEST(mat4_from_axes_rotation)
{
	// 局所 X→世界 +Y、局所 Y→世界 −X（Z 周り +90°回転）＋原点 (5,0,0)。
	const Mat4 r = Mat4::fromAxes(Vec3{0.0, 1.0, 0.0}, Vec3{-1.0, 0.0, 0.0}, Vec3{0.0, 0.0, 1.0},
								  Vec3{5.0, 0.0, 0.0});
	// 局所 (1,0,0) は X 軸(=世界 +Y)＋原点 = (5,1,0)。
	CHECK(nearVec(r.transformPoint(Vec3{1.0, 0.0, 0.0}), Vec3{5.0, 1.0, 0.0}));
	// 局所 (0,1,0) は Y 軸(=世界 −X)＋原点 = (4,0,0)。
	CHECK(nearVec(r.transformPoint(Vec3{0.0, 1.0, 0.0}), Vec3{4.0, 0.0, 0.0}));
	// 方向としては原点を無視して回転のみ。
	CHECK(nearVec(r.transformDirection(Vec3{1.0, 0.0, 0.0}), Vec3{0.0, 1.0, 0.0}));
}

TEST(mat4_multiply_composes_transforms)
{
	// 平行移動 T の後に回転 R をかけると、合成 (R*T) は「先に T、次に R」を意味する。
	const Mat4 rot = Mat4::fromAxes(Vec3{0.0, 1.0, 0.0}, Vec3{-1.0, 0.0, 0.0}, Vec3{0.0, 0.0, 1.0},
									Vec3{0.0, 0.0, 0.0});
	const Mat4 tr = Mat4::translation(Vec3{1.0, 0.0, 0.0});
	const Mat4 composed = rot * tr;
	// 点 (0,0,0): T で (1,0,0)、R で世界 +Y へ → (0,1,0)。
	CHECK(nearVec(composed.transformPoint(Vec3{0.0, 0.0, 0.0}), Vec3{0.0, 1.0, 0.0}));

	// 単位行列との積は元のまま。
	const Mat4 sameL = Mat4::identity() * rot;
	const Mat4 sameR = rot * Mat4::identity();
	CHECK(nearVec(sameL.transformPoint(Vec3{1.0, 0.0, 0.0}), Vec3{0.0, 1.0, 0.0}));
	CHECK(nearVec(sameR.transformPoint(Vec3{1.0, 0.0, 0.0}), Vec3{0.0, 1.0, 0.0}));
}

// ---------------------------------------------------------------------------
// IfcDirection / IfcCartesianPoint の解決
// ---------------------------------------------------------------------------

TEST(resolves_direction_and_point_3d_and_2d)
{
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((1.,2.,3.));\n"
										"#11=IFCCARTESIANPOINT((4.,5.));\n" // 2D 点 → Z=0
										"#20=IFCDIRECTION((0.,0.,1.));\n"
										"#21=IFCDIRECTION((1.,0.));\n"); // 2D 方向 → Z=0

	using parse::Value;
	const auto refTo = [](int id)
	{
		Value v;
		v.type = parse::ValueType::Reference;
		v.reference = id;
		return v;
	};

	Vec3 p{0.0, 0.0, 0.0};
	CHECK(parse::resolvePoint(model, refTo(10), p));
	CHECK(nearVec(p, Vec3{1.0, 2.0, 3.0}));
	CHECK(parse::resolvePoint(model, refTo(11), p));
	CHECK(nearVec(p, Vec3{4.0, 5.0, 0.0}));

	Vec3 d{0.0, 0.0, 0.0};
	CHECK(parse::resolveDirection(model, refTo(20), d));
	CHECK(nearVec(d, Vec3{0.0, 0.0, 1.0}));
	CHECK(parse::resolveDirection(model, refTo(21), d));
	CHECK(nearVec(d, Vec3{1.0, 0.0, 0.0}));

	// 未解決参照は false（out は変更しない）。
	Vec3 keep{9.0, 9.0, 9.0};
	CHECK(!parse::resolvePoint(model, refTo(999), keep));
	CHECK(nearVec(keep, Vec3{9.0, 9.0, 9.0}));
}

// ---------------------------------------------------------------------------
// IfcAxis2Placement3D
// ---------------------------------------------------------------------------

TEST(axis2placement3d_default_is_translation_only)
{
	// Axis / RefDirection 省略（$）→ 単位回転＋原点のみ。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((10.,20.,30.));\n"
										"#30=IFCAXIS2PLACEMENT3D(#10,$,$);\n");
	const Mat4 m = parse::resolveAxis2Placement3D(model, model.entity(30));
	CHECK(nearVec(m.transformPoint(Vec3{0.0, 0.0, 0.0}), Vec3{10.0, 20.0, 30.0}));
	CHECK(nearVec(m.transformDirection(Vec3{1.0, 0.0, 0.0}), Vec3{1.0, 0.0, 0.0}));
	CHECK(nearVec(m.transformDirection(Vec3{0.0, 0.0, 1.0}), Vec3{0.0, 0.0, 1.0}));
}

TEST(axis2placement3d_rotated_axes)
{
	// Axis=(0,0,1)、RefDirection=(0,1,0) → 局所 X=世界 +Y、Y=Z×X=(-1,0,0)。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCDIRECTION((0.,0.,1.));\n"
										"#12=IFCDIRECTION((0.,1.,0.));\n"
										"#30=IFCAXIS2PLACEMENT3D(#10,#11,#12);\n");
	const Mat4 m = parse::resolveAxis2Placement3D(model, model.entity(30));
	CHECK(nearVec(m.transformDirection(Vec3{1.0, 0.0, 0.0}), Vec3{0.0, 1.0, 0.0}));
	CHECK(nearVec(m.transformDirection(Vec3{0.0, 1.0, 0.0}), Vec3{-1.0, 0.0, 0.0}));
	CHECK(nearVec(m.transformDirection(Vec3{0.0, 0.0, 1.0}), Vec3{0.0, 0.0, 1.0}));
}

TEST(axis2placement3d_gram_schmidt_orthonormalizes)
{
	// RefDirection が Axis に直交していない場合、成分を落として正規直交化する。
	// Axis=(0,0,1)、RefDirection=(1,0,1) → X=正規化((1,0,1)−(0,0,1))=(1,0,0)。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCDIRECTION((0.,0.,1.));\n"
										"#12=IFCDIRECTION((1.,0.,1.));\n"
										"#30=IFCAXIS2PLACEMENT3D(#10,#11,#12);\n");
	const Mat4 m = parse::resolveAxis2Placement3D(model, model.entity(30));
	CHECK(nearVec(m.transformDirection(Vec3{1.0, 0.0, 0.0}), Vec3{1.0, 0.0, 0.0}));
	CHECK(nearVec(m.transformDirection(Vec3{0.0, 0.0, 1.0}), Vec3{0.0, 0.0, 1.0}));

	// RefDirection が非単位でも X は単位ベクトルになる（(2,2,0)→(1/√2,1/√2,0)）。
	Model const model2 = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										 "#11=IFCDIRECTION((0.,0.,1.));\n"
										 "#12=IFCDIRECTION((2.,2.,0.));\n"
										 "#30=IFCAXIS2PLACEMENT3D(#10,#11,#12);\n");
	const Mat4 m2 = parse::resolveAxis2Placement3D(model2, model2.entity(30));
	const double inv = 1.0 / std::sqrt(2.0);
	CHECK(nearVec(m2.transformDirection(Vec3{1.0, 0.0, 0.0}), Vec3{inv, inv, 0.0}));
}

TEST(axis2placement3d_degenerate_ref_falls_back)
{
	// RefDirection が Axis と平行（縮退）でも、直交する X 軸へフォールバックし、
	// 正規直交基底を維持する（各軸が単位長・相互直交）。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#11=IFCDIRECTION((0.,0.,1.));\n"
										"#12=IFCDIRECTION((0.,0.,1.));\n" // Axis と平行
										"#30=IFCAXIS2PLACEMENT3D(#10,#11,#12);\n");
	const Mat4 m = parse::resolveAxis2Placement3D(model, model.entity(30));
	const Vec3 x = m.transformDirection(Vec3{1.0, 0.0, 0.0});
	const Vec3 y = m.transformDirection(Vec3{0.0, 1.0, 0.0});
	const Vec3 z = m.transformDirection(Vec3{0.0, 0.0, 1.0});
	CHECK(near(core::length(x), 1.0));
	CHECK(near(core::length(y), 1.0));
	CHECK(nearVec(z, Vec3{0.0, 0.0, 1.0}));
	CHECK(near(core::dot(x, z), 0.0)); // X ⟂ Z
	CHECK(near(core::dot(x, y), 0.0)); // X ⟂ Y
}

TEST(axis2placement3d_null_entity_is_identity)
{
	Model const model = loadIfcFromText("#1=IFCDIRECTION((0.,0.,1.));\n");
	const Mat4 m = parse::resolveAxis2Placement3D(model, nullptr);
	CHECK(nearVec(m.transformPoint(Vec3{3.0, 4.0, 5.0}), Vec3{3.0, 4.0, 5.0}));
}

// ---------------------------------------------------------------------------
// IfcObjectPlacement（要素配置。★親 PlacementRelTo を合成しない ＝ Python 版一致）
// ---------------------------------------------------------------------------

TEST(object_placement_uses_element_own_relative_placement)
{
	// IfcColumn の ObjectPlacement（属性 5）の RelativePlacement だけを使う。
	// 原点 (300,400,-174)、Axis 省略 → 単位回転＋その原点。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((300.,400.,-174.));\n"
										"#11=IFCAXIS2PLACEMENT3D(#10,$,$);\n"
										"#12=IFCLOCALPLACEMENT($,#11);\n"
										"#20=IFCCOLUMN('gid',$,'柱',$,$,#12,$,$,$);\n");
	const Mat4 m = parse::resolveObjectPlacement(model, model.entity(20));
	CHECK(nearVec(m.transformPoint(Vec3{0.0, 0.0, 0.0}), Vec3{300.0, 400.0, -174.0}));
	CHECK(nearVec(m.transformDirection(Vec3{1.0, 0.0, 0.0}), Vec3{1.0, 0.0, 0.0}));
}

TEST(object_placement_ignores_parent_placement)
{
	// ★最重要（Python 版パリティ）: 親（階）配置に Z=+600 があっても、要素配置は
	// 要素自身の Location（Z=−174）だけを返し、親の +600 を合成しない（階高の二重計上防止）。
	// 実測（サンプル邸の柱）を再現: 要素 Z=−174 / 親階 Z=+600。合成すると +426 になるが、
	// Python 版はそうせず −174 を使う。
	Model const model =
		loadIfcFromText("#4=IFCCARTESIANPOINT((0.,0.,0.));\n"
						"#5=IFCAXIS2PLACEMENT3D(#4,$,$);\n"
						"#6=IFCLOCALPLACEMENT($,#5);\n" // 建物（Z=0）
						"#37=IFCCARTESIANPOINT((0.,0.,600.));\n"
						"#38=IFCAXIS2PLACEMENT3D(#37,$,$);\n"
						"#39=IFCLOCALPLACEMENT(#6,#38);\n" // 階（Z=+600）
						"#367=IFCCARTESIANPOINT((37765.,-25480.,-174.));\n"
						"#368=IFCAXIS2PLACEMENT3D(#367,$,$);\n"
						"#369=IFCLOCALPLACEMENT(#39,#368);\n" // 要素（Z=−174、親=階）
						"#370=IFCCOLUMN('gid',$,'柱',$,$,#369,$,$,$);\n");
	const Mat4 m = parse::resolveObjectPlacement(model, model.entity(370));
	const Vec3 o = m.transformPoint(Vec3{0.0, 0.0, 0.0});
	// 親の +600 を足さない → Z は要素自身の −174（+426 ではない）。
	CHECK(near(o.x, 37765.0));
	CHECK(near(o.y, -25480.0));
	CHECK(near(o.z, -174.0));
}

TEST(object_placement_missing_is_identity)
{
	// ObjectPlacement が $（未設定）/ 要素が nullptr → 単位行列。
	Model const model = loadIfcFromText("#20=IFCCOLUMN('gid',$,'柱',$,$,$,$,$,$);\n");
	const Mat4 m = parse::resolveObjectPlacement(model, model.entity(20));
	CHECK(nearVec(m.transformPoint(Vec3{5.0, 6.0, 7.0}), Vec3{5.0, 6.0, 7.0}));
	const Mat4 mn = parse::resolveObjectPlacement(model, nullptr);
	CHECK(nearVec(mn.transformPoint(Vec3{5.0, 6.0, 7.0}), Vec3{5.0, 6.0, 7.0}));
}

// ---------------------------------------------------------------------------
// 断面プロファイル
// ---------------------------------------------------------------------------

TEST(rectangle_profile_centered_corners)
{
	// XDim=105, YDim=210, Position 原点・既定 → 中心原点で 4 隅 (±52.5, ±105)。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.));\n"
										"#11=IFCAXIS2PLACEMENT2D(#10,$);\n"
										"#12=IFCRECTANGLEPROFILEDEF(.AREA.,$,#11,105.,210.);\n");
	Profile prof;
	CHECK(parse::resolveProfile(model, model.entity(12), prof));
	CHECK(prof.rectangle);
	CHECK(near(prof.xDim, 105.0));
	CHECK(near(prof.yDim, 210.0));
	CHECK_EQ(prof.outer.size(), static_cast<std::size_t>(4));
	CHECK(hasVertex(prof.outer, 52.5, 105.0));
	CHECK(hasVertex(prof.outer, -52.5, 105.0));
	CHECK(hasVertex(prof.outer, 52.5, -105.0));
	CHECK(hasVertex(prof.outer, -52.5, -105.0));
}

TEST(rectangle_profile_honors_position)
{
	// Position が (1000,2000) 平行移動 → 4 隅が中心 (1000,2000) 周りに来る。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((1000.,2000.));\n"
										"#11=IFCAXIS2PLACEMENT2D(#10,$);\n"
										"#12=IFCRECTANGLEPROFILEDEF(.AREA.,$,#11,100.,60.);\n");
	Profile prof;
	CHECK(parse::resolveProfile(model, model.entity(12), prof));
	CHECK(hasVertex(prof.outer, 1050.0, 2030.0));
	CHECK(hasVertex(prof.outer, 950.0, 1970.0));
}

TEST(rectangle_profile_ignores_position_rotation)
{
	// ★Python 版パリティ: 矩形 Position は Location の平行移動だけを反映し、RefDirection の
	// 回転は無視する（_profile_points に一致）。RefDirection=(0,1) を与えても 4 隅は回転せず、
	// 軸並行の中心 (0,0) 矩形のまま（回転していれば (±30,±50) 等の並びが変わるはず）。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.));\n"
										"#11=IFCDIRECTION((0.,1.));\n"
										"#12=IFCAXIS2PLACEMENT2D(#10,#11);\n"
										"#13=IFCRECTANGLEPROFILEDEF(.AREA.,$,#12,100.,60.);\n");
	Profile prof;
	CHECK(parse::resolveProfile(model, model.entity(13), prof));
	// 回転を無視した軸並行の 4 隅 (±50, ±30)。
	CHECK(hasVertex(prof.outer, 50.0, 30.0));
	CHECK(hasVertex(prof.outer, -50.0, 30.0));
	CHECK(hasVertex(prof.outer, 50.0, -30.0));
	CHECK(hasVertex(prof.outer, -50.0, -30.0));
}

TEST(rectangle_profile_rejects_nonpositive_dims)
{
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.));\n"
										"#11=IFCAXIS2PLACEMENT2D(#10,$);\n"
										"#12=IFCRECTANGLEPROFILEDEF(.AREA.,$,#11,0.,60.);\n");
	Profile prof;
	CHECK(!parse::resolveProfile(model, model.entity(12), prof));
}

TEST(arbitrary_profile_reads_outline)
{
	// 三角形の外形（IfcPolyline）。始点＝終点の重複を落として 3 頂点にする。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.));\n"
										"#11=IFCCARTESIANPOINT((100.,0.));\n"
										"#12=IFCCARTESIANPOINT((0.,50.));\n"
										"#13=IFCPOLYLINE((#10,#11,#12,#10));\n"
										"#14=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#13);\n");
	Profile prof;
	CHECK(parse::resolveProfile(model, model.entity(14), prof));
	CHECK(!prof.rectangle);
	CHECK_EQ(prof.outer.size(), static_cast<std::size_t>(3));
	CHECK(hasVertex(prof.outer, 0.0, 0.0));
	CHECK(hasVertex(prof.outer, 100.0, 0.0));
	CHECK(hasVertex(prof.outer, 0.0, 50.0));
}

TEST(arbitrary_profile_rejects_non_polyline_or_short)
{
	// OuterCurve が非ポリライン → false。点が 2 つ（面にならない）→ false。
	Model const model = loadIfcFromText("#10=IFCCARTESIANPOINT((0.,0.));\n"
										"#11=IFCCIRCLE(#10,50.);\n"
										"#12=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#11);\n"
										"#20=IFCCARTESIANPOINT((0.,0.));\n"
										"#21=IFCCARTESIANPOINT((100.,0.));\n"
										"#22=IFCPOLYLINE((#20,#21));\n"
										"#23=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#22);\n");
	Profile prof;
	CHECK(!parse::resolveProfile(model, model.entity(12), prof));
	CHECK(!parse::resolveProfile(model, model.entity(23), prof));
}

// ---------------------------------------------------------------------------
// IfcExtrudedAreaSolid
// ---------------------------------------------------------------------------

TEST(extrude_vertical_with_world_placement)
{
	// 105x105 の柱断面を Z へ 2844 押し出し、要素配置で (300,400,0) へ据える。
	// 底面は z=0（配置 Z）、天面は z=2844、押し出しベクトルは (0,0,2844)。
	Model const model = loadIfcFromText("#40=IFCCARTESIANPOINT((0.,0.));\n"
										"#41=IFCAXIS2PLACEMENT2D(#40,$);\n"
										"#42=IFCRECTANGLEPROFILEDEF(.AREA.,$,#41,105.,105.);\n"
										"#43=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#44=IFCAXIS2PLACEMENT3D(#43,$,$);\n"
										"#45=IFCDIRECTION((0.,0.,1.));\n"
										"#46=IFCEXTRUDEDAREASOLID(#42,#44,#45,2844.);\n");
	// 要素ワールド配置（平行移動のみ）。
	const Mat4 placement = Mat4::translation(Vec3{300.0, 400.0, 0.0});
	WorldSolid solid;
	CHECK(parse::resolveExtrudedAreaSolid(model, model.entity(46), placement, solid));

	// WorldSolid は 2D プロファイル＋基底を保持（Python _Solid 相当）。
	CHECK_EQ(solid.profile.size(), static_cast<std::size_t>(4));
	CHECK(solid.rectangle);
	CHECK(near(solid.depth, 2844.0));
	CHECK(nearVec(solid.extrudeDir, Vec3{0.0, 0.0, 1.0}));
	CHECK(nearVec(solid.extrusion(), Vec3{0.0, 0.0, 2844.0}));

	const std::vector<Vec3> base = solid.base();
	CHECK_EQ(base.size(), static_cast<std::size_t>(4));
	// 底面の 1 隅: 中心 (300,400) ± 52.5、z=0。
	bool sawCorner = false;
	for (const Vec3& p : base)
	{
		CHECK(near(p.z, 0.0));
		if (near(p.x, 352.5) && near(p.y, 452.5))
			sawCorner = true;
	}
	CHECK(sawCorner);
	// 天面は底面＋押し出し（z=2844）。
	const std::vector<Vec3> top = solid.top();
	CHECK_EQ(top.size(), static_cast<std::size_t>(4));
	for (const Vec3& p : top)
		CHECK(near(p.z, 2844.0));
}

TEST(extrude_horizontal_beam_direction)
{
	// 梁: Position の局所 Z を世界 +X へ向け（Axis=(1,0,0)、RefDir=(0,0,1)）、
	// 局所 Z へ 3000 押し出す → 世界の押し出しは (3000,0,0)（水平押し出し）。
	Model const model = loadIfcFromText("#40=IFCCARTESIANPOINT((0.,0.));\n"
										"#41=IFCAXIS2PLACEMENT2D(#40,$);\n"
										"#42=IFCRECTANGLEPROFILEDEF(.AREA.,$,#41,120.,240.);\n"
										"#43=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#44=IFCDIRECTION((1.,0.,0.));\n"
										"#45=IFCDIRECTION((0.,0.,1.));\n"
										"#46=IFCAXIS2PLACEMENT3D(#43,#44,#45);\n"
										"#47=IFCDIRECTION((0.,0.,1.));\n"
										"#48=IFCEXTRUDEDAREASOLID(#42,#46,#47,3000.);\n");
	WorldSolid solid;
	CHECK(parse::resolveExtrudedAreaSolid(model, model.entity(48), Mat4::identity(), solid));
	CHECK(nearVec(solid.extrusion(), Vec3{3000.0, 0.0, 0.0}));
	// 断面は世界の YZ 平面上（局所 X→世界 +Z、局所 Y→世界 −Y）。全底面点の世界 X は 0。
	for (const Vec3& p : solid.base())
		CHECK(near(p.x, 0.0));
}

TEST(extrude_depth_scales_unit_direction)
{
	// ExtrudedDirection が非単位 (0,0,2) でも、単位化して Depth を掛けるので
	// 押し出しは (0,0,Depth) になる（長さの二重計上を防ぐ）。
	Model const model = loadIfcFromText("#40=IFCCARTESIANPOINT((0.,0.));\n"
										"#41=IFCAXIS2PLACEMENT2D(#40,$);\n"
										"#42=IFCRECTANGLEPROFILEDEF(.AREA.,$,#41,100.,100.);\n"
										"#43=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#44=IFCAXIS2PLACEMENT3D(#43,$,$);\n"
										"#45=IFCDIRECTION((0.,0.,2.));\n"
										"#46=IFCEXTRUDEDAREASOLID(#42,#44,#45,500.);\n");
	WorldSolid solid;
	CHECK(parse::resolveExtrudedAreaSolid(model, model.entity(46), Mat4::identity(), solid));
	CHECK(nearVec(solid.extrusion(), Vec3{0.0, 0.0, 500.0}));
}

TEST(extrude_rejects_non_solid)
{
	Model const model = loadIfcFromText("#1=IFCDIRECTION((0.,0.,1.));\n");
	WorldSolid solid;
	CHECK(!parse::resolveExtrudedAreaSolid(model, model.entity(1), Mat4::identity(), solid));
	CHECK(!parse::resolveExtrudedAreaSolid(model, nullptr, Mat4::identity(), solid));
}

// ---------------------------------------------------------------------------
// IfcBooleanResult の第 1 オペランド辿り
// ---------------------------------------------------------------------------

TEST(boolean_result_walks_to_base_solid)
{
	// DIFFERENCE の第 1 オペランドが素の押し出しソリッド → それを返す。
	Model const model = loadIfcFromText("#40=IFCCARTESIANPOINT((0.,0.));\n"
										"#41=IFCAXIS2PLACEMENT2D(#40,$);\n"
										"#42=IFCRECTANGLEPROFILEDEF(.AREA.,$,#41,100.,100.);\n"
										"#43=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#44=IFCAXIS2PLACEMENT3D(#43,$,$);\n"
										"#45=IFCDIRECTION((0.,0.,1.));\n"
										"#46=IFCEXTRUDEDAREASOLID(#42,#44,#45,500.);\n"
										"#47=IFCEXTRUDEDAREASOLID(#42,#44,#45,200.);\n"
										"#48=IFCBOOLEANRESULT(.DIFFERENCE.,#46,#47);\n");
	const parse::Entity* base = parse::resolveBaseSolid(model, model.entity(48));
	CHECK(base != nullptr);
	if (base != nullptr)
		CHECK_EQ(base->id, 46);
}

TEST(boolean_result_walks_nested_first_operands)
{
	// 入れ子の boolean → 第 1 オペランドを再帰的に辿って最奥の素ソリッドへ。
	Model const model = loadIfcFromText("#40=IFCCARTESIANPOINT((0.,0.));\n"
										"#41=IFCAXIS2PLACEMENT2D(#40,$);\n"
										"#42=IFCRECTANGLEPROFILEDEF(.AREA.,$,#41,100.,100.);\n"
										"#43=IFCCARTESIANPOINT((0.,0.,0.));\n"
										"#44=IFCAXIS2PLACEMENT3D(#43,$,$);\n"
										"#45=IFCDIRECTION((0.,0.,1.));\n"
										"#46=IFCEXTRUDEDAREASOLID(#42,#44,#45,500.);\n"
										"#50=IFCEXTRUDEDAREASOLID(#42,#44,#45,200.);\n"
										"#51=IFCBOOLEANCLIPPINGRESULT(.DIFFERENCE.,#46,#50);\n"
										"#52=IFCBOOLEANRESULT(.DIFFERENCE.,#51,#50);\n");
	const parse::Entity* base = parse::resolveBaseSolid(model, model.entity(52));
	CHECK(base != nullptr);
	if (base != nullptr)
		CHECK_EQ(base->id, 46);

	// 非 boolean を渡せばそれ自身が返る。
	const parse::Entity* self = parse::resolveBaseSolid(model, model.entity(46));
	CHECK(self != nullptr);
	if (self != nullptr)
		CHECK_EQ(self->id, 46);
}

// ---------------------------------------------------------------------------
// 実フィクスチャ（例外なく解決できること）
// ---------------------------------------------------------------------------

TEST(resolves_geometry_on_real_fixture)
{
	// ホームズ君の実モデルに含まれる押し出しソリッドを、要素配置抜き（単位行列）で
	// 解決できること・矩形断面が拾えることを確認する。数値の厳密一致ではなく、
	// パイプライン（プロファイル→押し出し）が実データで通ることの担保。
	bool ok = false;
	Model const model =
		parse::loadIfc(std::string(HOMESKZ_FIXTURES_DIR) + "/サンプル1 (住木邸新築工事).ifc", &ok);
	CHECK(ok);

	int resolved = 0;
	int rectangles = 0;
	for (const int id : model.byType("IFCEXTRUDEDAREASOLID"))
	{
		WorldSolid solid;
		if (parse::resolveExtrudedAreaSolid(model, model.entity(id), Mat4::identity(), solid))
		{
			++resolved;
			CHECK(solid.base().size() >= 3);
			// 押し出しに有限の長さがあること（NaN/inf でない）。
			CHECK(std::isfinite(core::length(solid.extrusion())));
		}
	}
	for (const int id : model.byType("IFCRECTANGLEPROFILEDEF"))
	{
		Profile prof;
		if (parse::resolveProfile(model, model.entity(id), prof) && prof.rectangle)
			++rectangles;
	}
	// 大量の押し出し・矩形断面が解決できる（fixture 統計より数百オーダー）。
	CHECK(resolved > 100);
	CHECK(rectangles > 100);

	// 第 1 オペランド辿りも実 boolean で素ソリッドへ到達する。
	for (const int id : model.byType("IFCBOOLEANRESULT"))
	{
		const parse::Entity* base = parse::resolveBaseSolid(model, model.entity(id));
		CHECK(base != nullptr);
		if (base != nullptr)
			CHECK(base->type != "IFCBOOLEANRESULT" && base->type != "IFCBOOLEANCLIPPINGRESULT");
	}

	// 実 IfcColumn で要素配置（属性 5）が読め、決定的な有限値を返すこと。★親を合成しない
	// ので、resolveObjectPlacement の原点 Z は要素自身の RelativePlacement.Location の Z に
	// 一致する（親階の Z オフセットを含まない）。実データで属性インデックスと非合成を担保する。
	int columns = 0;
	for (const int id : model.byType("IFCCOLUMN"))
	{
		const parse::Entity* col = model.entity(id);
		const Mat4 pl = parse::resolveObjectPlacement(model, col);
		const Vec3 o = pl.transformPoint(Vec3{0.0, 0.0, 0.0});
		CHECK(std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z));

		// 要素自身の RelativePlacement.Location を直接読み、原点と一致することを確認
		// （親非合成の証左。ObjectPlacement=属性 5 → IfcLocalPlacement → RelativePlacement）。
		const parse::Entity* lp = (col != nullptr) ? model.resolve(col->attribute(5)) : nullptr;
		if (lp != nullptr && lp->type == "IFCLOCALPLACEMENT")
		{
			const parse::Entity* a2p = model.resolve(lp->attribute(1));
			Vec3 ownLoc{0.0, 0.0, 0.0};
			if (a2p != nullptr && parse::resolvePoint(model, a2p->attribute(0), ownLoc))
			{
				CHECK(near(o.x, ownLoc.x));
				CHECK(near(o.y, ownLoc.y));
				CHECK(near(o.z, ownLoc.z));
				++columns;
			}
		}
	}
	CHECK(columns > 10);
}

TEST_MAIN();
