//
//	parse/Splice.cpp
//
//	継手解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core のみ依存）。
//

#include "parse/Splice.h"
#include "core/ImportOptions.h"
#include "parse/Joint.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
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

		// 継手の片側 1 本（その材の命令とジオメトリ、継手にある端）。一時的に借りるだけなので
		// ポインタで持つ（参照メンバは clang-tidy の cppcoreguidelines が禁じる）。
		struct SplicePiece
		{
			const MemberCommand* member = nullptr;
			const MemberGeom* geom = nullptr;
			const SpliceEnd* end = nullptr;
		};

		double angleOf(const Vec2& direction)
		{
			return std::atan2(direction.y, direction.x) * 180.0 / std::numbers::pi;
		}

		// 材軸の向き（度・反時計回り）を (−90, 90] に正規化する。向きの手掛かりが無いときの
		// 既定で、どちらの材から見るかで 180 度変わるのを並び順に依存させないため
		// （parse/Splice.h「向き」）。
		double normalizedAxisAngle(const Vec2& axis)
		{
			double angle = angleOf(axis);
			if (angle > 90.0)
				angle -= 180.0;
			else if (angle <= -90.0)
				angle += 180.0;
			return angle;
		}

		// 点 p がその材の上（継手から材の内側へ、材の長さの範囲・幅の範囲）にあれば、継手からの
		// 軸方向の距離を返す。継手の真上（kSpliceAlongTol 以内）はどちらの側でもない。
		std::optional<double> distanceOnPiece(const SplicePiece& piece, const Vec2& p)
		{
			const Vec2 d = p - piece.end->point;
			const double along = core::dot(d, piece.end->inward);
			if (along <= kSpliceAlongTol || along > piece.geom->length + kSpliceSideTol)
				return std::nullopt;
			if (std::abs(core::cross(piece.end->inward, d)) >
				piece.geom->halfWidth + kSpliceSideTol)
				return std::nullopt;
			return along;
		}

		// その材の上にある点のうち、継手にいちばん近いものまでの距離（無ければ nullopt）。
		std::optional<double> nearestOnPiece(const SplicePiece& piece,
											 const std::vector<Vec2>& points)
		{
			std::optional<double> best;
			for (const Vec2& p : points)
			{
				const std::optional<double> d = distanceOnPiece(piece, p);
				if (d.has_value() && (!best.has_value() || *d < *best))
					best = d;
			}
			return best;
		}

		// その材を下から受ける柱（上端が材の下端に止まる柱・小屋束）の位置。通し柱のように
		// 材を越えて伸びる柱や、材の上に立つ上階の柱は支点ではない。
		std::vector<Vec2> supportsUnder(const SplicePiece& piece,
										const std::vector<ColumnCommand>& columns)
		{
			std::vector<Vec2> points;
			for (const ColumnCommand& column : columns)
			{
				const double top = core::columnDrawnTop(column);
				if (std::abs(top - piece.geom->zBottom) > kSpliceSupportZTol)
					continue;
				points.push_back(column.position);
			}
			return points;
		}

		// 2 つの距離のうち近い方の側（0 = a、1 = b）。片側にしか無ければその側、どちらにも
		// 無い・同じ距離なら決めない。
		std::optional<int> nearerSide(std::optional<double> a, std::optional<double> b)
		{
			if (a.has_value() && b.has_value())
			{
				if (std::abs(*a - *b) <= kSpliceAlongTol)
					return std::nullopt;
				return *a < *b ? 0 : 1;
			}
			if (a.has_value())
				return 0;
			if (b.has_value())
				return 1;
			return std::nullopt;
		}

		// 女木の側（0 = a、1 = b）。決まらなければ nullopt（parse/Splice.h「向き」）。
		std::optional<int> femaleSide(const SplicePiece& a, const SplicePiece& b,
									  const SpliceCues& cues)
		{
			// 土台: M12 のアンカーボルトが近い側が男木 → 女木はその反対。
			if (a.member->drawClass == CLASS_DODAI || b.member->drawClass == CLASS_DODAI)
			{
				const std::optional<int> male = nearerSide(nearestOnPiece(a, cues.anchorsM12),
														   nearestOnPiece(b, cues.anchorsM12));
				if (!male.has_value())
					return std::nullopt;
				return 1 - *male;
			}

			// 梁・大引・母屋など: 支点（下階の柱・小屋束。大引は床束も）が近い側が女木。
			const auto nearestSupport = [&cues](const SplicePiece& piece)
			{
				std::vector<Vec2> supports = supportsUnder(piece, cues.columns);
				if (piece.member->drawClass == CLASS_OOBIKI)
					supports.insert(supports.end(), cues.floorPosts.begin(), cues.floorPosts.end());
				return nearestOnPiece(piece, supports);
			};
			return nearerSide(nearestSupport(a), nearestSupport(b));
		}
	} // namespace

	std::vector<SymbolCommand> buildSpliceCommands(const std::vector<MemberCommand>& members,
												   const SpliceCues& cues,
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

						const SplicePiece pieceA{&members[i], &a, &endA};
						const SplicePiece pieceB{&members[j], &b, &endB};

						SymbolCommand command;
						command.layer = members[i].layer;
						command.symbol = symbol;
						command.position = Vec2{(endA.point.x + endB.point.x) / 2.0,
												(endA.point.y + endB.point.y) / 2.0};
						// シンボルの +X を女木の側（その材の内側）へ向ける。
						const std::optional<int> female = femaleSide(pieceA, pieceB, cues);
						if (female.has_value())
							command.angle = angleOf(*female == 0 ? endA.inward : endB.inward);
						else
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
