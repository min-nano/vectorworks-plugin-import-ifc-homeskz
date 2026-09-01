//
//	parse/FloorPost.cpp
//
//	床束解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。
//

#include "parse/FloorPost.h"
#include "core/UnionFind.h"
#include "parse/Context.h"
#include "parse/Footing.h"
#include "parse/IfcAttr.h"
#include "parse/Member.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::SymbolCommand;
	using core::Vec2;

	namespace
	{
		// 横架材の型。大引・土台はどちらの型でも出てくる。
		constexpr std::array<const char*, 2> kMemberTypes = {"IFCBEAM", "IFCMEMBER"};

		// 要素の平面芯線（始点・単位方向・長さ・断面幅）を取り出す。配置・断面を解決
		// できない／平面へ落とすと長さ 0 の材は false（1 本の欠損で全体を止めない）。
		bool memberCenterLine(const Model& model, const Entity& element, Vec2& outOrigin,
							  Vec2& outDirection, double& outLength, double& outWidth)
		{
			MemberPlacement placement;
			MemberProfile profile;
			if (!memberPlacement3D(model, element, placement) ||
				!memberProfileDims(model, element, profile))
				return false;

			const Vec2 origin{placement.x, placement.y};
			const Vec2 end{placement.x + (placement.axis.x * profile.length),
						   placement.y + (placement.axis.y * profile.length)};
			const double length = core::distance(origin, end);
			if (length <= 0.0)
				return false;

			outOrigin = origin;
			outDirection = Vec2{(end.x - origin.x) / length, (end.y - origin.y) / length};
			outLength = length;
			outWidth = profile.width;
			return true;
		}

		// 床束（position を中心）が立上り 1 本の footprint を clearance だけ広げた領域に
		// 入っているか。壁芯を軸に、直交方向は 半壁厚 + clearance、沿軸方向は区間の外側へ
		// clearance まで見る（半壁厚は直交方向の寸法なので沿軸には効かない）。
		bool wallCovers(const core::WallCommand& wall, const Vec2& position, double clearance)
		{
			const Vec2 delta = wall.end - wall.start;
			const double wallLength = core::length(delta);
			if (wallLength <= 0.0)
				return false; // 縮退した立上りは向きが定まらない
			const Vec2 u{delta.x / wallLength, delta.y / wallLength};

			const Vec2 r = position - wall.start;
			const double along = core::dot(r, u); // 壁芯に沿った位置
			if (along < -clearance || along > wallLength + clearance)
				return false;
			const double perp = std::abs(core::cross(u, r)); // 壁芯からの直交距離
			return perp <= (wall.thickness / 2.0) + clearance;
		}

	} // namespace

	std::vector<double> floorPostOffsets(double length)
	{
		std::vector<double> offsets;
		if (length <= 0.0)
			return offsets;
		// 始点側の支持材芯から kFloorPostInterval ずつ。終点ちょうど（＝支持材芯）以遠には
		// 置かない（端部は支持材が受ける）ので、比較は「<」であって「<=」ではない。
		for (long long k = 1; kFloorPostInterval * static_cast<double>(k) < length; ++k)
			offsets.push_back(kFloorPostInterval * static_cast<double>(k));
		return offsets;
	}

	std::vector<SupportLine> collectSupportLines(const Model& model)
	{
		std::vector<SupportLine> lines;
		for (const char* memberType : kMemberTypes)
		{
			for (const int elementId : model.byType(memberType))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr)
					continue;
				// 大引を受けるのは土台と他の大引。それ以外の横架材（梁・桁）は床下に無いので
				// 支持材に数えない。
				const std::optional<std::string> memberClass =
					memberClassFromName(entityName(*element));
				if (!memberClass.has_value() ||
					(*memberClass != CLASS_DODAI && *memberClass != CLASS_OOBIKI))
					continue;

				SupportLine line;
				if (!memberCenterLine(model, *element, line.origin, line.direction, line.length,
									  line.width))
					continue;
				lines.push_back(line);
			}
		}
		return lines;
	}

	std::vector<OhbikiRun> collectOhbikiLines(const Model& model)
	{
		std::vector<OhbikiRun> lines;
		for (const char* memberType : kMemberTypes)
		{
			for (const int elementId : model.byType(memberType))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr)
					continue;
				const std::optional<std::string> memberClass =
					memberClassFromName(entityName(*element));
				if (!memberClass.has_value() || *memberClass != CLASS_OOBIKI)
					continue;

				Vec2 origin;
				Vec2 direction;
				double length = 0.0;
				double width = 0.0;
				if (!memberCenterLine(model, *element, origin, direction, length, width))
					continue;
				lines.push_back(OhbikiRun{origin, origin + (direction * length), width});
			}
		}
		return lines;
	}

	std::optional<Vec2> shinReference(const Vec2& point, const Vec2& direction,
									  const std::vector<SupportLine>& supports)
	{
		std::optional<double> bestT;
		std::optional<Vec2> bestPoint;
		for (const SupportLine& support : supports)
		{
			const double den = core::cross(direction, support.direction);
			// 平行（自身の芯線・同一直線上の大引を含む）は交点が定まらない。
			if (std::abs(den) < kFloorPostParallelTol)
				continue;

			const Vec2 r = support.origin - point;
			// t: 大引芯上のパラメータ（端からの符号付き距離）。s: 支持材芯上のパラメータ。
			const double t = core::cross(r, support.direction) / den;
			const double s = core::cross(r, direction) / den;
			if (s < -kFloorPostSegTol || s > support.length + kFloorPostSegTol)
				continue; // 交点が支持材の区間外＝受けていない
			if (std::abs(t) > (support.width / 2.0) + kFloorPostShinMargin)
				continue; // 大引端が支持材の footprint に載っていない

			if (!bestT.has_value() || std::abs(t) < std::abs(*bestT))
			{
				bestT = t;
				bestPoint = point + (direction * t);
			}
		}
		return bestPoint;
	}

	std::optional<double> collinearGap(const OhbikiRun& first, const OhbikiRun& second)
	{
		const Vec2 da = first.end - first.start;
		const double la = core::length(da);
		const Vec2 db = second.end - second.start;
		const double lb = core::length(db);
		if (la <= 0.0 || lb <= 0.0)
			return std::nullopt;

		const Vec2 u{da.x / la, da.y / la};
		// 方向が平行でなければ同一直線上ではない。
		if (std::abs(core::cross(u, Vec2{db.x / lb, db.y / lb})) > kCollinearAngleTol)
			return std::nullopt;
		// 平行でも別の直線上（芯線からの直交距離が大きい）なら継手ではない。
		const Vec2 offset = second.start - first.start;
		if (std::abs(core::cross(u, offset)) > kCollinearPerpTol)
			return std::nullopt;

		// a 方向に射影した b の区間と a の区間 [0, la] のすき間（重なる／接触するなら 0）。
		const double tb1 = core::dot(u, offset);
		const Vec2 offsetEnd = second.end - first.start;
		const double tb2 = core::dot(u, offsetEnd);
		const double lo = std::min(tb1, tb2);
		const double hi = std::max(tb1, tb2);
		if (lo > la)
			return lo - la;
		if (hi < 0.0)
			return -hi;
		return 0.0;
	}

	std::vector<OhbikiRun> mergeCollinearOhbiki(const std::vector<OhbikiRun>& lines)
	{
		// 同一直線上（collinearGap）ですき間が継手許容以下の大引を 1 つの連にまとめる。
		// 連結成分の骨格は core/UnionFind の connectedComponents（決定性の担保も同所）。
		const std::vector<std::vector<std::size_t>> components =
			core::connectedComponents(lines.size(),
									  [&lines](std::size_t i, std::size_t j)
									  {
										  const std::optional<double> gap =
											  collinearGap(lines[i], lines[j]);
										  return gap.has_value() && *gap <= kJointGapTol;
									  });

		std::vector<OhbikiRun> runs;
		runs.reserve(components.size());
		for (const std::vector<std::size_t>& members : components)
		{
			if (members.size() == 1)
			{
				runs.push_back(lines[members.front()]);
				continue;
			}

			// 成分の先頭（＝代表）の芯線方向へ全端点を射影し、最小〜最大区間の 1 本にする
			// （core/Geometry の collinearSpan）。統合した連の床束幅は成分の最大値
			// （安全側＝立上りとの重なりを拾い漏らさない）。
			OhbikiRun run;
			core::collinearSpan(lines, members, run.start, run.end);
			for (const std::size_t index : members)
				run.width = std::max(run.width, lines[index].width);
			runs.push_back(run);
		}
		return runs;
	}

	bool overlapsFoundationWall(const Vec2& position, double postWidth,
								const std::vector<core::WallCommand>& walls)
	{
		// 床束の半幅ぶん（＋丸め誤差の下駄）だけ立上りの footprint を広げてから点で判定する。
		const double clearance = (postWidth / 2.0) + kFloorPostWallMargin;
		return std::ranges::any_of(walls, [&position, clearance](const core::WallCommand& wall)
								   { return wallCovers(wall, position, clearance); });
	}

	std::vector<SymbolCommand> buildFloorPostCommands(Context& context)
	{
		const Model& model = context.model();
		// 基礎が無いモデルは配置先レイヤ（F-床束）も高さ基準も定まらないので何も出さない。
		if (!hasFoundation(model))
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();
		const std::vector<SupportLine> supports = collectSupportLines(model);
		const std::vector<OhbikiRun> runs = mergeCollinearOhbiki(collectOhbikiLines(model));
		// 立上りは**センタリング済み**の命令（人通口の分割・切り下げまで反映済み）。
		// コンテキストが 1 回だけ組み立てたものを共有する（parse/Context）。
		const std::vector<core::WallCommand>& walls = context.walls();

		std::vector<SymbolCommand> commands;
		for (const OhbikiRun& run : runs)
		{
			const Vec2 delta = run.end - run.start;
			const double length = core::length(delta);
			if (length <= 0.0)
				continue;
			const Vec2 direction{delta.x / length, delta.y / length};

			// 端部は実部材端ではなく支持材芯（受ける支持材が無ければ実部材端に戻す）。
			const Vec2 start = shinReference(run.start, direction, supports).value_or(run.start);
			const Vec2 end = shinReference(run.end, direction, supports).value_or(run.end);
			const Vec2 span = end - start;
			const double spanLength = (span.x * direction.x) + (span.y * direction.y);
			if (spanLength <= 0.0)
				continue;

			for (const double distance : floorPostOffsets(spanLength))
			{
				const Vec2 position = start + (direction * distance) - center;
				// 立上りと重なる位置には立てられない（その位置の大引は立上りが受ける）ので
				// **その 1 本だけ落とす**——間隔は詰め替えない（parse/FloorPost.h の doc）。
				if (overlapsFoundationWall(position, run.width, walls))
					continue;

				SymbolCommand command;
				command.layer = kLayerFoundationFloorPost;
				command.symbol = kSymbolFloorPost;
				command.position = position;
				// 回転角は持たない（床束は軸対称）。SymbolCommand::angle の既定 0 のまま。
				commands.push_back(std::move(command));
			}
		}
		return commands;
	}

	std::vector<SymbolCommand> buildFloorPostCommands(const Model& model)
	{
		Context context(model);
		return buildFloorPostCommands(context);
	}
} // namespace HomeskzIfcImport::parse
