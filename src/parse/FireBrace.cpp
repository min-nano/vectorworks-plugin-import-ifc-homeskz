//
//	parse/FireBrace.cpp
//
//	火打解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。
//

#include "parse/FireBrace.h"
#include "core/ImportOptions.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
#include "parse/IfcGeometry.h"
#include "parse/Story.h"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::SymbolCommand;
	using core::Vec2;
	using core::Vec3;

	namespace
	{
		// 火打の平面外形を「ワールド XY 頂点列」と「プロファイル局所頂点列」の対で返す。
		// 局所頂点はワールド頂点と同じ並びで、端面（局所 Y の符号が始終点で反転する辺）
		// の識別に使う。押し出しソリッドを解決できなければ false。
		bool worldFootprint(const Model& model, const Entity& element, std::vector<Vec2>& outWorld,
							std::vector<Vec2>& outLocal)
		{
			WorldSolid solid;
			if (!resolveElementWorldSolid(model, &element, solid))
				return false;

			// base() は origin + xAxis·u + yAxis·v。火打は鉛直押し出しなので、この底面ループ
			// の XY がそのまま平面外形になる。
			const std::vector<Vec3> base = solid.base();
			outWorld.clear();
			outWorld.reserve(base.size());
			for (const Vec3& point : base)
				outWorld.push_back(Vec2{point.x, point.y});
			outLocal = solid.profile;
			return true;
		}
	} // namespace

	bool isFireBrace(const Entity& element)
	{
		if (element.type != "IFCBEAM" && element.type != "IFCMEMBER")
			return false;
		const std::string name = entityName(element);
		const std::string prefix(kFireBracePrefix);
		return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
	}

	std::optional<Vec2> segmentIntersection(const Segment2D& first, const Segment2D& second)
	{
		const Vec2 d1 = first.b - first.a;
		const Vec2 d2 = second.b - second.a;
		const double denom = (d1.x * d2.y) - (d1.y * d2.x);
		if (std::abs(denom) < kFireBraceParallelTol)
			return std::nullopt;
		const double t =
			(((second.a.x - first.a.x) * d2.y) - ((second.a.y - first.a.y) * d2.x)) / denom;
		return Vec2{first.a.x + (t * d1.x), first.a.y + (t * d1.y)};
	}

	std::vector<Segment2D> fireBraceEndFaces(const std::vector<Vec2>& world,
											 const std::vector<Vec2>& local)
	{
		std::vector<Segment2D> faces;
		// world と local は同じ並び。長さが食い違う（＝解決に失敗した）ときは端面を出さない。
		if (local.size() != world.size())
			return faces;

		const std::size_t n = local.size();
		for (std::size_t i = 0; i < n; ++i)
		{
			const std::size_t next = (i + 1) % n;
			// 中心線（局所 v=0）をまたぐ辺だけが端面。長辺は v が ±半幅で一定なので
			// 積が正になり、ここで落ちる。
			if (local[i].y * local[next].y < 0.0)
				faces.push_back(Segment2D{world[i], world[next]});
		}
		return faces;
	}

	std::optional<Vec2> fireBraceBasePoint(const std::vector<Segment2D>& faces)
	{
		if (faces.size() != 2)
			return std::nullopt;
		return segmentIntersection(faces[0], faces[1]);
	}

	double fireBraceAngle(const Vec2& base, const std::vector<Vec2>& world)
	{
		if (world.empty())
			return 0.0;

		Vec2 centroid;
		for (const Vec2& point : world)
			centroid = centroid + point;
		const auto count = static_cast<double>(world.size());
		centroid = centroid * (1.0 / count);

		// 内角の二等分方向（基準点 → 重心）。シンボルの基準姿勢のずれを補正して返す。
		const double bisector =
			std::atan2(centroid.y - base.y, centroid.x - base.x) * 180.0 / std::numbers::pi;
		return bisector + kFireBraceAngleOffset;
	}

	std::vector<SymbolCommand> buildFireBraceCommands(Context& context)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo> stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();

		std::vector<SymbolCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			// 火打は横架材と同じレイヤに置く。最上階には横架材天端が無いので軒高。
			const std::string layer =
				storyLayerName(i, story.isTop, story.isTop ? kLevelEaves : kLevelBeamTop);

			for (const int elementId : context.storyElements(story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isFireBrace(*element))
					continue;

				std::vector<Vec2> world;
				std::vector<Vec2> local;
				if (!worldFootprint(model, *element, world, local))
					continue; // 押し出しソリッドを解決できない火打はスキップ

				const std::optional<Vec2> base =
					fireBraceBasePoint(fireBraceEndFaces(world, local));
				if (!base.has_value())
					continue; // 端面が 2 つ取れない／平行で交点が定まらない火打はスキップ

				SymbolCommand command;
				command.layer = layer;
				command.symbol = context.options().symbol(core::SymbolRole::FireBrace);
				command.position = *base - center;
				command.angle = fireBraceAngle(*base, world);
				commands.push_back(std::move(command));
			}
		}
		return commands;
	}

	std::vector<SymbolCommand> buildFireBraceCommands(const Model& model)
	{
		Context context(model);
		return buildFireBraceCommands(context);
	}
} // namespace HomeskzIfcImport::parse
