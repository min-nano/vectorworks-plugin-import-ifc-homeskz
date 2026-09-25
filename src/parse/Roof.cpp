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

		// 軒（屋根軸）は**勾配座標系で見た footprint の外接矩形の、軒側の辺そのもの**。
		// {along, down} は正規直交（parse/IfcGeometry の roofSlope が down を単位化し
		// along をその直交にする）ので、射影値の組 (e, d) から平面座標へそのまま戻せる。
		//
		// ★**軸は footprint の外へ出してはならない**（M28）。かつては「最も軒側の頂点を 1 つ
		// 選び、そこから along 方向へ eSpan だけ伸ばす」作りだったが、選んだ頂点が軒方向の
		// **終わり側**（e = eMax）に在ると、終点が eMax + eSpan ＝ footprint 1 つぶん外へ
		// 飛び出す。屋根面オブジェクトは**この軸を勾配の基準線として図に描く**ので、飛び出した
		// 軸がそのままビューポートの外形を広げ、伏図が用紙に収まらなくなっていた（実機で
		// 母屋伏図が縦に建物 1 つぶん＝5,680mm 大きく測られた。docs/DEV-NOTES.md M28）。
		//
		// **屋根面の平面そのものは変わらない**——軸は変更前と同じ d = dMax の直線上にあり、
		// 動かすのは直線上での端点だけなので、勾配も軒の高さ（elevation）も同じである。
		const auto atSlopeCoord = [&slope](double e, double d)
		{
			return Vec2{(slope.along.x * e) + (slope.down.x * d),
						(slope.along.y * e) + (slope.down.y * d)};
		};
		// 軸は軒（最も +d 側）の辺を端から端まで。upslope 定義点は棟（最も -d 側）の中央
		// ——**方向ではなく「棟側にある点」**なので、footprint の内側に採れば足りる
		// （core/Document.h の RoofCommand）。
		const Vec2 axisStart = atSlopeCoord(eMin, dMax);
		const Vec2 axisEnd = atSlopeCoord(eMax, dMax);
		const Vec2 upslope = atSlopeCoord((eMin + eMax) / 2.0, dMin);

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
		// 軒の Z は**軸の上ならどこで測っても同じ**（平面の Z は勾配方向 d だけで決まり、
		// 軸は d = dMax の直線に乗っている）ので、軸の始点で代表する。
		cmd.elevation = slope.zAt(axisStart.x, axisStart.y, storeyElevation) + lift;
		return cmd;
	}

	std::vector<RoofCommand> buildRoofCommands(Context& context)
	{
		const std::vector<StoryInfo>& stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();

		std::vector<RoofCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerName(i, story.isTop, kLevelNojiita);

			// 屋根版の判定・屋根面の走査は垂木（parse/Rafter）・登り梁と共有する
			// （Context::storyRoofPlanes。同じ屋根版を拾い、解決も 1 度で済む）。
			for (const RoofPlane* plane : context.storyRoofPlanes(story.id))
			{
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
