//
//	parse/IfcGeometry.cpp
//
//	IFC 配置・断面・押し出しの幾何解決（ROADMAP.md M2）。宣言は IfcGeometry.h、
//	方針・扱う型はそちらの doc コメントを参照。【SDK 非依存】STEP グラフ（parse/Step）
//	と自前幾何型（core/Geometry）だけに依存する。
//

#include "parse/IfcGeometry.h"

#include <cmath>
#include <cstddef>

namespace HomeskzIfcImport::parse
{
	using core::cross;
	using core::dot;
	using core::normalized;

	namespace
	{
		// 配置の再帰合成・boolean 辿りの深さ上限。ホームズ君 IFC の階層は浅い
		// （建物→階→要素で数段）が、循環参照や壊れたグラフでも無限再帰しないよう
		// 実用上十分な上限で打ち切る（超えたら既定へフォールバック）。
		constexpr int kMaxDepth = 64;

		// z にほぼ平行でない参照軸を選び、それと z の外積で z 直交な単位ベクトルを作る。
		// Axis2Placement3D で RefDirection が省略／z と平行なときの X 軸フォールバック。
		Vec3 perpendicularTo(const Vec3& z)
		{
			// z の主要成分が Z 寄りなら X 基準、そうでなければ Z 基準を補助軸に選ぶ。
			const Vec3 helper = (std::abs(z.z) < 0.9) ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
			const Vec3 axis = normalized(cross(helper, z));
			if (core::length(axis) < core::kGeomEps)
				return Vec3{1.0, 0.0, 0.0};
			return axis;
		}

		// IfcLocalPlacement のワールド行列を深さ付きで再帰合成する（公開 API の実体）。
		Mat4 localPlacementImpl(const Model& model, const Entity* placement, int depth)
		{
			if (placement == nullptr || placement->type != "IFCLOCALPLACEMENT" || depth > kMaxDepth)
				return Mat4::identity();

			// IfcLocalPlacement(PlacementRelTo, RelativePlacement)。
			const Mat4 relative =
				resolveAxis2Placement3D(model, model.resolve(placement->attribute(1)));
			const Entity* parent = model.resolve(placement->attribute(0));
			if (parent == nullptr)
				return relative;
			return localPlacementImpl(model, parent, depth + 1) * relative;
		}

		// IfcBooleanResult 系の第 1 オペランドを深さ付きで辿る（公開 API の実体）。
		const Entity* baseSolidImpl(const Model& model, const Entity* item, int depth)
		{
			if (item == nullptr || depth > kMaxDepth)
				return nullptr;
			if (item->type != "IFCBOOLEANRESULT" && item->type != "IFCBOOLEANCLIPPINGRESULT")
				return item;
			// IfcBooleanResult(Operator, FirstOperand, SecondOperand)。第 1 を辿る。
			return baseSolidImpl(model, model.resolve(item->attribute(1)), depth + 1);
		}
	} // namespace

	bool resolveDirection(const Model& model, const Value& ref, Vec3& out)
	{
		const Entity* dir = model.resolve(ref);
		if (dir == nullptr)
			return false;
		// IfcDirection.DirectionRatios は実数のリスト（属性 0）。2D なら Z=0。
		const Value& ratios = dir->attribute(0);
		if (!ratios.isList() || ratios.items.size() < 2)
			return false;
		out.x = ratios.items[0].asReal();
		out.y = ratios.items[1].asReal();
		out.z = (ratios.items.size() >= 3) ? ratios.items[2].asReal() : 0.0;
		return true;
	}

	bool resolvePoint(const Model& model, const Value& ref, Vec3& out)
	{
		const Entity* point = model.resolve(ref);
		if (point == nullptr)
			return false;
		// IfcCartesianPoint.Coordinates は実数のリスト（属性 0）。2D なら Z=0。
		const Value& coords = point->attribute(0);
		if (!coords.isList() || coords.items.size() < 2)
			return false;
		out.x = coords.items[0].asReal();
		out.y = coords.items[1].asReal();
		out.z = (coords.items.size() >= 3) ? coords.items[2].asReal() : 0.0;
		return true;
	}

