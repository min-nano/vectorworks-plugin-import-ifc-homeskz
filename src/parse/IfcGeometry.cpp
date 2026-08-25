//
//	parse/IfcGeometry.cpp
//
//	IFC 配置・断面・押し出しの幾何解決（docs/DEV-NOTES.md M2）。宣言は IfcGeometry.h、
//	方針・扱う型はそちらの doc コメントを参照。【SDK 非依存】STEP グラフ（parse/Step）
//	と自前幾何型（core/Geometry）だけに依存する。
//

#include "parse/IfcGeometry.h"
#include "parse/IfcAttr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

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

		// IfcBooleanResult 系の第 1 オペランドを深さ付きで辿る（公開 API の実体）。
		const Entity* baseSolidImpl(const Model& model, const Entity* item, int depth)
		{
			if (item == nullptr || depth > kMaxDepth)
				return nullptr;
			if (item->type != "IFCBOOLEANRESULT" && item->type != "IFCBOOLEANCLIPPINGRESULT")
				return item;
			// IfcBooleanResult(Operator, FirstOperand, SecondOperand)。第 1 を辿る。
			return baseSolidImpl(
				model, model.resolve(item->attribute(attr::kBooleanResultFirstOperand)), depth + 1);
		}

		// 要素の形状表現アイテムを出現順に訪ねる（Representation → Representations → Items）。
		// visit が true を返したら打ち切る。**素のソリッド探索（firstExtrudedSolid）と
		// 削り取り収集（elementVoidSolids）が同じ道筋を辿る**ので、走査はここに 1 つだけ置く。
		template <typename Visit>
		void forEachShapeItem(const Model& model, const Entity* element, Visit visit)
		{
			if (element == nullptr)
				return;
			// IfcProduct.Representation（属性 6）＝ IfcProductDefinitionShape。その
			// Representations（属性 2）が IfcShapeRepresentation の列で、各 Items（属性 3）に
			// 形状アイテムが入る（Python 版 rep.Representations → shape_rep.Items と同じ道筋。
			// Body/Axis 等の表現識別子で絞らないのも Python 版と同じ）。
			const Entity* shape = model.resolve(element->attribute(attr::kProductRepresentation));
			if (shape == nullptr)
				return;
			const Value& representations =
				shape->attribute(attr::kProductDefinitionShapeRepresentations);
			if (!representations.isList())
				return;

			for (const Value& repRef : representations.items)
			{
				const Entity* rep = model.resolve(repRef);
				if (rep == nullptr)
					continue;
				const Value& items = rep->attribute(attr::kShapeRepresentationItems);
				if (!items.isList())
					continue;
				for (const Value& itemRef : items.items)
				{
					if (visit(model.resolve(itemRef)))
						return;
				}
			}
		}

		// 差演算アイテムか（IfcBooleanClippingResult は常に差演算、IfcBooleanResult は
		// Operator が DIFFERENCE のとき）。列挙値は ".DIFFERENCE." のように前後をピリオドで
		// 囲んで書かれるが、parse/Step は記号部分（DIFFERENCE）だけを text に入れる。
		bool isDifferenceItem(const Entity& item)
		{
			if (item.type == "IFCBOOLEANCLIPPINGRESULT")
				return true;
			return item.attribute(attr::kBooleanResultOperator).text == "DIFFERENCE";
		}
	} // namespace

	bool resolveDirection(const Model& model, const Value& ref, Vec3& out)
	{
		const Entity* dir = model.resolve(ref);
		if (dir == nullptr)
			return false;
		// IfcDirection.DirectionRatios は実数のリスト。2D なら Z=0。
		const Value& ratios = dir->attribute(attr::kDirectionRatios);
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
		// IfcCartesianPoint.Coordinates は実数のリスト。2D なら Z=0。
		const Value& coords = point->attribute(attr::kCartesianPointCoordinates);
		if (!coords.isList() || coords.items.size() < 2)
			return false;
		out.x = coords.items[0].asReal();
		out.y = coords.items[1].asReal();
		out.z = (coords.items.size() >= 3) ? coords.items[2].asReal() : 0.0;
		return true;
	}

	bool resolvePoint2D(const Model& model, const Value& ref, Vec2& out)
	{
		// 3D 点でも Z は捨てる（通り芯のように平面だけで決まる要素向け）。判定規則は
		// resolvePoint と完全に同じにしたいので、そちらへ委譲して XY だけを写す。
		Vec3 point{0.0, 0.0, 0.0};
		if (!resolvePoint(model, ref, point))
			return false;
		out = Vec2{point.x, point.y};
		return true;
	}

	Mat4 resolveAxis2Placement3D(const Model& model, const Entity* placement)
	{
		if (placement == nullptr || placement->type != "IFCAXIS2PLACEMENT3D")
			return Mat4::identity();

		// IfcAxis2Placement3D(Location, Axis, RefDirection)。
		Vec3 origin{0.0, 0.0, 0.0};
		resolvePoint(model, placement->attribute(attr::kAxis2PlacementLocation), origin);

		// Axis(=局所 Z)。省略／縮退なら (0,0,1)。
		Vec3 zAxis{0.0, 0.0, 1.0};
		Vec3 rawZ{0.0, 0.0, 0.0};
		if (resolveDirection(model, placement->attribute(attr::kAxis2PlacementAxis), rawZ))
		{
			const Vec3 nz = normalized(rawZ);
			if (core::length(nz) >= core::kGeomEps)
				zAxis = nz;
		}

		// RefDirection(≈局所 X)。省略なら (1,0,0)。z 成分を除いて正規化（Gram-Schmidt）。
		Vec3 refX{1.0, 0.0, 0.0};
		Vec3 rawRef{0.0, 0.0, 0.0};
		if (resolveDirection(model, placement->attribute(attr::kAxis2PlacementRefDirection),
							 rawRef))
			refX = rawRef;
		Vec3 xAxis = normalized(refX - (zAxis * dot(refX, zAxis)));
		if (core::length(xAxis) < core::kGeomEps) // RefDirection が z と平行（縮退）
			xAxis = perpendicularTo(zAxis);

		const Vec3 yAxis = cross(zAxis, xAxis);
		return Mat4::fromAxes(xAxis, yAxis, zAxis, origin);
	}

	Mat4 resolveObjectPlacement(const Model& model, const Entity* element)
	{
		if (element == nullptr)
			return Mat4::identity();

		// element.ObjectPlacement（IfcLocalPlacement）→ その RelativePlacement のみ。
		// 親 PlacementRelTo は辿らない（ヘッダの★参照。Python 版と一致）。
		const Entity* placement = model.resolve(element->attribute(attr::kProductObjectPlacement));
		if (placement == nullptr || placement->type != "IFCLOCALPLACEMENT")
			return Mat4::identity();
		return resolveAxis2Placement3D(
			model, model.resolve(placement->attribute(attr::kLocalPlacementRelativePlacement)));
	}

	bool resolveProfile(const Model& model, const Entity* profileDef, Profile& out)
	{
		if (profileDef == nullptr)
			return false;

		// --- 矩形断面 -------------------------------------------------------
		if (profileDef->type == "IFCRECTANGLEPROFILEDEF")
		{
			// IfcRectangleProfileDef(ProfileType, ProfileName, Position, XDim, YDim)。
			const double xDim = profileDef->attribute(attr::kRectangleProfileXDim).asReal();
			const double yDim = profileDef->attribute(attr::kRectangleProfileYDim).asReal();
			if (xDim <= core::kGeomEps || yDim <= core::kGeomEps)
				return false;

			const double hx = xDim * 0.5;
			const double hy = yDim * 0.5;
			// 中心原点の 4 隅を反時計回り（左下→右下→右上→左上）に並べる。
			// Position は Location の平行移動のみ足す（Python 版 _profile_points に一致。
			// RefDirection の回転は反映しない）。
			Vec2 offset{0.0, 0.0};
			const Entity* pos = model.resolve(profileDef->attribute(attr::kProfilePosition));
			if (pos != nullptr && pos->type == "IFCAXIS2PLACEMENT2D")
			{
				Vec3 loc{0.0, 0.0, 0.0};
				if (resolvePoint(model, pos->attribute(attr::kAxis2PlacementLocation), loc))
					offset = Vec2{loc.x, loc.y};
			}
			const std::array<Vec2, 4> corners{Vec2{-hx, -hy}, Vec2{hx, -hy}, Vec2{hx, hy},
											  Vec2{-hx, hy}};
			out.outer.clear();
			out.outer.reserve(corners.size());
			for (const Vec2& corner : corners)
				out.outer.push_back(corner + offset);
			out.rectangle = true;
			out.xDim = xDim;
			out.yDim = yDim;
			return true;
		}

		// --- 任意閉断面 -----------------------------------------------------
		// IFCARBITRARYPROFILEDEFWITHVOIDS は IfcArbitraryClosedProfileDef の派生で、
		// 属性の並びは (ProfileType, ProfileName, OuterCurve, InnerCurves)。外形の
		// 位置（属性 2）は同じなので、同じ経路で外形だけを読む。
		// ［既知の制限］InnerCurves（階段の吹抜け等の開口）は無視するので、床は開口を
		// 塞いだ形で入る。開口ごと落として床を丸ごと失うよりは良い、という判断
		// （CLAUDE.md「1 要素の欠損で全体を止めない」）。開口の再現は今後の課題
		// （docs/DEV-NOTES.md「残っている宿題」）。
		if (profileDef->type == "IFCARBITRARYCLOSEDPROFILEDEF" ||
			profileDef->type == "IFCARBITRARYPROFILEDEFWITHVOIDS")
		{
			// IfcArbitraryClosedProfileDef(ProfileType, ProfileName, OuterCurve)。属性番号は
			// IfcRectangleProfileDef の Position と同じ 2 だが、指すのは外形曲線。
			const Entity* curve =
				model.resolve(profileDef->attribute(attr::kArbitraryProfileOuterCurve));
			if (curve == nullptr || curve->type != "IFCPOLYLINE")
				return false;
			const Value& points = curve->attribute(attr::kPolylinePoints);
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

	std::vector<Vec3> WorldSolid::base() const
	{
		// プロファイル 2D 頂点 (u,v) を origin + xAxis·u + yAxis·v で世界系へ写す
		// （Python 版 _footprint / to_world の base と一致）。
		std::vector<Vec3> result;
		result.reserve(profile.size());
		for (const Vec2& p : profile)
			result.push_back(origin + (xAxis * p.x) + (yAxis * p.y));
		return result;
	}

	Vec3 WorldSolid::extrusion() const
	{
		return extrudeDir * depth;
	}

	std::vector<Vec3> WorldSolid::top() const
	{
		const Vec3 disp = extrusion();
		std::vector<Vec3> result = base();
		for (Vec3& p : result)
			p = p + disp;
		return result;
	}

	bool resolveExtrudedAreaSolid(const Model& model, const Entity* solid, const Mat4& placement,
								  WorldSolid& out)
	{
		if (solid == nullptr || solid->type != "IFCEXTRUDEDAREASOLID")
			return false;

		// IfcExtrudedAreaSolid(SweptArea, Position, ExtrudedDirection, Depth)。
		Profile profile;
		if (!resolveProfile(
				model, model.resolve(solid->attribute(attr::kExtrudedAreaSolidSweptArea)), profile))
			return false;
		if (profile.outer.empty())
			return false;

		// Position はプロファイルを 3D（オブジェクト座標）へ据える。要素配置と合成すると
		// プロファイル 2D 点 (u,v,0) をそのまま世界系へ写せる（Python 版 _compose に対応）。
		const Mat4 position = resolveAxis2Placement3D(
			model, model.resolve(solid->attribute(attr::kExtrudedAreaSolidPosition)));
		const Mat4 full = placement * position;

		// 押し出し方向は Position 座標系。単位化して同じ基底で世界系へ（長さは depth 別持ち）。
		Vec3 dir{0.0, 0.0, 1.0};
		Vec3 rawDir{0.0, 0.0, 0.0};
		if (resolveDirection(model, solid->attribute(attr::kExtrudedAreaSolidDirection), rawDir))
		{
			const Vec3 nd = normalized(rawDir);
			if (core::length(nd) >= core::kGeomEps)
				dir = nd;
		}

		// 配置基底（origin/軸）を full から取り出す（Python 版 _Placement=(origin,lX,lY,lZ)）。
		out.origin = full.transformPoint(Vec3{0.0, 0.0, 0.0});
		out.xAxis = full.transformDirection(Vec3{1.0, 0.0, 0.0});
		out.yAxis = full.transformDirection(Vec3{0.0, 1.0, 0.0});
		out.zAxis = full.transformDirection(Vec3{0.0, 0.0, 1.0});
		out.extrudeDir = full.transformDirection(dir);
		out.depth = solid->attribute(attr::kExtrudedAreaSolidDepth).asReal();
		out.profile = profile.outer;
		out.rectangle = profile.rectangle;
		out.xDim = profile.xDim;
		out.yDim = profile.yDim;
		return true;
	}

	const Entity* resolveBaseSolid(const Model& model, const Entity* item)
	{
		return baseSolidImpl(model, item, 0);
	}

	std::vector<const Entity*> elementVoidSolids(const Model& model, const Entity* element)
	{
		std::vector<const Entity*> voids;
		forEachShapeItem(
			model, element,
			[&model, &voids](const Entity* item)
			{
				// ((base − void1) − void2) のように第 1 オペランドを辿りながら、
				// 各差演算の第 2 オペランド（削り取る側）を集める。
				for (int depth = 0; item != nullptr && depth <= kMaxDepth; ++depth)
				{
					if (item->type != "IFCBOOLEANRESULT" &&
						item->type != "IFCBOOLEANCLIPPINGRESULT")
						break;
					const Entity* second =
						model.resolve(item->attribute(attr::kBooleanResultSecondOperand));
					if (isDifferenceItem(*item) && second != nullptr &&
						second->type == "IFCEXTRUDEDAREASOLID")
						voids.push_back(second);
					item = model.resolve(item->attribute(attr::kBooleanResultFirstOperand));
				}
				return false; // すべてのアイテムを見る
			});
		return voids;
	}

	const Entity* firstExtrudedSolid(const Model& model, const Entity* element)
	{
		// 最初に見つかった押し出しを採る（Python 版 _first_extruded_solid と同じ）。差演算
		// （端部が他材で削られた形状）は第 1 オペランド＝素のソリッドを使う。
		const Entity* found = nullptr;
		forEachShapeItem(model, element,
						 [&model, &found](const Entity* item)
						 {
							 const Entity* solid = resolveBaseSolid(model, item);
							 if (solid == nullptr || solid->type != "IFCEXTRUDEDAREASOLID")
								 return false;
							 found = solid;
							 return true; // 打ち切り
						 });
		return found;
	}

	bool resolveElementWorldSolid(const Model& model, const Entity* element, WorldSolid& out)
	{
		const Entity* solid = firstExtrudedSolid(model, element);
		if (solid == nullptr)
			return false;
		// 要素配置（RelativePlacement のみ＝親非合成）の上にアイテム配置を合成する。
		return resolveExtrudedAreaSolid(model, solid, resolveObjectPlacement(model, element), out);
	}

	bool roofPlane(const Model& model, const Entity* element, RoofPlane& out)
	{
		WorldSolid solid;
		if (!resolveElementWorldSolid(model, element, solid))
			return false;

		// 平面外形はプロファイル頂点をワールドへ写した底面ループ（origin + xAxis·u + yAxis·v）。
		// 3 点未満（面にならない）屋根版は扱わない（Python 版と同じ関門）。
		std::vector<Vec3> vertices = solid.base();
		if (vertices.size() < 3)
			return false;

		// 法線は配置の局所 Z 軸。上向き（z 成分 ≥ 0）に揃える（平面式・勾配方向は符号反転に
		// 対して不変。上向きに固定することで勾配計算の分母 nz を正にできる）。
		Vec3 normal = solid.zAxis;
		if (normal.z < 0.0)
			normal = normal * -1.0;

		out.vertices = std::move(vertices);
		out.normal = normal;
		return true;
	}

	double RoofSlope::zAt(double x, double y, double elevationOffset) const
	{
		// 平面式: 法線 n との内積が一定。n の水平成分（rise·down）と鉛直成分（run）から
		// z = origin.z − (n_h·(p − origin_h)) / n_z。法線の符号反転に対して不変
		// （rise/run/down が揃って反転し比が変わらない）。
		const double nx = down.x * rise;
		const double ny = down.y * rise;
		return origin.z - (((nx * (x - origin.x)) + (ny * (y - origin.y))) / run) + elevationOffset;
	}

	std::vector<Vec2> RoofSlope::plan(const RoofPlane& plane)
	{
		std::vector<Vec2> points;
		points.reserve(plane.vertices.size());
		for (const Vec3& v : plane.vertices)
			points.push_back(Vec2{v.x, v.y});
		return points;
	}

	void RoofSlope::projectionRange(const std::vector<Vec2>& points, const Vec2& dir,
									double& outMin, double& outMax)
	{
		if (points.empty())
		{
			outMin = 0.0;
			outMax = 0.0;
			return;
		}
		outMin = (points.front().x * dir.x) + (points.front().y * dir.y);
		outMax = outMin;
		for (const Vec2& p : points)
		{
			const double t = (p.x * dir.x) + (p.y * dir.y);
			outMin = std::min(outMin, t);
			outMax = std::max(outMax, t);
		}
	}

	bool roofSlope(const RoofPlane& plane, RoofSlope& out, double flatTol)
	{
		if (plane.vertices.empty())
			return false;

		const double nx = plane.normal.x;
		const double ny = plane.normal.y;
		const double nz = plane.normal.z;
		const double dh = std::hypot(nx, ny);
		// ほぼ水平な面は勾配方向が定まらない。鉛直な面（法線が水平）は平面式の分母 nz が
		// 0 になり天端 Z が定まらない（RoofPlane の normal は上向きなので nz >= 0）。
		if (dh <= flatTol || nz <= flatTol)
			return false;

		// 勾配方向 d（最急降下＝水平法線方向）。+d へ進むと天端 Z は下がる（軒側）。
		out.down = Vec2{nx / dh, ny / dh};
		// 掃引方向 e（軒・棟に平行。勾配方向に直交）。
		out.along = Vec2{-out.down.y, out.down.x};
		out.rise = dh;
		out.run = nz;
		out.origin = plane.vertices.front();
		return true;
	}

	void zTopAndThickness(const WorldSolid& solid, double& outTop, double& outThickness)
	{
		// 底面ループと天面ループの Z を集めて最大／振幅を採る（Python 版と同じ手順。
		// 押し出し方向が鉛直でも水平でも同じ式で正しい）。
		const std::vector<Vec3> baseLoop = solid.base();
		if (baseLoop.empty())
		{
			outTop = 0.0;
			outThickness = 0.0;
			return;
		}
		const double dz = solid.extrudeDir.z * solid.depth;
		double minZ = baseLoop.front().z;
		double maxZ = baseLoop.front().z;
		for (const Vec3& p : baseLoop)
		{
			for (const double z : {p.z, p.z + dz})
			{
				minZ = std::min(minZ, z);
				maxZ = std::max(maxZ, z);
			}
		}
		outTop = maxZ;
		outThickness = maxZ - minZ;
	}

	std::vector<Vec2> footprint(const WorldSolid& solid)
	{
		// 鉛直押し出し（床版・底盤）: 底面ループの XY がそのまま平面外形。
		if (std::abs(solid.extrudeDir.z) > kVerticalExtrudeTol)
		{
			std::vector<Vec2> result;
			result.reserve(solid.profile.size());
			for (const Vec3& p : solid.base())
				result.push_back(Vec2{p.x, p.y});
			return result;
		}

		// 水平押し出し（立上り・地中梁）: プロファイルは鉛直面内にあるので、断面の水平方向
		// の幅（u の範囲）を押し出し方向へ掃引した矩形を平面外形にする。
		if (solid.profile.empty())
			return {};
		double uMin = solid.profile.front().x;
		double uMax = solid.profile.front().x;
		for (const Vec2& p : solid.profile)
		{
			uMin = std::min(uMin, p.x);
			uMax = std::max(uMax, p.x);
		}
		const auto corner = [&solid](double u, double t)
		{
			const Vec3 p =
				solid.origin + (solid.xAxis * u) + (solid.extrudeDir * (solid.depth * t));
			return Vec2{p.x, p.y};
		};
		return {corner(uMin, 0.0), corner(uMax, 0.0), corner(uMax, 1.0), corner(uMin, 1.0)};
	}
} // namespace HomeskzIfcImport::parse
