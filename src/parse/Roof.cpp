//
//	parse/Roof.cpp
//
//	野地板解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。
//

#include "parse/Roof.h"
#include "parse/Context.h"
#include "parse/IfcGeometry.h"
#include "parse/Rafter.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::RoofCommand;
	using core::Vec2;

	namespace
	{
		// footprint の広がり（軒方向・勾配方向）がこれ未満なら退化とみなしスキップ（mm）。
		constexpr double kMinSpan = 1.0;
	} // namespace

	std::optional<RoofCommand> roofCommandForPlane(const RoofPlane& plane, const std::string& layer,
												   double storeyElevation, const Vec2& center)
	{
		// 勾配の座標系は垂木（parse/Rafter）と共有する（parse/IfcGeometry の RoofSlope）。
		// ほぼ水平な面（勾配方向・軒が定まらない）と鉛直な面（平面式が nz で除算する）は
		// ここで弾かれる（閾値 kRoofFlatTol も垂木と共有＝roofSlope の既定値）。
		RoofSlope slope;
		if (!roofSlope(plane, slope))
			return std::nullopt;

		const std::vector<Vec2> plan = RoofSlope::plan(plane);

		// 勾配方向・軒方向への射影の範囲（footprint の広がり）。
		double dMin = 0.0;
		double dMax = 0.0;
		double eMin = 0.0;
		double eMax = 0.0;
		RoofSlope::projectionRange(plan, slope.down, dMin, dMax);
		RoofSlope::projectionRange(plan, slope.along, eMin, eMax);
		const double eSpan = eMax - eMin;
		const double dSpan = dMax - dMin;
		if (eSpan < kMinSpan || dSpan < kMinSpan)
			return std::nullopt; // 退化した屋根版（線状・点状）

		// 軒（屋根軸）の基準点 = 最も低い（最も +d 側＝軒側）の頂点。ここを通り e
		// 方向に伸ばした軸なら footprint 全体が軸の棟側（upslope 側）に来る。最大値が複数ある
		// ときは最初の頂点を採る。
		std::size_t eaveIndex = 0;
		double eaveD = 0.0;
		for (std::size_t i = 0; i < plan.size(); ++i)
		{
			const double d = (plan[i].x * slope.down.x) + (plan[i].y * slope.down.y);
			if (i == 0 || d > eaveD)
			{
				eaveD = d;
				eaveIndex = i;
			}
		}
		const double ax = plan[eaveIndex].x;
		const double ay = plan[eaveIndex].y;

		// 軸は軒に沿って footprint の広がりぶん伸ばす（方向が主で、長さは表現用）。
		// upslope 定義点は軸から棟側へ勾配方向の広がりぶん進んだ点（同じく方向が主）。
		// 棟（高い）側を指す upslope 単位方向は勾配方向の逆。
		const Vec2 axisStart{ax, ay};
		const Vec2 axisEnd{ax + (slope.along.x * eSpan), ay + (slope.along.y * eSpan)};
		const Vec2 upslope{ax - (slope.down.x * dSpan), ay - (slope.down.y * dSpan)};

		// 野地板は垂木の上に載る（野地板下端＝垂木上端）。垂木下端は屋根版の平面に一致する
		// （実機で確認済み）ので、屋根版の平面（zAt）から垂木せい（屋根面に直交する寸法）
		// を鉛直換算（÷cosθ、cosθ＝単位法線の鉛直成分＝slope.run）して持ち上げた Z を軒（軸）
		// の目標にする。
		//
		// ［仕様メモ］持ち上げるのは**垂木せいのみ**（＝軸 Z は野地板の下端＝垂木上端で、
		// 厚みは軸から上へ伸びる）。野地板厚まで足すと 1 枚ぶん浮くので足さない。
		// 厚みが軸のどちら側へ伸びるかは実機での目視確認項目（docs/DEV-NOTES.md M6）。
		const double lift = kDefaultRafterHeight / slope.run;

		RoofCommand cmd;
		cmd.layer = layer;
		cmd.drawClass = CLASS_ROOF_SHEATHING;
		cmd.boundary.reserve(plan.size());
		for (const Vec2& p : plan)
			cmd.boundary.push_back(Vec2{p.x - center.x, p.y - center.y});
		cmd.axisStart = Vec2{axisStart.x - center.x, axisStart.y - center.y};
		cmd.axisEnd = Vec2{axisEnd.x - center.x, axisEnd.y - center.y};
		cmd.upslope = Vec2{upslope.x - center.x, upslope.y - center.y};
		cmd.rise = slope.rise;
		cmd.run = slope.run;
		cmd.thickness = kNojiitaThickness;
		cmd.elevation = slope.zAt(ax, ay, storeyElevation) + lift;
		return cmd;
	}

	std::vector<RoofCommand> buildRoofCommands(Context& context)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo> stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();

		std::vector<RoofCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerName(i, story.isTop, kLevelNojiita);

			for (const int elementId : context.storyElements(story.id))
			{
				const Entity* element = model.entity(elementId);
				// 屋根版の判定は垂木（parse/Rafter）と同じで、屋根面を共有する両者が
				// 同じ屋根版を拾うようにする。
				if (element == nullptr || !isRoofSlab(*element))
					continue;

				// 屋根面は垂木（parse/Rafter）と共有する（コンテキストが 1 度だけ解決する）。
				const RoofPlane* plane = context.roofPlane(elementId);
				if (plane == nullptr)
					continue; // 屋根面を解決できない屋根版はスキップ

				std::optional<RoofCommand> command =
					roofCommandForPlane(*plane, layer, story.elevation, center);
				if (command.has_value())
					commands.push_back(std::move(*command));
			}
		}
		return commands;
	}

	std::vector<RoofCommand> buildRoofCommands(const Model& model)
	{
		Context context(model);
		return buildRoofCommands(context);
	}
} // namespace HomeskzIfcImport::parse