	Mat4 resolveAxis2Placement3D(const Model& model, const Entity* placement)
	{
		if (placement == nullptr || placement->type != "IFCAXIS2PLACEMENT3D")
			return Mat4::identity();

		// IfcAxis2Placement3D(Location, Axis, RefDirection)。
		Vec3 origin{0.0, 0.0, 0.0};
		resolvePoint(model, placement->attribute(0), origin);

		// Axis(=局所 Z)。省略／縮退なら (0,0,1)。
		Vec3 zAxis{0.0, 0.0, 1.0};
		Vec3 rawZ{0.0, 0.0, 0.0};
		if (resolveDirection(model, placement->attribute(1), rawZ))
		{
			const Vec3 nz = normalized(rawZ);
			if (core::length(nz) >= core::kGeomEps)
				zAxis = nz;
		}

		// RefDirection(≈局所 X)。省略なら (1,0,0)。z 成分を除いて正規化（Gram-Schmidt）。
		Vec3 refX{1.0, 0.0, 0.0};
		Vec3 rawRef{0.0, 0.0, 0.0};
		if (resolveDirection(model, placement->attribute(2), rawRef))
			refX = rawRef;
		Vec3 xAxis = normalized(refX - (zAxis * dot(refX, zAxis)));
		if (core::length(xAxis) < core::kGeomEps) // RefDirection が z と平行（縮退）
			xAxis = perpendicularTo(zAxis);

		const Vec3 yAxis = cross(zAxis, xAxis);
		return Mat4::fromAxes(xAxis, yAxis, zAxis, origin);
	}

	Mat4 resolveAxis2Placement2D(const Model& model, const Entity* placement)
	{
		if (placement == nullptr || placement->type != "IFCAXIS2PLACEMENT2D")
			return Mat4::identity();

		// IfcAxis2Placement2D(Location, RefDirection)。Z 軸は (0,0,1) 固定。
		Vec3 origin{0.0, 0.0, 0.0};
		resolvePoint(model, placement->attribute(0), origin);

		Vec3 refX{1.0, 0.0, 0.0};
		Vec3 rawRef{0.0, 0.0, 0.0};
		if (resolveDirection(model, placement->attribute(1), rawRef))
			refX = rawRef;
		refX.z = 0.0; // 2D 方向（念のため Z を落とす）
		Vec3 xAxis = normalized(refX);
		if (core::length(xAxis) < core::kGeomEps)
			xAxis = Vec3{1.0, 0.0, 0.0};

		// Y は X を左 90°回した (−X.y, X.x)。
		const Vec3 yAxis{-xAxis.y, xAxis.x, 0.0};
		return Mat4::fromAxes(xAxis, yAxis, Vec3{0.0, 0.0, 1.0}, origin);
	}

	Mat4 resolveLocalPlacement(const Model& model, const Entity* placement)
	{
		return localPlacementImpl(model, placement, 0);
	}

