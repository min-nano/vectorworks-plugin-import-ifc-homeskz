//
//	parse/Roof.cpp
//
//	野地板解析の実装。Python 版 ifc/roof.py の build_roof_commands ほかに対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Roof.h"
#include "parse/Grid.h"
#include "parse/IfcGeometry.h"
#include "parse/Rafter.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::RoofCommand;
	using core::Vec2;
	using core::Vec3;

	namespace
	{
		// IfcRoot の Name 属性インデックス（GlobalId, OwnerHistory, Name=2, …）。
		constexpr std::size_t kNameAttr = 2;

		// 屋根面の法線の水平成分／鉛直成分がこれ以下なら勾配・軒・天端 Z が定まらないため
		// 屋根オブジェクトを作らない（Python 版 _FLAT_TOL。垂木と同じ扱い）。
		constexpr double kFlatTol = 1e-6;

		// footprint の広がり（軒方向・勾配方向）がこれ未満なら退化とみなしスキップ（mm。
		// Python 版 _MIN_SPAN）。
		constexpr double kMinSpan = 1.0;

		// 要素が屋根版（IfcSlab かつ Name が "屋根版" 始まり）か。判定は垂木（parse/Rafter）と
		// 同じで、屋根面を共有する両者が同じ屋根版を拾うようにする。
		bool isRoofSlab(const Entity& element)
		{
			if (element.type != "IFCSLAB")
				return false;
			const Value& name = element.attribute(kNameAttr);
			if (name.type != ValueType::String)
				return false;
			const std::string prefix(kRoofSlabPrefix);
			return name.text.compare(0, prefix.size(), prefix) == 0;
		}
	} // namespace

	std::optional<RoofCommand> roofCommandForPlane(const RoofPlane& plane, const std::string& layer,
												   double storeyElevation, const Vec2& center)
	{
		const double nx = plane.normal.x;
		const double ny = plane.normal.y;
		const double nz = plane.normal.z;
		const double dh = std::hypot(nx, ny);
		if (dh <= kFlatTol)
			return std::nullopt; // ほぼ水平な面は勾配方向・軒が定まらない
		if (nz <= kFlatTol)
		{
			// 鉛直な面（法線が水平）は勾配・天端 Z が定まらない（平面式が nz で除算する）。
			return std::nullopt;
		}

		// 勾配方向 d（最急降下＝水平法線方向）。+d へ進むと天端 Z は下がる（軒側）。
		const double dx = nx / dh;
		const double dy = ny / dh;
		// 軒・棟に平行な方向 e（勾配方向に直交）。
		const double ex = -dy;
		const double ey = dx;
		// 棟（高い）側を指す upslope 単位方向（勾配方向の逆）。
		const double ux = -dx;
		const double uy = -dy;

		std::vector<Vec2> plan;
		plan.reserve(plane.vertices.size());
		for (const Vec3& v : plane.vertices)
			plan.push_back(Vec2{v.x, v.y});
		const Vec3 origin = plane.vertices.front();

		// 屋根面（平面）上の点の天端 Z（絶対値。ストーリ相対 ＋ Elevation）。
		const auto zAt = [&](double x, double y) {
			return origin.z - (((nx * (x - origin.x)) + (ny * (y - origin.y))) / nz) +
				   storeyElevation;
		};

		// 勾配方向・軒方向への射影の範囲（footprint の広がり）。
		std::vector<double> ds;
		ds.reserve(plan.size());
		double eMin = (plan.front().x * ex) + (plan.front().y * ey);
		double eMax = eMin;
		for (const Vec2& p : plan)
		{
			ds.push_back((p.x * dx) + (p.y * dy));
			const double e = (p.x * ex) + (p.y * ey);
			eMin = std::min(eMin, e);
			eMax = std::max(eMax, e);
		}
		const double dMin = *std::ranges::min_element(ds);
		const double dMax = *std::ranges::max_element(ds);
		const double eSpan = eMax - eMin;
		const double dSpan = dMax - dMin;
		if (eSpan < kMinSpan || dSpan < kMinSpan)
			return std::nullopt; // 退化した屋根版（線状・点状）

		// 軒（屋根軸）の基準点 = 最も低い（最も +d 側＝軒側）の頂点。ここを通り e 方向に
		// 伸ばした軸なら footprint 全体が軸の棟側（upslope 側）に来る。最大値が複数あるときは
		// 最初の頂点を採る（Python 版 max(range(...), key=…) と同じ＝決定的）。
		const auto eaveIndex =
			static_cast<std::size_t>(std::distance(ds.begin(), std::ranges::max_element(ds)));
		const double ax = plan[eaveIndex].x;
		const double ay = plan[eaveIndex].y;

		// 軸は軒に沿って footprint の広がりぶん伸ばす（方向が主で、長さは表現用）。
		// upslope 定義点は軸から棟側へ勾配方向の広がりぶん進んだ点（同じく方向が主）。
		const Vec2 axisStart{ax, ay};
		const Vec2 axisEnd{ax + (ex * eSpan), ay + (ey * eSpan)};
		const Vec2 upslope{ax + (ux * dSpan), ay + (uy * dSpan)};

		// 野地板は垂木の上に載る（野地板下端＝垂木上端）。垂木下端が屋根版の平面に一致すること
		// が Python 版の VW 上の実測で確認されているため、屋根版の平面（zAt）から垂木せい
		// （屋根面に直交する寸法）を鉛直換算（÷cosθ、cosθ＝単位法線の鉛直成分 nz）して
		// 持ち上げた Z を軒（軸）の目標にする。
		//
		// ［仕様メモ］Python 版 CLAUDE.md「野地板」節の文言は「垂木せい＋野地板厚を鉛直換算」と
		// 読めるが、実装（ifc/roof.py）とそのテスト（test_ifc_roof.py の
		// test_elevation_is_rafter_top_plus_sheathing）はいずれも**垂木せいのみ**を持ち上げて
		// おり（＝軸 Z は野地板の下端＝垂木上端で、厚みは軸から上へ伸びる）、ローカル確認済みの
		// 実装がこちら。実装＝実証済みの資産に合わせる（CLAUDE.md「移植の基本方針」）。厚みが
		// 軸のどちら側へ伸びるかは VW 実機での目視確認項目にする（ROADMAP.md M6）。
		const double lift = kDefaultRafterHeight / nz;

		RoofCommand cmd;
		cmd.layer = layer;
		cmd.drawClass = CLASS_ROOF_SHEATHING;
		cmd.boundary.reserve(plan.size());
		for (const Vec2& p : plan)
			cmd.boundary.push_back(Vec2{p.x - center.x, p.y - center.y});
		cmd.axisStart = Vec2{axisStart.x - center.x, axisStart.y - center.y};
		cmd.axisEnd = Vec2{axisEnd.x - center.x, axisEnd.y - center.y};
		cmd.upslope = Vec2{upslope.x - center.x, upslope.y - center.y};
		cmd.rise = dh;
		cmd.run = nz;
		cmd.thickness = kNojiitaThickness;
		cmd.elevation = zAt(ax, ay) + lift;
		return cmd;
	}

	std::vector<RoofCommand> buildRoofCommands(const Model& model)
	{
		const std::vector<StoryInfo> stories = collectStories(model);
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ補正なし＝生の IFC 座標）。
		Vec2 center{0.0, 0.0};
		resolveGridCenter(model, center);

		std::vector<RoofCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerPrefix(i, story.isTop) + "-" + kLevelNojiita;

			for (const int elementId : collectStoryElements(model, story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isRoofSlab(*element))
					continue;

				RoofPlane plane;
				if (!roofPlane(model, element, plane))
					continue; // 屋根面を解決できない屋根版はスキップ

				std::optional<RoofCommand> command =
					roofCommandForPlane(plane, layer, story.elevation, center);
				if (command.has_value())
					commands.push_back(std::move(*command));
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
