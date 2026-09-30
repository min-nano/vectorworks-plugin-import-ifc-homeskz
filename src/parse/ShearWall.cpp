//
//	parse/ShearWall.cpp
//
//	耐力壁解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。意図と規約は parse/ShearWall.h を参照。
//

#include "parse/ShearWall.h"
#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
#include "parse/IfcGeometry.h"
#include "parse/StructuralClass.h"
#include "parse/Story.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::ShearWallCommand;
	using core::Vec2;
	using core::Vec3;

	namespace
	{
		// 文字列が接頭辞で始まるか。
		bool startsWith(const std::string& text, const char* prefix)
		{
			const std::string head(prefix);
			return text.size() >= head.size() && text.compare(0, head.size(), head) == 0;
		}

		// 平面ベクトルの内積。
		double dot2(const Vec2& a, const Vec2& b)
		{
			return (a.x * b.x) + (a.y * b.y);
		}

		// 平面の点 (s, n) を軸・法線から復元する（axis ⊥ normal・どちらも単位ベクトル）。
		Vec2 planPoint(const Vec2& axis, const Vec2& normal, double s, double n)
		{
			return (axis * s) + (normal * n);
		}

		// 壁面座標（s＝軸方向, z＝高さ）の点。
		struct FacePoint
		{
			double s = 0.0;
			double z = 0.0;
		};

		// 壁面内の見付け幅（筋かいの軸に直交する広がり）を返す。外形が面にならない・
		// 縮退しているときは 0。
		//
		// 押し出しが壁面の法線方向なので、(s, z) へ落とした外形は IFC のプロファイルを
		// 回転しただけの形になる。筋かいは「長い帯」なので、**外形を回転キャリパで測った
		// 最小の幅**がそのまま見付け幅（45×90 なら 90）になる——凸多角形の最小幅は
		// どれかの辺に直交する向きで実現されるので、辺ごとに直交方向の広がりを測って
		// 最小を採ればよい。
		//
		// **「最も離れた 2 点」を軸と見なしてはいけない。** 実データの筋かいは端が
		// 尖った六角形なので偶然それでも合うが、単純な矩形断面では最長が対角線になり、
		// 幅が 2 倍近くに化ける（軸が対角線へ傾くため）。
		double faceWidth(const std::vector<FacePoint>& face)
		{
			if (face.size() < 3)
				return 0.0;

			double best = std::numeric_limits<double>::max();
			const std::size_t count = face.size();
			for (std::size_t i = 0; i < count; ++i)
			{
				const FacePoint& from = face[i];
				const FacePoint& to = face[(i + 1) % count];
				const Vec2 edge{to.s - from.s, to.z - from.z};
				const double len = std::hypot(edge.x, edge.y);
				if (len < core::kGeomEps)
					continue; // 重複頂点の辺は向きが定まらない

				const Vec2 dir = edge * (1.0 / len);
				double low = std::numeric_limits<double>::max();
				double high = std::numeric_limits<double>::lowest();
				for (const FacePoint& point : face)
				{
					// dir を +90 度回した向きへの射影。
					const double perp = (-point.s * dir.y) + (point.z * dir.x);
					low = std::min(low, perp);
					high = std::max(high, perp);
				}
				best = std::min(best, high - low);
			}
			return best == std::numeric_limits<double>::max() ? 0.0 : best;
		}

		// 解析中の耐力壁 1 枚（同じ耐力壁になる要素をまとめたもの）。
		struct Group
		{
			core::ShearWallKind kind = core::ShearWallKind::Brace;
			std::string name;		  // 筋かいのまとめ鍵（同名＝たすき掛け）
			bool doubleBrace = false; // Name が "筋かいダブル" 始まりだったか
			std::vector<ShearWallPiece> pieces;
		};

		// グループ全体の広がり（軸方向・高さ・材厚・見付け幅）。
		struct GroupExtent
		{
			double sMin = 0.0;
			double sMax = 0.0;
			double zBottom = 0.0;
			double zTop = 0.0;
			double thickness = 0.0;
			double width = 0.0;
		};

		GroupExtent groupExtent(const Group& group)
		{
			GroupExtent extent;
			extent.sMin = std::numeric_limits<double>::max();
			extent.sMax = std::numeric_limits<double>::lowest();
			extent.zBottom = std::numeric_limits<double>::max();
			extent.zTop = std::numeric_limits<double>::lowest();
			for (const ShearWallPiece& piece : group.pieces)
			{
				extent.sMin = std::min(extent.sMin, piece.sMin);
				extent.sMax = std::max(extent.sMax, piece.sMax);
				extent.zBottom = std::min(extent.zBottom, piece.zBottom);
				extent.zTop = std::max(extent.zTop, piece.zTop);
				extent.thickness = std::max(extent.thickness, piece.thickness);
				extent.width = std::max(extent.width, piece.width);
			}
			return extent;
		}

		// span 柱レイヤの base ストーリ（0 起点）が index の柱だけを集める。
		std::vector<const core::ColumnCommand*>
		columnsOfStory(const std::vector<core::ColumnCommand>& columns, std::size_t index)
		{
			std::vector<const core::ColumnCommand*> found;
			for (const core::ColumnCommand& column : columns)
			{
				double from = 0.0;
				double to = 0.0;
				if (!parseSpanLayer(column.layer, from, to))
					continue;
				if (std::llround(from) == static_cast<long long>(index) + 1)
					found.push_back(&column);
			}
			return found;
		}

		// 点に最も近い柱を返す（許容内に無ければ nullptr）。同距離なら**先に現れた柱**を
		// 採るので、列挙順に依存しない決定的な結果になる（columns の並びが決定的なため）。
		const core::ColumnCommand*
		nearestColumn(const std::vector<const core::ColumnCommand*>& columns, const Vec2& point)
		{
			const core::ColumnCommand* best = nullptr;
			double bestDistance = kShearWallColumnTol;
			for (const core::ColumnCommand* column : columns)
			{
				const double distance =
					std::hypot(column->position.x - point.x, column->position.y - point.y);
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = column;
				}
			}
			return best;
		}

		// 柱レイヤ名を ";" で連ねる（PIO の TargetLayers パラメータ）。
		std::string joinLayers(const std::vector<std::string>& layers)
		{
			std::string joined;
			for (const std::string& layer : layers)
			{
				if (!joined.empty())
					joined += ";";
				joined += layer;
			}
			return joined;
		}

		// 面材の面（表／裏／両面）と、軸からの距離を決める。line は軸の線が法線方向の
		// どこにあるか（＝柱芯を通る線の位置）。
		void resolvePanelSide(const Group& group, double line, ShearWallCommand& command)
		{
			bool front = false;
			bool back = false;
			double sum = 0.0;
			for (const ShearWallPiece& piece : group.pieces)
			{
				const double side = piece.offset - line;
				if (side > kShearWallMergeTol)
					front = true;
				else if (side < -kShearWallMergeTol)
					back = true;
				sum += std::abs(side);
			}
			command.panelOffset = sum / static_cast<double>(group.pieces.size());
			if (front && back)
				command.panelSide = core::ShearWallPanelSide::Both;
			else if (back)
				command.panelSide = core::ShearWallPanelSide::Back;
			else
				command.panelSide = core::ShearWallPanelSide::Front;
		}

		// 階 1 つぶんの耐力壁要素を、同じ耐力壁になるものごとにまとめて集める。
		// 並びは要素の出現順（＝storyElements の並び）で決定的。
		std::vector<Group> collectGroups(const Model& model, const std::vector<int>& elements)
		{
			std::vector<Group> groups;
			for (const int elementId : elements)
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr)
					continue;
				const bool brace = isShearBrace(*element);
				if (!brace && !isShearPanel(*element))
					continue;

				ShearWallPiece piece;
				if (!resolveShearWallPiece(model, *element, brace, piece))
					continue; // ソリッドを解決できない・水平押し出しでない要素はスキップ

				Group* found = nullptr;
				if (brace)
				{
					// たすき掛けは**同じ Name の 2 要素**として出るので、Name でまとめる。
					const std::string name = entityName(*element);
					for (Group& group : groups)
					{
						if (group.kind == core::ShearWallKind::Brace && group.name == name)
						{
							found = &group;
							break;
						}
					}
					if (found == nullptr)
					{
						Group group;
						group.kind = core::ShearWallKind::Brace;
						group.name = name;
						group.doubleBrace = isDoubleBrace(*element);
						groups.push_back(std::move(group));
						found = &groups.back();
					}
				}
				else
				{
					// 大壁の表裏は「同じ軸・同じ区間」の面材 2 枚として出るので、それを
					// まとめる（法線の正負で表裏に分かれる）。
					for (Group& group : groups)
					{
						if (group.kind != core::ShearWallKind::Panel || group.pieces.empty())
							continue;
						const ShearWallPiece& first = group.pieces.front();
						if (std::abs(dot2(first.axis, piece.axis) - 1.0) > core::kPointEps)
							continue;
						if (std::abs(first.sMin - piece.sMin) > kShearWallMergeTol ||
							std::abs(first.sMax - piece.sMax) > kShearWallMergeTol)
							continue;
						// **法線方向の近さも要る。** 同じ通りに並ぶ 2 枚の壁は、軸も
						// 軸方向の区間も一致しうる（間口の同じ部屋が並ぶだけで起こる）。
						// 表裏はせいぜい柱幅＋板厚しか離れないので、その範囲だけをまとめる。
						if (std::abs(first.offset - piece.offset) > kShearWallPairOffsetTol)
							continue;
						found = &group;
						break;
					}
					if (found == nullptr)
					{
						Group group;
						group.kind = core::ShearWallKind::Panel;
						groups.push_back(std::move(group));
						found = &groups.back();
					}
				}
				found->pieces.push_back(piece);
			}
			return groups;
		}

		// 耐力壁の軸に載る横架材 1 本を、軸の座標（始点からの距離 s）で表したもの。
		struct AxisBeam
		{
			const core::MemberCommand* member = nullptr;
			double from = 0.0; // 材の実際の範囲（端部オフセット込み）の s
			double to = 0.0;   //
		};

		// 軸（start から axis の向き）に載る横架材を集める。軸と平行で、軸が材の芯線上
		// （半幅の内）にあるものだけ。
		std::vector<AxisBeam> beamsOnAxis(const std::vector<core::MemberCommand>& members,
										  const Vec2& start, const Vec2& axis)
		{
			std::vector<AxisBeam> found;
			for (const core::MemberCommand& member : members)
			{
				const Vec2 run = member.end - member.start;
				const double length = std::hypot(run.x, run.y);
				if (length < core::kPointEps)
					continue;
				const Vec2 dir = run * (1.0 / length);
				if (std::abs((axis.x * dir.y) - (axis.y * dir.x)) > kShearWallBeamParallelTol)
					continue;
				const Vec2 rel = start - member.start;
				if (std::abs((rel.x * dir.y) - (rel.y * dir.x)) >
					(member.width / 2.0) + kShearWallBeamLateralTol)
					continue;

				// 実際の範囲（負のオフセット＝短く）を軸の s へ写す。
				const double sStart = dot2(member.start - start, axis);
				const double sign = dot2(dir, axis) >= 0.0 ? 1.0 : -1.0;
				const double a = sStart + (sign * -member.startOffset);
				const double b = sStart + (sign * (length + member.endOffset));
				found.push_back(AxisBeam{&member, std::min(a, b), std::max(a, b)});
			}
			return found;
		}

		// 軸上の 1 点で見つけた上下の横架材の高さ（絶対 Z）。見つからない側は空。
		struct BeamBounds
		{
			std::optional<double> upper; // 上の材の下端
			std::optional<double> lower; // 下の材の天端
		};

		// 軸上の点 s の真上・真下の横架材を探す。mid より上に下端がある材から最も低い下端を、
		// mid より下に天端がある材から最も高い天端を採る。探すのは IFC の要素自身の上端
		// ifcTop・下端 ifcBottom から kShearWallBeamSearch 以内。
		BeamBounds beamBoundsAt(const std::vector<AxisBeam>& beams, const Vec2& start,
								const Vec2& axis, double s, double mid, double ifcBottom,
								double ifcTop)
		{
			const Vec2 point = start + (axis * s);
			BeamBounds found;
			for (const AxisBeam& beam : beams)
			{
				if (s < beam.from - kShearWallBeamCoverTol || s > beam.to + kShearWallBeamCoverTol)
					continue;
				const core::MemberCommand& member = *beam.member;
				const Vec2 run = member.end - member.start;
				const double length = std::hypot(run.x, run.y);
				const double along = dot2(point - member.start, run * (1.0 / length));

				// その点での天端と下端。傾斜梁の断面は材軸に直交するので、鉛直に測った
				// せいは せい/cosθ になる（parse/Member「登り梁の直切りの幾何」と同じ）。
				const double rise = member.endElevation - member.elevation;
				const double top = member.elevation + (rise * along / length);
				const double bottom = top - (member.height * std::hypot(length, rise) / length);

				if (bottom >= mid && std::abs(bottom - ifcTop) <= kShearWallBeamSearch)
				{
					if (!found.upper.has_value() || bottom < *found.upper)
						found.upper = bottom;
				}
				else if (top <= mid && std::abs(top - ifcBottom) <= kShearWallBeamSearch)
				{
					if (!found.lower.has_value() || top > *found.lower)
						found.lower = top;
				}
			}
			return found;
		}

		// 測る点（軸の s）を並べる。内法の両端と、内法に入る**材の端の両側**（上下の材が
		// 入れ替わりうる点）、さらに隣り合う点の中点。上下の材の高さは材の端のあいだでは
		// 直線なので、これで段差を取りこぼさない。
		std::vector<double> samplePoints(const std::vector<AxisBeam>& beams, double clearStart,
										 double clearEnd)
		{
			std::vector<double> points{clearStart, clearEnd};
			for (const AxisBeam& beam : beams)
			{
				for (const double edge : {beam.from, beam.to})
				{
					for (const double s :
						 {edge - kShearWallBeamBreakGap, edge + kShearWallBeamBreakGap})
					{
						if (s > clearStart && s < clearEnd)
							points.push_back(s);
					}
				}
			}
			std::ranges::sort(points);
			const auto [first, last] = std::ranges::unique(
				points, [](double a, double b) { return std::abs(a - b) < core::kPointEps; });
			points.erase(first, last);

			std::vector<double> withMids;
			withMids.reserve((points.size() * 2) - 1);
			for (std::size_t i = 0; i < points.size(); ++i)
			{
				if (i > 0)
					withMids.push_back((points[i - 1] + points[i]) / 2.0);
				withMids.push_back(points[i]);
			}
			return withMids;
		}

		// 柱芯 point に立つ柱の半幅（その階の span 柱から探す。柱が無ければ 0＝軸の端が
		// そのまま内法の端）。buildShearWallCommands が柱芯を start / end に入れているので、
		// 同じ点の柱がそのまま見つかる。
		double halfColumnWidthAt(const std::vector<const core::ColumnCommand*>& columns,
								 const Vec2& point)
		{
			for (const core::ColumnCommand* column : columns)
			{
				if (core::samePoint(column->position, point))
					return column->width / 2.0;
			}
			return 0.0;
		}

		// 耐力壁 1 枚の高さを上下の横架材に合わせる（fitShearWallsToMembers の本体）。
		// layerZ は配置先レイヤ平面の絶対 Z。測れない・潰れるときは wall を変えない。
		void fitShearWall(ShearWallCommand& wall, double layerZ,
						  const std::vector<core::MemberCommand>& members,
						  const std::vector<const core::ColumnCommand*>& columns)
		{
			const Vec2 run = wall.end - wall.start;
			const double length = std::hypot(run.x, run.y);
			if (length < core::kPointEps)
				return;
			const Vec2 axis = run * (1.0 / length);

			// 内法の両端（軸の始点からの距離）。
			const double clearStart = halfColumnWidthAt(columns, wall.start);
			const double clearEnd = length - halfColumnWidthAt(columns, wall.end);
			if (clearEnd - clearStart < core::kPointEps)
				return;

			const double ifcBottom = layerZ + wall.bottomHeight;
			const double ifcTop = layerZ + std::max(wall.topHeight, wall.topHeightEnd);
			const double mid = (ifcBottom + ifcTop) / 2.0;

			const std::vector<AxisBeam> beams = beamsOnAxis(members, wall.start, axis);
			const std::vector<double> points = samplePoints(beams, clearStart, clearEnd);
			std::vector<BeamBounds> samples;
			samples.reserve(points.size());
			for (const double s : points)
				samples.push_back(beamBoundsAt(beams, wall.start, axis, s, mid, ifcBottom, ifcTop));

			// 上端。両端とも取れて、両端を結ぶ直線がどの点でも上の材の下端に載るなら
			// その直線（登り梁・水平の梁）。載らない（段差梁）・片端しか取れないなら、
			// 取れた点のうち最も低い値で水平にそろえる。
			double topAtStart = layerZ + wall.topHeight;
			double topAtEnd = layerZ + wall.topHeightEnd;
			const std::optional<double> upperStart = samples.front().upper;
			const std::optional<double> upperEnd = samples.back().upper;
			bool straight = upperStart.has_value() && upperEnd.has_value();
			std::optional<double> lowestUpper;
			for (std::size_t k = 0; k < samples.size(); ++k)
			{
				const std::optional<double>& upper = samples[k].upper;
				if (!upper.has_value())
					continue;
				if (!lowestUpper.has_value() || *upper < *lowestUpper)
					lowestUpper = upper;
				if (straight)
				{
					const double ratio = (points[k] - clearStart) / (clearEnd - clearStart);
					const double line = *upperStart + ((*upperEnd - *upperStart) * ratio);
					if (std::abs(line - *upper) > kShearWallStepTol)
						straight = false;
				}
			}
			if (straight)
			{
				topAtStart = *upperStart;
				topAtEnd = *upperEnd;
			}
			else if (lowestUpper.has_value())
			{
				topAtStart = *lowestUpper;
				topAtEnd = *lowestUpper;
			}

			// 下端。PIO は下端を 1 つしか持たないので、取れた点のうち最も高い天端
			// （軸組の外へ出さない側）。
			double bottom = ifcBottom;
			std::optional<double> highestLower;
			for (const BeamBounds& sample : samples)
			{
				if (sample.lower.has_value() &&
					(!highestLower.has_value() || *sample.lower > *highestLower))
					highestLower = sample.lower;
			}
			if (highestLower.has_value())
				bottom = *highestLower;

			// 測り直した内法が潰れるなら（上下の材の取り違え）、IFC の高さのまま残す。
			if (topAtStart <= bottom || topAtEnd <= bottom)
				return;
			wall.bottomHeight = bottom - layerZ;
			wall.topHeight = topAtStart - layerZ;
			wall.topHeightEnd = topAtEnd - layerZ;
		}
	} // namespace

	bool isShearBrace(const Entity& element)
	{
		return element.type == "IFCMEMBER" && startsWith(entityName(element), kBracePrefix);
	}

	bool isDoubleBrace(const Entity& element)
	{
		return element.type == "IFCMEMBER" && startsWith(entityName(element), kDoubleBracePrefix);
	}

	bool isShearPanel(const Entity& element)
	{
		return element.type == "IFCWALL" && startsWith(entityName(element), kPanelPrefix);
	}

	bool resolveShearWallPiece(const Model& model, const Entity& element, bool brace,
							   ShearWallPiece& out)
	{
		WorldSolid solid;
		if (!resolveElementWorldSolid(model, &element, solid))
			return false;

		// 押し出しは壁面に直交する＝水平でなければならない（鉛直押し出しの火打等は
		// 耐力壁として解釈できない）。
		const Vec3 extrude = solid.extrudeDir;
		const double planLength = std::hypot(extrude.x, extrude.y);
		if (std::abs(extrude.z) > kShearWallHorizontalTol || planLength < core::kGeomEps)
			return false;

		const std::vector<Vec3> base = solid.base();
		const std::vector<Vec3> top = solid.top();
		if (base.size() < 3 || top.size() != base.size())
			return false;

		ShearWallPiece piece;
		// 法線＝押し出し方向、軸＝それを −90 度回した向き。**normal は axis を +90 度
		// 回した向き**という関係を保つ（表／裏の左右がこの関係で決まる）。軸の向きは
		// (x, y) の辞書順で正へ揃え、反転したら法線も一緒に返す。
		piece.normal = Vec2{extrude.x / planLength, extrude.y / planLength};
		piece.axis = Vec2{piece.normal.y, -piece.normal.x};
		if (piece.axis.x < -core::kGeomEps ||
			(std::abs(piece.axis.x) <= core::kGeomEps && piece.axis.y < 0.0))
		{
			piece.axis = piece.axis * -1.0;
			piece.normal = piece.normal * -1.0;
		}

		double sMin = std::numeric_limits<double>::max();
		double sMax = std::numeric_limits<double>::lowest();
		double nMin = std::numeric_limits<double>::max();
		double nMax = std::numeric_limits<double>::lowest();
		double zMin = std::numeric_limits<double>::max();
		double zMax = std::numeric_limits<double>::lowest();
		std::vector<FacePoint> face;
		face.reserve(base.size());
		for (std::size_t loop = 0; loop < 2; ++loop)
		{
			const std::vector<Vec3>& points = (loop == 0) ? base : top;
			for (const Vec3& point : points)
			{
				const Vec2 plan{point.x, point.y};
				const double s = dot2(plan, piece.axis);
				const double n = dot2(plan, piece.normal);
				sMin = std::min(sMin, s);
				sMax = std::max(sMax, s);
				nMin = std::min(nMin, n);
				nMax = std::max(nMax, n);
				zMin = std::min(zMin, point.z);
				zMax = std::max(zMax, point.z);
				if (loop == 0)
					face.push_back(FacePoint{s, point.z});
			}
		}
		if (sMax - sMin < core::kPointEps || zMax - zMin < core::kPointEps ||
			nMax - nMin < core::kPointEps)
			return false;

		piece.sMin = sMin;
		piece.sMax = sMax;
		piece.offset = (nMin + nMax) / 2.0;
		piece.zBottom = zMin;
		piece.zTop = zMax;
		piece.thickness = nMax - nMin;

		if (brace)
		{
			piece.width = faceWidth(face);
			if (piece.width < core::kPointEps)
				return false;

			// 傾きの向き: 断面外形の**最も高い点**が軸方向のどちら寄りにあるか。
			double topS = face.front().s;
			double topZ = face.front().z;
			for (const FacePoint& point : face)
			{
				if (point.z > topZ)
				{
					topZ = point.z;
					topS = point.s;
				}
			}
			piece.risesToMax = (sMax - topS) < (topS - sMin);
		}

		out = piece;
		return true;
	}

	std::vector<ShearWallCommand>
	buildShearWallCommands(Context& context, const std::vector<core::ColumnCommand>& columns)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo> stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();
		const std::map<int, std::vector<std::string>> columnLayers =
			collectColumnLayersByStory(columns);

		std::vector<ShearWallCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerName(i, story.isTop, kLevelShearWall);
			// レイヤ平面（ストーリ相対）＝その階の横架材天端。最上階は軒高＝0。
			const double layerZ = story.isTop ? 0.0 : story.beamOffset;
			const auto layerList = columnLayers.find(static_cast<int>(i));
			const std::string targets =
				layerList == columnLayers.end() ? std::string() : joinLayers(layerList->second);
			const std::vector<const core::ColumnCommand*> storyColumns = columnsOfStory(columns, i);

			for (const Group& group : collectGroups(model, context.storyElements(story.id)))
			{
				const GroupExtent extent = groupExtent(group);
				const ShearWallPiece& first = group.pieces.front();

				// 要素自身の端（センタリング済み）で柱を探す。面材は壁芯から板厚ぶん
				// 外れているが、探す許容（kShearWallColumnTol）に対しては誤差の範囲。
				const core::ColumnCommand* startColumn = nearestColumn(
					storyColumns,
					planPoint(first.axis, first.normal, extent.sMin, first.offset) - center);
				const core::ColumnCommand* endColumn = nearestColumn(
					storyColumns,
					planPoint(first.axis, first.normal, extent.sMax, first.offset) - center);

				// 軸の線が法線方向のどこにあるか。柱が見つかればその柱芯を通り、
				// 見つからなければ要素自身（面材が複数枚あればその中点）を線とみなす。
				// **柱の無い端もこの線へ載せる**——面材自身の端をそのまま使うと、
				// 片端だけ板厚ぶん外れた斜めの軸になってしまう。
				double line = first.offset;
				if (startColumn != nullptr)
					line = dot2(startColumn->position + center, first.normal);
				else if (endColumn != nullptr)
					line = dot2(endColumn->position + center, first.normal);
				else if (group.pieces.size() > 1)
				{
					line = 0.0;
					for (const ShearWallPiece& piece : group.pieces)
						line += piece.offset;
					line /= static_cast<double>(group.pieces.size());
				}

				const Vec2 startPoint =
					startColumn != nullptr
						? startColumn->position
						: planPoint(first.axis, first.normal, extent.sMin, line) - center;
				const Vec2 endPoint =
					endColumn != nullptr
						? endColumn->position
						: planPoint(first.axis, first.normal, extent.sMax, line) - center;
				if (core::samePoint(startPoint, endPoint))
					continue; // 両端が同じ柱に寄った（＝軸が決まらない）

				// 内法は柱芯間から両側の半柱幅を引いたもの。柱が見つからなければ要素自身の
				// 広がりで代用する（PIO は図面の柱から引き直すので、これは控え）。
				double clear = std::hypot(endPoint.x - startPoint.x, endPoint.y - startPoint.y);
				if (startColumn != nullptr)
					clear -= startColumn->width / 2.0;
				if (endColumn != nullptr)
					clear -= endColumn->width / 2.0;
				if (clear <= 0.0)
					clear = extent.sMax - extent.sMin;
				if (clear <= 0.0)
					continue;

				ShearWallCommand command;
				command.layer = layer;
				command.targetLayers = targets;
				command.start = startPoint;
				command.end = endPoint;
				command.kind = group.kind;
				command.thickness = extent.thickness;
				command.clearSpan = clear;
				command.bottomHeight = extent.zBottom - layerZ;
				command.topHeight = extent.zTop - layerZ;
				command.topHeightEnd = command.topHeight; // 上下の横架材で測り直す（後処理）

				if (group.kind == core::ShearWallKind::Brace)
				{
					command.drawClass = CLASS_BRACE;
					command.width = extent.width;
					command.braceStyle = (group.doubleBrace || group.pieces.size() >= 2)
											 ? core::ShearWallBraceStyle::Double
											 : core::ShearWallBraceStyle::Single;
					command.braceRisesToEnd = first.risesToMax;
				}
				else
				{
					command.drawClass = CLASS_SHEAR_PANEL;
					resolvePanelSide(group, line, command);
				}
				commands.push_back(std::move(command));
			}
		}
		return commands;
	}

	std::vector<ShearWallCommand> buildShearWallCommands(Context& context)
	{
		return buildShearWallCommands(context, context.columns());
	}

	std::vector<ShearWallCommand> buildShearWallCommands(const Model& model)
	{
		Context context(model);
		return buildShearWallCommands(context, context.columns());
	}

	void fitShearWallsToMembers(std::vector<ShearWallCommand>& walls,
								const std::vector<StoryInfo>& stories,
								const std::vector<core::MemberCommand>& members,
								const std::vector<core::ColumnCommand>& columns)
	{
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerName(i, story.isTop, kLevelShearWall);
			// レイヤ平面（絶対 Z）＝その階の横架材天端（最上階は軒高）。
			const double layerZ = beamTopElevation(story);
			const std::vector<const core::ColumnCommand*> storyColumns = columnsOfStory(columns, i);
			for (ShearWallCommand& wall : walls)
			{
				if (wall.layer == layer)
					fitShearWall(wall, layerZ, members, storyColumns);
			}
		}
	}

	bool anyShearWallOnLayer(const std::vector<ShearWallCommand>& walls, const std::string& layer)
	{
		return std::ranges::any_of(walls, [&layer](const ShearWallCommand& wall)
								   { return wall.layer == layer; });
	}
} // namespace HomeskzIfcImport::parse
