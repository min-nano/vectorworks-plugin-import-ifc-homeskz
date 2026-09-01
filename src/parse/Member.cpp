//
//	parse/Member.cpp
//
//	横架材解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。
//

#include "parse/Member.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
#include "parse/IfcGeometry.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::MemberCommand;
	using core::StoryBoundCommand;
	using core::Vec2;
	using core::Vec3;

	namespace
	{
		// 取り合い（T 字の甲乙梁・L 字の出隅）の判定に使う許容値（mm）。
		constexpr double kZOverlapTol = 1.0; // これ以下の Z 重なりは干渉とみなさない
		constexpr double kParallelTol = 1e-6; // 軸がほぼ平行な相手は対象外（継ぎ手）
		constexpr double kAlongTol = 1.0; // 相手軸方向の範囲判定の余裕（角部も含める）
		constexpr double kFaceTol = 1.0; // 面ちょうどで止まる端部も相手に載っているとみなす余裕
		constexpr double kMinLength = 1.0; // 調整後に描かれる長さがこれ未満なら調整しない
		constexpr double kSymmetryTol = 1.0; // 出隅で相互の食い込み量がこの差以内なら対称とみなす

		// Body 表現の識別子（IfcShapeRepresentation.RepresentationIdentifier）。
		constexpr const char* kBodyRepresentation = "Body";

		// 平面座標（センタリング済み）で表した横架材の中心線。食い込み調整の作業用。
		// axis は単位ベクトル、length は平面投影長。傾斜梁・退化した材は valid=false。
		struct MemberGeom
		{
			Vec2 start;
			Vec2 end;
			Vec2 axis;
			double length = 0.0;
			bool valid = false;
		};

		// 2 つの横架材の Z 範囲（[天端 − せい, 天端]）が重なるか。重なりが許容値以下（段差で
		// 上下に離れている等）なら干渉とみなさない。式は core/Document.h の zRangesOverlap
		// （仕口の取り付き・登り梁の受け材と共有。傾斜梁は geomOf が先に弾くので、ここは
		// elevation だけで天端を代表できる）。
		bool zOverlaps(double elevA, double heightA, double elevB, double heightB)
		{
			return core::zRangesOverlap(elevA - heightA, elevA, elevB - heightB, elevB,
										kZOverlapTol);
		}

		// 端部 1 つぶんの調整量。reach ＝ 端点を芯線へ載せるための移動量（−outward 方向）、
		// setback ＝ そこから材の端（相手の手前の面）まで戻す量（>= 0。端部オフセットは
		// −setback）。取り合う相手がいなければ両方 0 ＝ 端点も材の端も動かさない。
		struct EndAdjust
		{
			double reach = 0.0;
			double setback = 0.0;
		};

		// 相手 B の端部が自分（self）の**途中**へ取り付いているか。取り付いているなら
		// 相互に負けている＝勝ち負けが付かない（従来の「相互の食い込み量が同等なら触らない」
		// と同じ結論）。B 始端の外向きは −軸、終端は +軸。
		bool jointsSelfInterior(const MemberGeom& b, const MemberGeom& self, double selfHalfWidth)
		{
			return memberEndJoint(b.start, Vec2{-b.axis.x, -b.axis.y}, self.start, self.axis,
								  self.length, selfHalfWidth)
					   .interior ||
				   memberEndJoint(b.end, b.axis, self.start, self.axis, self.length, selfHalfWidth)
					   .interior;
		}

		// 端点 point・外向き outward が相手 b に負けるか。
		//   * 相手の**途中**へ取り付いている（T 字）なら負け。相手が通し材だからで、食い込みが
		//     無い（既に相手の面で止まっている）取り合いもこれで拾える。ただし相手の端部も
		//     自分の途中へ取り付いているとき（相互）は勝ち負けが付かない。
		//   * 相手の**端部**での取り合い（L 字の出隅）は、従来どおり**相互の食い込み量**で
		//     決める。自分の方が深く食い込むなら負け、同等（同寸の出隅・火打）なら触らない
		//     ——ここを外すと、幅の違う出隅で負け側が詰められなくなる。
		bool losesTo(const MemberJoint& joint, const Vec2& point, const Vec2& outward,
					 const MemberGeom& self, double selfHalfWidth, const MemberGeom& b,
					 double bHalfWidth)
		{
			if (!joint.found)
				return false;
			if (joint.interior)
				return !jointsSelfInterior(b, self, selfHalfWidth);

			const double sAB =
				memberPenetrationDepth(point, outward, b.start, b.axis, b.length, bHalfWidth);
			const double sBA =
				std::max(memberPenetrationDepth(b.start, Vec2{-b.axis.x, -b.axis.y}, self.start,
												self.axis, self.length, selfHalfWidth),
						 memberPenetrationDepth(b.end, b.axis, self.start, self.axis, self.length,
												selfHalfWidth));
			return sAB > sBA + kSymmetryTol;
		}

		// 端点 point・外向き outward が相手梁群 others のどれに負けるかを決め、その調整量を
		// 返す。複数の相手に取り付くときは、材の端が最も手前（すべての面より外側）になる
		// 相手＝ reach + setback が最大の相手が支配する（従来の「食い込み量の最大値を採る」と
		// 同じ選び方）。どれにも負けなければ両方 0 ＝ 端点も材の端も動かさない。
		EndAdjust adjustForEnd(const Vec2& point, const Vec2& outward, const MemberGeom& self,
							   double selfHalfWidth,
							   const std::vector<std::pair<MemberGeom, double>>& others)
		{
			EndAdjust best;
			bool found = false;
			for (const std::pair<MemberGeom, double>& other : others)
			{
				const MemberGeom& b = other.first;
				const MemberJoint joint =
					memberEndJoint(point, outward, b.start, b.axis, b.length, other.second);
				if (!losesTo(joint, point, outward, self, selfHalfWidth, b, other.second))
					continue;

				if (!found || joint.reach + joint.setback > best.reach + best.setback)
					best = EndAdjust{joint.reach, joint.setback};
				found = true;
			}
			return best;
		}

		// 命令 1 件を平面の中心線へ落とす（傾斜梁・退化した材は valid=false）。
		MemberGeom geomOf(const MemberCommand& command)
		{
			MemberGeom geom;
			if (std::abs(command.endElevation - command.elevation) > kSlopeTol)
				return geom; // 傾斜梁は調整対象外
			const double dx = command.end.x - command.start.x;
			const double dy = command.end.y - command.start.y;
			const double length = std::hypot(dx, dy);
			if (length <= 0.0)
				return geom;
			geom.start = command.start;
			geom.end = command.end;
			geom.axis = Vec2{dx / length, dy / length};
			geom.length = length;
			geom.valid = true;
			return geom;
		}

		// IfcMaterial / IfcMaterialList / IfcMaterialLayerSetUsage から材種名を取り出す。
		// 対応しない型は空文字。
		std::string materialNameOf(const Model& model, const Entity* material)
		{
			if (material == nullptr)
				return {};
			if (material->type == "IFCMATERIAL")
				return entityString(*material, attr::kMaterialName);
			if (material->type == "IFCMATERIALLIST")
			{
				const Value& materials = material->attribute(attr::kMaterialListMaterials);
				if (materials.isList() && !materials.items.empty())
				{
					const Entity* first = model.resolve(materials.items.front());
					if (first != nullptr)
						return entityString(*first, attr::kMaterialName);
				}
				return {};
			}
			if (material->type == "IFCMATERIALLAYERSETUSAGE")
			{
				const Entity* layerSet =
					model.resolve(material->attribute(attr::kMaterialLayerSetUsageForLayerSet));
				if (layerSet == nullptr)
					return {};
				const Value& layers = layerSet->attribute(attr::kMaterialLayerSetLayers);
				if (!layers.isList() || layers.items.empty())
					return {};
				const Entity* layer = model.resolve(layers.items.front());
				if (layer == nullptr)
					return {};
				const Entity* layerMaterial =
					model.resolve(layer->attribute(attr::kMaterialLayerMaterial));
				if (layerMaterial == nullptr)
					return {};
				return entityString(*layerMaterial, attr::kMaterialName);
			}
			return {};
		}
	} // namespace

	bool isMemberElement(const Entity& element)
	{
		return element.type == "IFCBEAM" || element.type == "IFCMEMBER";
	}

	std::string makeMemberId(double width, double height, const std::string& material)
	{
		const std::string section =
			std::to_string(std::llround(width)) + "×" + std::to_string(std::llround(height));
		return material.empty() ? section : section + " - " + material;
	}

	bool memberPlacement3D(const Model& model, const Entity& element, MemberPlacement& out)
	{
		// ローカル配置原点（鎖の解決は parse/IfcGeometry の resolveLocalPlacementOrigin）。
		// 座標が 2 要素しか無い（Z が無い）梁は、呼び出し側がレイヤ基準高さへフォールバックする。
		LocalOrigin origin;
		const Entity* axisPlacement = nullptr;
		if (!resolveLocalPlacementOrigin(model, element, origin, &axisPlacement))
			return false;

		MemberPlacement result;
		result.x = origin.x;
		result.y = origin.y;
		result.z = origin.z;
		result.hasZ = origin.hasZ;

		// 梁軸＝押し出し方向はローカル Z（Axis）。未設定なら (1,0,0)。
		Vec3 axis;
		if (resolveDirection(model, axisPlacement->attribute(attr::kAxis2PlacementAxis), axis))
		{
			const double norm =
				std::sqrt((axis.x * axis.x) + (axis.y * axis.y) + (axis.z * axis.z));
			if (norm > 0.0)
				result.axis = Vec3{axis.x / norm, axis.y / norm, axis.z / norm};
		}
		out = result;
		return true;
	}

	bool memberProfileDims(const Model& model, const Entity& element, MemberProfile& out)
	{
		const Entity* shape = model.resolve(element.attribute(attr::kProductRepresentation));
		if (shape == nullptr)
			return false;
		const Value& representations =
			shape->attribute(attr::kProductDefinitionShapeRepresentations);
		if (!representations.isList())
			return false;

		for (const Value& representationRef : representations.items)
		{
			const Entity* representation = model.resolve(representationRef);
			if (representation == nullptr)
				continue;
			// Body 表現だけを見る。
			if (entityString(*representation, attr::kShapeRepresentationIdentifier) !=
				kBodyRepresentation)
				continue;

			const Value& items = representation->attribute(attr::kShapeRepresentationItems);
			if (!items.isList())
				continue;
			for (const Value& itemRef : items.items)
			{
				const Entity* item = model.resolve(itemRef);
				// 差演算は剥がさない（剥がすと登り梁の任意断面と見分けが付かなくなる）。
				if (item == nullptr || item->type != "IFCEXTRUDEDAREASOLID")
					continue;
				const Entity* area =
					model.resolve(item->attribute(attr::kExtrudedAreaSolidSweptArea));
				if (area == nullptr || area->type != "IFCRECTANGLEPROFILEDEF")
					continue;

				out.width = area->attribute(attr::kRectangleProfileXDim).asReal();
				out.height = area->attribute(attr::kRectangleProfileYDim).asReal();
				out.length = item->attribute(attr::kExtrudedAreaSolidDepth).asReal();
				return true;
			}
		}
		return false;
	}

	bool slopedMemberGeometry(const Model& model, const Entity& element, SlopedMemberGeometry& out)
	{
		WorldSolid solid;
		if (!resolveElementWorldSolid(model, &element, solid))
			return false;
		if (solid.rectangle)
			return false; // 矩形断面は通常経路（memberProfileDims）が処理する
		if (solid.profile.size() != 4)
			return false; // 対象は平行四辺形（4 頂点）のみ。火打・筋かい等は除外する

		double uMin = solid.profile.front().x;
		double uMax = uMin;
		double vMin = solid.profile.front().y;
		double vMax = vMin;
		for (const Vec2& point : solid.profile)
		{
			uMin = std::min(uMin, point.x);
			uMax = std::max(uMax, point.x);
			vMin = std::min(vMin, point.y);
			vMax = std::max(vMax, point.y);
		}
		const double uSpan = uMax - uMin;
		const double vSpan = vMax - vMin;
		if (uSpan <= 0.0 || vSpan <= 0.0)
			return false;

		// 長さ軸 ＝ プロファイル 2D で span の大きい座標。せい ＝ もう一方の span。
		// 幅（厚み）＝ 押し出し長。長辺（材の長さ方向）は長さ軸に沿う辺、端辺（直切り）は
		// もう 1 対で、その中心が中心軸の両端になる。
		const bool lengthIsU = uSpan >= vSpan;
		const double height = lengthIsU ? vSpan : uSpan;
		const auto along = [lengthIsU](const Vec2& edge)
		{ return lengthIsU ? std::abs(edge.x) : std::abs(edge.y); };

		const std::vector<Vec2>& points = solid.profile;
		const Vec2 e0{points[1].x - points[0].x, points[1].y - points[0].y};
		const Vec2 e1{points[2].x - points[1].x, points[2].y - points[1].y};
		Vec2 endA;
		Vec2 endB;
		if (along(e0) >= along(e1))
		{
			// 長辺 ＝ 辺 0-1・2-3、端辺 ＝ 辺 1-2・3-0
			endA = Vec2{(points[1].x + points[2].x) / 2.0, (points[1].y + points[2].y) / 2.0};
			endB = Vec2{(points[3].x + points[0].x) / 2.0, (points[3].y + points[0].y) / 2.0};
		}
		else
		{
			// 長辺 ＝ 辺 1-2・3-0、端辺 ＝ 辺 0-1・2-3
			endA = Vec2{(points[0].x + points[1].x) / 2.0, (points[0].y + points[1].y) / 2.0};
			endB = Vec2{(points[2].x + points[3].x) / 2.0, (points[2].y + points[3].y) / 2.0};
		}

		// プロファイル 2D 座標 → ワールド。厚みの中央（押し出し方向へ depth/2）へ寄せると
		// 断面中心になる。
		const double halfWidth = solid.depth / 2.0;
		const auto toWorld = [&solid, halfWidth](const Vec2& c)
		{
			return Vec3{solid.origin.x + (solid.xAxis.x * c.x) + (solid.yAxis.x * c.y) +
							(solid.extrudeDir.x * halfWidth),
						solid.origin.y + (solid.xAxis.y * c.x) + (solid.yAxis.y * c.y) +
							(solid.extrudeDir.y * halfWidth),
						solid.origin.z + (solid.xAxis.z * c.x) + (solid.yAxis.z * c.y) +
							(solid.extrudeDir.z * halfWidth)};
		};

		const Vec3 worldA = toWorld(endA);
		const Vec3 worldB = toWorld(endB);
		const Vec3 axis{worldB.x - worldA.x, worldB.y - worldA.y, worldB.z - worldA.z};
		const double length = std::sqrt((axis.x * axis.x) + (axis.y * axis.y) + (axis.z * axis.z));
		if (length <= 0.0)
			return false;

		out.origin = worldA;
		out.axis = Vec3{axis.x / length, axis.y / length, axis.z / length};
		out.width = solid.depth;
		out.height = height;
		out.length = length;
		return true;
	}

	std::string memberMaterialName(const Model& model, const Entity& element)
	{
		// element を RelatedObjects に持つ IfcRelAssociatesMaterial を逆参照から辿る（同じ逆
		// 関係）。referrers は #id 昇順なので、複数あっても常に同じものを選ぶ（決定的）。
		for (const int relId : model.referrers(element.id))
		{
			const Entity* rel = model.entity(relId);
			if (rel == nullptr || rel->type != "IFCRELASSOCIATESMATERIAL")
				continue;
			const Value& related = rel->attribute(attr::kRelAssociatesRelatedObjects);
			if (!related.isList())
				continue;
			const bool associatesThis =
				std::ranges::any_of(related.items, [&element](const Value& ref)
									{ return ref.reference == element.id; });
			if (!associatesThis)
				continue;

			const Entity* material =
				model.resolve(rel->attribute(attr::kRelAssociatesMaterialRelatingMaterial));
			return materialNameOf(model, material);
		}
		return {};
	}

	MemberJoint memberEndJoint(const Vec2& point, const Vec2& outward, const Vec2& otherStart,
							   const Vec2& otherAxis, double otherLength, double otherHalfWidth)
	{
		MemberJoint joint;
		// 相手梁の断面幅方向（中心線に直交する単位ベクトル）。
		const Vec2 perpendicular{-otherAxis.y, otherAxis.x};
		const double a = (outward.x * perpendicular.x) + (outward.y * perpendicular.y);
		if (std::abs(a) < kParallelTol)
			return joint; // ほぼ平行 → 取り合いではなく継ぎ手

		const double dx = point.x - otherStart.x;
		const double dy = point.y - otherStart.y;
		const double d =
			(dx * perpendicular.x) + (dy * perpendicular.y); // 中心線からの符号付き距離
		if (std::abs(d) > otherHalfWidth + kFaceTol)
			return joint; // 端点が相手の幅の外（載っていない）
		const double t = (dx * otherAxis.x) + (dy * otherAxis.y); // 相手軸方向の位置
		if (t <= -kAlongTol || t >= otherLength + kAlongTol)
			return joint; // 相手の長さの範囲外

		joint.found = true;
		// 芯線まで（d を 0 にする移動量）と、そこから手前の面まで（半幅ぶん。斜交なら 1/cos）。
		joint.reach = d / a;
		joint.setback = otherHalfWidth / std::abs(a);
		// 相手の**途中**での取り合いか。出隅（L 字）では取り合いが相手の端部に来るので、
		// 端から半幅ぶんは「途中」とみなさない——直交する同寸の材が角で突き合う場合、
		// 一方の芯線は他方の面（＝端から半幅）まで伸びているのが普通で、そこを途中と数えると
		// 勝ち負けの付かない出隅を片側だけ動かしてしまう。
		const double cornerTol = otherHalfWidth + kAlongTol;
		joint.interior = t > cornerTol && t < otherLength - cornerTol;
		return joint;
	}

	double memberPenetrationDepth(const Vec2& point, const Vec2& outward, const Vec2& otherStart,
								  const Vec2& otherAxis, double otherLength, double otherHalfWidth)
	{
		const MemberJoint joint =
			memberEndJoint(point, outward, otherStart, otherAxis, otherLength, otherHalfWidth);
		if (!joint.found)
			return 0.0;
		// 端点が侵入してきた側（手前）の面まで引き戻す距離。
		const double s = joint.reach + joint.setback;
		return s > 0.0 ? s : 0.0;
	}

	std::vector<MemberCommand>
	resolveMemberInterferences(const std::vector<MemberCommand>& commands)
	{
		// 判定は入力時点のジオメトリ（スナップショット）に対して行うので、命令の並び順に
		// 依存しない決定的な結果になる。
		std::vector<MemberGeom> geoms;
		geoms.reserve(commands.size());
		for (const MemberCommand& command : commands)
			geoms.push_back(geomOf(command));

		std::vector<MemberCommand> result;
		result.reserve(commands.size());
		for (std::size_t i = 0; i < commands.size(); ++i)
		{
			MemberCommand command = commands[i];
			const MemberGeom& self = geoms[i];
			if (!self.valid)
			{
				result.push_back(std::move(command));
				continue;
			}

			const double selfHalfWidth = command.width / 2.0;
			std::vector<std::pair<MemberGeom, double>> others;
			for (std::size_t j = 0; j < commands.size(); ++j)
			{
				if (j == i || !geoms[j].valid)
					continue;
				const MemberCommand& other = commands[j];
				if (command.layer != other.layer)
					continue;
				if (!zOverlaps(command.elevation, command.height, other.elevation, other.height))
					continue;
				others.emplace_back(geoms[j], other.width / 2.0);
			}

			const Vec2 backward{-self.axis.x, -self.axis.y};
			const EndAdjust end = adjustForEnd(self.end, self.axis, self, selfHalfWidth, others);
			const EndAdjust start = adjustForEnd(self.start, backward, self, selfHalfWidth, others);
			// 実際に描かれる長さ（芯線間のパス長 − 両端の戻り）。従来「相手の面まで詰めた
			// 長さ」と同じ値で、これが残らないなら取り合いの解釈が破綻しているので触らない。
			const double drawn =
				self.length - start.reach - end.reach - start.setback - end.setback;
			if (drawn > kMinLength)
			{
				// 端点は相手の芯線上へ移す（平面座標のみ。高さバインドはそのまま）。材が実際に
				// 止まる位置は端部オフセットで戻す（core/Document.h「端部オフセット」）。
				command.start = Vec2{self.start.x + (self.axis.x * start.reach),
									 self.start.y + (self.axis.y * start.reach)};
				command.end = Vec2{self.end.x - (self.axis.x * end.reach),
								   self.end.y - (self.axis.y * end.reach)};
				command.startOffset = -start.setback;
				command.endOffset = -end.setback;
			}
			result.push_back(std::move(command));
		}
		return result;
	}

	std::vector<MemberCommand> buildMemberCommands(Context& context)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo>& stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();
		const auto topIndex = static_cast<int>(stories.size()) - 1;

		std::vector<MemberCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			// 最上階は横架材天端レイヤが無く軒高レイヤに配置する（beamTopLayerName が分岐を
			// 持つ）。layerElevation はレベルの絶対 Z（バインド offset の基準）。
			const std::string layerName = beamTopLayerName(i, story);
			const double layerElevation = beamTopElevation(story);

			for (const int elementId : context.storyElements(story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isMemberElement(*element))
					continue;

				MemberPlacement placement;
				if (!memberPlacement3D(model, *element, placement))
					continue;
				// 軸（押し出し方向）が鉛直な材は横架材でないためスキップ（火打等）。
				// **断面種別より先に判定して**火打を確実に除外する。
				if (std::hypot(placement.axis.x, placement.axis.y) <= kVerticalAxisTol)
					continue;

				double ox = placement.x;
				double oy = placement.y;
				double oz = placement.z;
				bool hasZ = placement.hasZ;
				Vec3 axis = placement.axis;
				double width = 0.0;
				double height = 0.0;
				double length = 0.0;
				bool verticalCut = false;

				MemberProfile profile;
				if (memberProfileDims(model, *element, profile))
				{
					width = profile.width;
					height = profile.height;
					length = profile.length;
				}
				else
				{
					// 矩形断面で拾えない材は、登り梁等の傾斜梁（任意断面＝平行四辺形の側面を
					// 厚み方向へ押し出したソリッド）として中心軸を導出する。平行四辺形として
					// 解釈できない材（筋かいの 6 頂点等）はスキップ。
					SlopedMemberGeometry sloped;
					if (!slopedMemberGeometry(model, *element, sloped))
						continue;
					ox = sloped.origin.x;
					oy = sloped.origin.y;
					oz = sloped.origin.z;
					hasZ = true;
					axis = sloped.axis;
					width = sloped.width;
					height = sloped.height;
					length = sloped.length;
					// 登り梁は端部が直切り（鉛直面）。下の高さ補正で矩形前提の軸直交持ち上げ
					// ではなく直切りの幾何（XY ずらし無し・鉛直持ち上げ）を使う。
					verticalCut = true;
				}

				const double horiz = std::hypot(axis.x, axis.y);
				if (horiz <= kVerticalAxisTol)
					continue;

				// 断面中心線の始端・終端（傾斜梁は軸の Z 成分で終端の高さが変わる。平面座標も
				// 軸の XY 成分 × 全長で求め、平面投影長を正しくする）。
				double x1 = ox - center.x;
				double y1 = oy - center.y;
				double x2 = x1 + (axis.x * length);
				double y2 = y1 + (axis.y * length);

				const double half = height / 2.0;
				double elevation = 0.0;
				double endElevation = 0.0;
				if (!hasZ)
				{
					// レイヤ基準高さ（横架材天端）は既に天端の高さなので補正不要。
					elevation = layerElevation;
					endElevation = layerElevation;
				}
				else if (verticalCut)
				{
					// 登り梁: 断面中心軸は鉛直な端面の中央高さを通るため、天端中央線の端点は
					// 端面中央の**直上**（XY は同じ）＝鉛直な端面の上端にあり、高さは
					// 断面中心 ＋ せい/(2·cosθ)（cosθ = horiz）。ヘッダ「登り梁の直切りの幾何」参照。
					elevation = story.elevation + oz + (half / horiz);
					endElevation = elevation + (axis.z * length);
				}
				else
				{
					// 軸に直交し軸を含む鉛直面内で上向きの単位ベクトル n の方向へ せい/2 だけ
					// 持ち上げて天端中央線にする（端部が軸直交切りの傾斜梁・水平梁）。
					const double nx = -axis.z * axis.x / horiz;
					const double ny = -axis.z * axis.y / horiz;
					const double nz = horiz;
					x1 += nx * half;
					y1 += ny * half;
					x2 += nx * half;
					y2 += ny * half;
					elevation = story.elevation + oz + (nz * half);
					endElevation = elevation + (axis.z * length);
				}

				// クラスは IFC 名の種別で判別する。判別できない部材は階・高さで推定する
				// （最上階は天端が軒高を超える材を母屋、軒高付近を小屋梁）。
				const bool aboveEaves =
					std::max(elevation, endElevation) > layerElevation + kSlopeTol;
				const std::string memberClass = resolveMemberClass(
					entityName(*element), static_cast<int>(i), topIndex, aboveEaves);

				// 母屋・棟木（小屋組の上端材）と登り梁は、梁（小屋梁・軒桁）と重なって
				// 見にくいため専用レイヤへ分け、高さ基準もそのレベルにバインドする。母屋・
				// 登り梁レベルは横架材天端（最上階は軒高）と同じ絶対 Z なので、layerElevation は
				// 変わらず offset の算出はそのまま。
				std::string elementLayer = layerName;
				std::string boundLevel = beamTopLevelType(story.isTop);
				if (memberClass == CLASS_MOYA || memberClass == CLASS_MUNAGI)
				{
					elementLayer = storyLayerName(i, story.isTop, kLevelMoya);
					boundLevel = kLevelMoya;
				}
				else if (memberClass == CLASS_NOBORIBARI)
				{
					elementLayer = storyLayerName(i, story.isTop, kLevelNoboribari);
					boundLevel = kLevelNoboribari;
				}

				MemberCommand cmd;
				cmd.layer = std::move(elementLayer);
				cmd.memberId = makeMemberId(width, height, memberMaterialName(model, *element));
				cmd.drawClass = memberClass;
				cmd.start = Vec2{x1, y1};
				cmd.end = Vec2{x2, y2};
				cmd.width = width;
				cmd.height = height;
				cmd.elevation = elevation;
				cmd.endElevation = endElevation;
				// offset はレベルの絶対 Z から天端 Z までの距離。平らな梁は ≈0、段差梁は
				// 一定値、傾斜梁は始端／終端で異なる値になる。
				cmd.startBound = StoryBoundCommand{0, boundLevel, elevation - layerElevation};
				cmd.endBound =
					StoryBoundCommand{0, std::move(boundLevel), endElevation - layerElevation};
				commands.push_back(std::move(cmd));
			}
		}

		// 横架材同士が食い込んでいる箇所は端部の長さを詰めて干渉を解消する。
		return resolveMemberInterferences(commands);
	}

	std::vector<MemberCommand> buildMemberCommands(const Model& model)
	{
		Context context(model);
		return buildMemberCommands(context);
	}
} // namespace HomeskzIfcImport::parse
