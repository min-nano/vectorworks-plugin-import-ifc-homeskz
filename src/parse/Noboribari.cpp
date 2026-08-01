//
//	parse/Noboribari.cpp
//
//	登り梁の位置補正の実装。Python 版 ifc/noboribari.py の correct_noboribari ほかに対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Noboribari.h"
#include "parse/Context.h"
#include "parse/Member.h"
#include "parse/Rafter.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::MemberCommand;
	using core::Vec2;

	double NoboribariRoofPlane::zAt(double x, double y) const
	{
		// 天端 Z の式は垂木・野地板と共有する（parse/IfcGeometry の RoofSlope::zAt）。
		// 平面のストーリ相対 Z に階の Elevation を足すと絶対 Z になる。
		return slope.zAt(x, y, storeyElevation);
	}

	bool NoboribariRoofPlane::contains(double x, double y) const
	{
		// 走査線法（Python 版 _RoofPlane.contains と同じ半開判定）。
		bool inside = false;
		const std::size_t count = plan.size();
		if (count < 3)
			return false;
		std::size_t j = count - 1;
		for (std::size_t i = 0; i < count; ++i)
		{
			const Vec2& a = plan[i];
			const Vec2& b = plan[j];
			if (((a.y > y) != (b.y > y)) && (x < (((b.x - a.x) * (y - a.y)) / (b.y - a.y)) + a.x))
				inside = !inside;
			j = i;
		}
		return inside;
	}

	std::vector<NoboribariRoofPlane> collectRoofPlanes(Context& context)
	{
		const Model& model = context.model();
		std::vector<NoboribariRoofPlane> planes;
		for (const StoryInfo& story : context.stories())
		{
			for (const int elementId : context.storyElements(story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isRoofSlab(*element))
					continue;

				// 屋根面は垂木・野地板と共有する（コンテキストが 1 度だけ解決する）。
				const RoofPlane* plane = context.roofPlane(elementId);
				if (plane == nullptr)
					continue;
				// 勾配方向が定まらない面（ほぼ水平）と平面式が発散する面（鉛直）は roofSlope が
				// 弾く。垂木・野地板とまったく同じ関門なので、拾う面が三者でズレない。
				RoofSlope slope;
				if (!roofSlope(*plane, slope))
					continue;
				planes.push_back(
					NoboribariRoofPlane{slope, RoofSlope::plan(*plane), story.elevation});
			}
		}
		return planes;
	}

	const NoboribariRoofPlane* roofPlaneFor(const MemberCommand& command,
											const std::vector<NoboribariRoofPlane>& planes,
											const Vec2& center)
	{
		const double dx = command.end.x - command.start.x;
		const double dy = command.end.y - command.start.y;
		const double length = std::hypot(dx, dy);
		if (length < kNoboribariMinLength)
			return nullptr;
		const Vec2 direction{dx / length, dy / length};

		// 命令座標はセンタリング済みなので、+center でワールドへ戻して内包判定する。
		const std::vector<Vec2> probes = {
			Vec2{((command.start.x + command.end.x) / 2.0) + center.x,
				 ((command.start.y + command.end.y) / 2.0) + center.y},
			Vec2{command.start.x + center.x, command.start.y + center.y},
			Vec2{command.end.x + center.x, command.end.y + center.y},
		};

		for (const NoboribariRoofPlane& plane : planes)
		{
			// 屋根面の勾配方向（RoofSlope::down は法線の水平成分の単位ベクトル）が登り梁の
			// 勾配方向と平行な面だけを、その登り梁の屋根面とみなす。
			const double dot =
				(plane.slope.down.x * direction.x) + (plane.slope.down.y * direction.y);
			if (std::abs(dot) < kNoboribariSlopeDirDot)
				continue;
			const bool covers = std::ranges::any_of(probes, [&plane](const Vec2& p)
													{ return plane.contains(p.x, p.y); });
			if (covers)
				return &plane;
		}
		return nullptr;
	}

	double noboribariEndTrim(const Vec2& point, const Vec2& outward, double zBottom, double zTop,
							 const std::vector<MemberCommand>& receivers)
	{
		double best = 0.0;
		for (const MemberCommand& receiver : receivers)
		{
			const double dx = receiver.end.x - receiver.start.x;
			const double dy = receiver.end.y - receiver.start.y;
			const double length = std::hypot(dx, dy);
			if (length < kNoboribariMinLength)
				continue; // 平面投影長が極小の受け材は無視する

			const double top = std::max(receiver.elevation, receiver.endElevation);
			const double bottom =
				std::min(receiver.elevation, receiver.endElevation) - receiver.height;
			if (std::min(zTop, top) - std::max(zBottom, bottom) <= kNoboribariZOverlapTol)
				continue; // Z 範囲が離れた材は取り合いでない

			best = std::max(best, memberPenetrationDepth(point, outward, receiver.start,
														 Vec2{dx / length, dy / length}, length,
														 receiver.width / 2.0));
		}
		return best;
	}

	MemberCommand correctOneNoboribari(const MemberCommand& command,
									   const std::vector<NoboribariRoofPlane>& planes,
									   const std::vector<MemberCommand>& receivers,
									   const Vec2& center)
	{
		const double dx = command.end.x - command.start.x;
		const double dy = command.end.y - command.start.y;
		const double length = std::hypot(dx, dy);
		if (length < kNoboribariMinLength)
			return command;
		const Vec2 axis{dx / length, dy / length};
		const Vec2 backward{-axis.x, -axis.y};

		const double zTop = std::max(command.elevation, command.endElevation);
		const double zBottom = std::min(command.elevation, command.endElevation) - command.height;

		// 1. 端部の食い込み解消: 始端は外向き −u、終端は外向き +u。詰めた後に極小長に
		//    ならない範囲で端点を軸に沿って内側へ引き戻す。
		double sStart = noboribariEndTrim(command.start, backward, zBottom, zTop, receivers);
		double sEnd = noboribariEndTrim(command.end, axis, zBottom, zTop, receivers);
		if (sStart < kNoboribariMinTrim)
			sStart = 0.0;
		if (sEnd < kNoboribariMinTrim)
			sEnd = 0.0;
		if (length - sStart - sEnd < kNoboribariMinLength)
		{
			sStart = 0.0;
			sEnd = 0.0;
		}

		MemberCommand updated = command;
		updated.start =
			Vec2{command.start.x + (axis.x * sStart), command.start.y + (axis.y * sStart)};
		updated.end = Vec2{command.end.x - (axis.x * sEnd), command.end.y - (axis.y * sEnd)};

		// 2. 屋根面スナップ: 天端中央線の両端（詰めた後の XY）を屋根面へ落として、勾配・高さを
		//    垂木下面に合わせる。屋根面が無ければ parse/Member の直切りの幾何のまま残す。
		const NoboribariRoofPlane* plane = roofPlaneFor(command, planes, center);
		if (plane != nullptr)
		{
			// バインド先レベル（登り梁レベル）の絶対 Z。offset を差し引いて逆算する。
			const double levelZ = command.elevation - command.startBound.offset;
			updated.elevation = plane->zAt(updated.start.x + center.x, updated.start.y + center.y);
			updated.endElevation = plane->zAt(updated.end.x + center.x, updated.end.y + center.y);
			updated.startBound.offset = updated.elevation - levelZ;
			updated.endBound.offset = updated.endElevation - levelZ;
		}
		return updated;
	}

	std::vector<MemberCommand> correctNoboribari(Context& context,
												 const std::vector<MemberCommand>& members)
	{
		// 登り梁が 1 本も無ければ屋根面の収集ごと省く（素通しと同じ結果）。
		const bool hasNoboribari = std::ranges::any_of(members, [](const MemberCommand& m)
													   { return m.drawClass == CLASS_NOBORIBARI; });
		if (!hasNoboribari)
			return members;

		const Vec2 center = context.gridCenter();
		const std::vector<NoboribariRoofPlane> planes = collectRoofPlanes(context);

		// 受ける材は「登り梁でない横架材」（登り梁同士は受け合わない）。
		std::vector<MemberCommand> receivers;
		for (const MemberCommand& member : members)
		{
			if (member.drawClass != CLASS_NOBORIBARI)
				receivers.push_back(member);
		}

		std::vector<MemberCommand> result;
		result.reserve(members.size());
		for (const MemberCommand& member : members)
		{
			if (member.drawClass != CLASS_NOBORIBARI)
				result.push_back(member);
			else
				result.push_back(correctOneNoboribari(member, planes, receivers, center));
		}
		return result;
	}

	std::vector<MemberCommand> correctNoboribari(const Model& model,
												 const std::vector<MemberCommand>& members)
	{
		Context context(model);
		return correctNoboribari(context, members);
	}
} // namespace HomeskzIfcImport::parse
