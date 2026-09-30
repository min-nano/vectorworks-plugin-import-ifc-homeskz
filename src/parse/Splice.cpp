//
//	parse/Splice.cpp
//
//	継手解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core のみ依存）。
//

#include "parse/Splice.h"
#include "core/ImportOptions.h"
#include "parse/Joint.h"

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
	using core::MemberCommand;
	using core::SymbolCommand;
	using core::Vec2;

	namespace
	{
		// 材の端 1 つ（材の端の点・部材内側へ向かう方向・レイヤ平面からの相対 Z）。
		struct SpliceEnd
		{
			Vec2 point;
			Vec2 inward;
			double zOffset = 0.0;
		};

		std::array<SpliceEnd, 2> endsOf(const MemberGeom& geom, const MemberCommand& member)
		{
			return {
				SpliceEnd{geom.start, geom.axis, member.startBound.offset},
				SpliceEnd{geom.end, Vec2{-geom.axis.x, -geom.axis.y}, member.endBound.offset},
			};
		}

		// 材軸の向き（度・反時計回り）を (−90, 90] に正規化する。継手は 2 本が共有するので
		// どちらの材から見るかで向きが 180 度変わる——並び順に依存させないために正規化する
		// （parse/Splice.h「回転角は材軸の向き」）。
		double normalizedAxisAngle(const Vec2& axis)
		{
			double angle = std::atan2(axis.y, axis.x) * 180.0 / std::numbers::pi;
			if (angle > 90.0)
				angle -= 180.0;
			else if (angle <= -90.0)
				angle += 180.0;
			return angle;
		}
	} // namespace

	std::vector<SymbolCommand> buildSpliceCommands(const std::vector<MemberCommand>& members,
												   const core::ImportOptions& options)
	{
		// 取り込まない役割は命令を 1 つも作らない（core/ImportOptions.h）。
		if (!options.isEnabled(core::SymbolRole::Splice))
			return {};

		const std::string& symbol = options.symbol(core::SymbolRole::Splice);

		// 材の端は仕口と同じ「材が実際に占める端」で見る（parse/Joint の memberGeom。
		// parse/Splice.h「ホームズ君 IFC での継手の現れ方」）。
		std::vector<MemberGeom> geoms;
		geoms.reserve(members.size());
		for (const MemberCommand& member : members)
			geoms.push_back(memberGeom(member));

		std::vector<SymbolCommand> commands;
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const MemberGeom& a = geoms[i];
			if (!a.valid)
				continue;
			for (std::size_t j = i + 1; j < members.size(); ++j)
			{
				const MemberGeom& b = geoms[j];
				if (!b.valid || members[j].layer != members[i].layer)
					continue;
				if (std::abs(core::cross(a.axis, b.axis)) >= kSpliceParallelTol)
					continue;
				if (!core::zRangesOverlap(a.zBottom, a.zTop, b.zBottom, b.zTop, kSpliceZOverlapTol))
					continue;

				for (const SpliceEnd& endA : endsOf(a, members[i]))
				{
					for (const SpliceEnd& endB : endsOf(b, members[j]))
					{
						if (core::length(endA.point - endB.point) > kSpliceEndTol)
							continue;
						// 互いに反対側へ伸びる（突き付く）組だけ。同じ側へ伸びるなら 2 本は
						// 重なっていて、継手ではない。
						if (core::dot(endA.inward, endB.inward) >= 0.0)
							continue;

						SymbolCommand command;
						command.layer = members[i].layer;
						command.symbol = symbol;
						command.position = Vec2{(endA.point.x + endB.point.x) / 2.0,
												(endA.point.y + endB.point.y) / 2.0};
						command.angle = normalizedAxisAngle(a.axis);
						command.zOffset = std::max(endA.zOffset, endB.zOffset);
						commands.push_back(std::move(command));
					}
				}
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