	bool resolveProfile(const Model& model, const Entity* profileDef, Profile& out)
	{
		if (profileDef == nullptr)
			return false;

		// --- 矩形断面 -------------------------------------------------------
		if (profileDef->type == "IFCRECTANGLEPROFILEDEF")
		{
			// IfcRectangleProfileDef(ProfileType, ProfileName, Position, XDim, YDim)。
			const double xDim = profileDef->attribute(3).asReal();
			const double yDim = profileDef->attribute(4).asReal();
			if (xDim <= core::kGeomEps || yDim <= core::kGeomEps)
				return false;

			const Mat4 pos =
				resolveAxis2Placement2D(model, model.resolve(profileDef->attribute(2)));
			const double hx = xDim * 0.5;
			const double hy = yDim * 0.5;
			// 中心原点の 4 隅を反時計回り（左下→右下→右上→左上）に並べ、Position を適用。
			const std::array<Vec2, 4> corners{Vec2{-hx, -hy}, Vec2{hx, -hy}, Vec2{hx, hy},
											  Vec2{-hx, hy}};
			out.outer.clear();
			out.outer.reserve(corners.size());
			for (const Vec2& corner : corners)
			{
				const Vec3 p = pos.transformPoint(Vec3{corner.x, corner.y, 0.0});
				out.outer.push_back(Vec2{p.x, p.y});
			}
			out.rectangle = true;
			out.xDim = xDim;
			out.yDim = yDim;
			return true;
		}

		// --- 任意閉断面 -----------------------------------------------------
		if (profileDef->type == "IFCARBITRARYCLOSEDPROFILEDEF")
		{
			// IfcArbitraryClosedProfileDef(ProfileType, ProfileName, OuterCurve)。
			const Entity* curve = model.resolve(profileDef->attribute(2));
			if (curve == nullptr || curve->type != "IFCPOLYLINE")
				return false;
			const Value& points = curve->attribute(0);
			if (!points.isList() || points.items.size() < 3)
				return false;

			std::vector<Vec2> outline;
			outline.reserve(points.items.size());
			for (const Value& ref : points.items)
			{
				Vec3 p{0.0, 0.0, 0.0};
				if (!resolvePoint(model, ref, p))
					return false; // 1 点でも解決できなければ断面ごとスキップ
				outline.push_back(Vec2{p.x, p.y});
			}
			// 明示的に閉じている（始点＝終点）ときは終点の重複を落とす。
			if (outline.size() >= 2)
			{
				const Vec2& first = outline.front();
				const Vec2& last = outline.back();
				if (std::abs(first.x - last.x) < core::kGeomEps &&
					std::abs(first.y - last.y) < core::kGeomEps)
					outline.pop_back();
			}
			if (outline.size() < 3)
				return false;

			out.outer = outline;
			out.rectangle = false;
			out.xDim = 0.0;
			out.yDim = 0.0;
			return true;
		}

		return false; // 未対応のプロファイル型
	}

	std::vector<Vec3> WorldSolid::top() const
	{
		std::vector<Vec3> result;
		result.reserve(base.size());
		for (const Vec3& p : base)
			result.push_back(p + extrusion);
		return result;
	}

	bool resolveExtrudedAreaSolid(const Model& model, const Entity* solid, const Mat4& placement,
								  WorldSolid& out)
	{
		if (solid == nullptr || solid->type != "IFCEXTRUDEDAREASOLID")
			return false;

		// IfcExtrudedAreaSolid(SweptArea, Position, ExtrudedDirection, Depth)。
		Profile profile;
		if (!resolveProfile(model, model.resolve(solid->attribute(0)), profile))
			return false;
		if (profile.outer.empty())
			return false;

		// Position はプロファイルを 3D（オブジェクト座標）へ据える。ワールド行列と
		// 合成すると、プロファイル 2D 点 (u,v,0) をそのまま世界系へ写せる。
		const Mat4 position = resolveAxis2Placement3D(model, model.resolve(solid->attribute(1)));
		const Mat4 full = placement * position;

		// 押し出し方向は Position 座標系。単位化して Depth を掛け、同じ行列で世界系へ。
		Vec3 dir{0.0, 0.0, 1.0};
		Vec3 rawDir{0.0, 0.0, 0.0};
		if (resolveDirection(model, solid->attribute(2), rawDir))
		{
			const Vec3 nd = normalized(rawDir);
			if (core::length(nd) >= core::kGeomEps)
				dir = nd;
		}
		const double depth = solid->attribute(3).asReal();

		out.base.clear();
		out.base.reserve(profile.outer.size());
		for (const Vec2& p : profile.outer)
			out.base.push_back(full.transformPoint(Vec3{p.x, p.y, 0.0}));
		out.extrusion = full.transformDirection(dir * depth);
		return true;
	}

	const Entity* resolveBaseSolid(const Model& model, const Entity* item)
	{
		return baseSolidImpl(model, item, 0);
	}
} // namespace HomeskzIfcImport::parse
