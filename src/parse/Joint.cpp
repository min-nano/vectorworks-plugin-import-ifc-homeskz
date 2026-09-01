//
//	parse/Joint.cpp
//
//	仕口解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core のみ依存）。
//

#include "parse/Joint.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::ColumnCommand;
	using core::MemberCommand;
	using core::SymbolCommand;
	using core::Vec2;

	namespace
	{
		// 2 つの Z 範囲が許容値（kJointZOverlapTol）を超えて重なるか。式は core/Document.h の
		// zRangesOverlap（横架材の食い込み・登り梁の受け材と共有）。
		bool zRangesOverlap(double aBottom, double aTop, double bBottom, double bTop)
		{
			return core::zRangesOverlap(aBottom, aTop, bBottom, bTop, kJointZOverlapTol);
		}

		// 判定する 1 端分（基準点・部材内側へ向かう方向・レイヤ平面からの相対 Z）。始端と
		// 終端で 3 つ組が変わるだけなので、同じループで両端を回すためにまとめる。
		struct JointEnd
		{
			Vec2 point;
			Vec2 inward;
			double zOffset = 0.0;
		};
	} // namespace

	MemberGeom memberGeom(const MemberCommand& command)
	{
		MemberGeom geom;
		const Vec2 delta = command.end - command.start;
		const double length = core::length(delta);
		if (length < kJointMinLength)
			return geom; // valid=false（端部・向きが定まらない退化した材）

		geom.valid = true;
		geom.start = command.start;
		geom.end = command.end;
		geom.axis = Vec2{delta.x / length, delta.y / length};
		geom.length = length;
		geom.halfWidth = command.width / 2.0;
		// 実体の Z 範囲。傾斜梁（elevation ≠ endElevation）も下端〜上端を覆う
		// （core/Document.h の memberTopZ / memberBottomZ）。
		geom.zTop = core::memberTopZ(command);
		geom.zBottom = core::memberBottomZ(command);
		return geom;
	}

	ColumnGeom columnGeom(const ColumnCommand& command)
	{
		ColumnGeom geom;
		geom.center = command.position;
		geom.halfWidth = command.width / 2.0;
		geom.halfDepth = command.depth / 2.0;
		geom.zBottom = command.elevation;
		geom.zTop = command.elevation + command.height;
		return geom;
	}

	bool pointInMember(const Vec2& point, const MemberGeom& other)
	{
		const Vec2 d = point - other.start;
		const double along = core::dot(d, other.axis);
		if (along < -kJointAlongTol || along > other.length + kJointAlongTol)
			return false;
		// 相手中心線からの直交距離（軸を 90 度回した向きへの射影）。
		const double perp = core::cross(other.axis, d);
		return std::abs(perp) <= other.halfWidth + kJointFaceTol;
	}

	bool pointInColumn(const Vec2& point, const ColumnGeom& column)
	{
		return std::abs(point.x - column.center.x) <= column.halfWidth + kJointFaceTol &&
			   std::abs(point.y - column.center.y) <= column.halfDepth + kJointFaceTol;
	}

	bool endHasReceiver(std::size_t index, const Vec2& point, const std::vector<MemberGeom>& geoms,
						const std::vector<MemberCommand>& members,
						const std::vector<ColumnGeom>& columnGeoms)
	{
		if (index >= geoms.size() || !geoms[index].valid)
			return false;
		const MemberGeom& self = geoms[index];
		const std::string& layer = members[index].layer;
		// 登り梁は別レイヤの軒桁・母屋・棟木に取り付くのでレイヤ一致の制約を外す。
		const bool crossLayer = members[index].drawClass == CLASS_NOBORIBARI;

		for (std::size_t j = 0; j < geoms.size(); ++j)
		{
			if (j == index || !geoms[j].valid)
				continue;
			if (!crossLayer && members[j].layer != layer)
				continue;
			const MemberGeom& other = geoms[j];
			// 平行（同一直線上の継ぎ手・側並び）は受ける材とみなさない。
			if (std::abs(core::cross(self.axis, other.axis)) < kJointParallelTol)
				continue;
			if (!zRangesOverlap(self.zBottom, self.zTop, other.zBottom, other.zTop))
				continue;
			if (pointInMember(point, other))
				return true;
		}

		// 柱の側面に取り付く端部も受ける材のある端部とする。
		return std::ranges::any_of(columnGeoms,
								   [&self, &point](const ColumnGeom& column)
								   {
									   return zRangesOverlap(self.zBottom, self.zTop,
															 column.zBottom, column.zTop) &&
											  pointInColumn(point, column);
								   });
	}

	std::vector<SymbolCommand> buildJointCommands(const std::vector<MemberCommand>& members,
												  const std::vector<ColumnCommand>& columns)
	{
		std::vector<MemberGeom> geoms;
		geoms.reserve(members.size());
		for (const MemberCommand& member : members)
			geoms.push_back(memberGeom(member));

		std::vector<ColumnGeom> columnGeoms;
		columnGeoms.reserve(columns.size());
		for (const ColumnCommand& column : columns)
			columnGeoms.push_back(columnGeom(column));

		std::vector<SymbolCommand> commands;
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const MemberGeom& geom = geoms[i];
			if (!geom.valid)
				continue;

			// 始端（内側方向は +軸・高さは startBound）・終端（内側方向は −軸・高さは
			// endBound）の順に判定する。高さはレイヤ平面からの相対 Z＝その端部のバウンド
			// offset（parse/Joint.h「高さも梁端の天端に合わせる」）。
			const std::array<JointEnd, 2> ends = {
				JointEnd{geom.start, geom.axis, members[i].startBound.offset},
				JointEnd{geom.end, Vec2{-geom.axis.x, -geom.axis.y}, members[i].endBound.offset},
			};
			for (const auto& [point, inward, zOffset] : ends)
			{
				if (!endHasReceiver(i, point, geoms, members, columnGeoms))
					continue;

				SymbolCommand command;
				command.layer = members[i].layer;
				command.symbol = kSymbolJoint;
				command.position = point;
				// 梁軸に沿って端部から部材内側へ向かう方向（度・反時計回り）。
				command.angle = std::atan2(inward.y, inward.x) * 180.0 / std::numbers::pi;
				command.zOffset = zOffset;
				commands.push_back(std::move(command));
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
