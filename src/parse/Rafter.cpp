//
//	parse/Rafter.cpp
//
//	垂木解析の実装。Python 版 ifc/rafter.py の build_rafter_commands ほかに対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Rafter.h"
#include "parse/Grid.h"
#include "parse/IfcGeometry.h"
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
	using core::RafterCommand;
	using core::Vec2;
	using core::Vec3;

	namespace
	{
		// IfcRoot の Name 属性インデックス（GlobalId, OwnerHistory, Name=2, …）。
		constexpr std::size_t kNameAttr = 2;

		// 屋根面の法線の水平成分がこれ以下（ほぼ水平な面）なら勾配方向が定まらないため垂木を
		// 流さない（Python 版 _FLAT_TOL）。
		constexpr double kFlatTol = 1e-6;

		// クリップした垂木の平面投影長がこれ未満（隅木際の極小片等）なら配置しない（mm。
		// Python 版 _MIN_RAFTER_LENGTH）。
		constexpr double kMinRafterLength = 100.0;

		// 掃引線と外形辺の交点判定の許容（mm。Python 版 _EDGE_TOL）。両端の掃引線を半幅
		// 内側へ寄せた実効幅がこの 2 倍以下なら、区間が取れない極小面として中央 1 本にする。
		constexpr double kEdgeTol = 1.0;

		// 要素が屋根版（IfcSlab かつ Name が "屋根版" 始まり）か（Python 版の
		// element.is_a('IfcSlab') and Name.startswith('屋根版') と同じ判定）。
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

		// 走査線と外形辺の交点 1 つ（勾配方向 d の座標＋平面座標）。d 昇順に並べると
		// [偶, 奇] の対が面内の区間になる（非凸面も走査線法で正しく分割される）。
		struct Hit
		{
			double d = 0.0;
			double x = 0.0;
			double y = 0.0;
		};
	} // namespace

	bool storyHasRoofSlab(const Model& model, int storeyId)
	{
		const std::vector<int> elements = collectStoryElements(model, storeyId);
		return std::ranges::any_of(elements,
								   [&model](int elementId)
								   {
									   const Entity* element = model.entity(elementId);
									   return element != nullptr && isRoofSlab(*element);
								   });
	}

	std::string rafterLabel()
	{
		// 断面・間隔が決め打ちなので全垂木で共通のラベル（"45×45@455"）。整数へ丸めて
		// 組み立てるのは Python 版 _rafters_for_plane と同じ（表示用のラベルなので端数不要）。
		const long long w = std::llround(kDefaultRafterWidth);
		const long long h = std::llround(kDefaultRafterHeight);
		const long long interval = std::llround(kRafterInterval);
		return std::to_string(w) + "×" + std::to_string(h) + "@" + std::to_string(interval);
	}

	std::vector<double> sweepPositions(double eMin, double eMax, double interval, double inset)
	{
		const double loEdge = eMin + inset;
		const double hiEdge = eMax - inset;
		const double width = hiEdge - loEdge;
		if (width <= 2.0 * kEdgeTol)
		{
			// 半幅を差し引くと広がりが極小（屋根が垂木幅程度に狭い）: 中央 1 本のみ。
			return {(eMin + eMax) / 2.0};
		}

		// interval 以下に割る最小の区間数（1e-9 は「ちょうど整数倍」を切り上げない保険。
		// Python 版と同値）。
		const auto n = static_cast<long long>(std::ceil((width / interval) - 1e-9));
		std::vector<double> positions;
		if (n <= 1)
		{
			positions = {loEdge, hiEdge};
			return positions;
		}

		// 中間 n−2 区間は interval ちょうど、端数（余り）は両端 2 区間へ等分する。
		const double endGap = (width - ((static_cast<double>(n) - 2.0) * interval)) / 2.0;
		positions.reserve(static_cast<std::size_t>(n) + 1);
		positions.push_back(loEdge);
		positions.push_back(loEdge + endGap);
		for (long long i = 1; i < n - 1; ++i)
			positions.push_back(loEdge + endGap + (static_cast<double>(i) * interval));
		positions.push_back(hiEdge);
		return positions;
	}

	std::vector<RafterCommand> raftersForPlane(const RoofPlane& plane, const std::string& layer,
											   double storeyElevation, const Vec2& center,
											   std::optional<double> beamTopZ)
	{
		const double nx = plane.normal.x;
		const double ny = plane.normal.y;
		const double nz = plane.normal.z;
		const double dh = std::hypot(nx, ny);
		if (dh <= kFlatTol)
			return {}; // ほぼ水平な面は勾配方向が定まらない

		// 勾配方向 d（最急降下＝水平法線方向）。+d へ進むと天端 Z は下がる（軒側）。
		const double dx = nx / dh;
		const double dy = ny / dh;
		// 掃引方向 e（軒・棟に平行。勾配方向に直交）。
		const double ex = -dy;
		const double ey = dx;

		// 平面外形の XY と、平面式の基準点（頂点 0）。
		std::vector<Vec2> plan;
		plan.reserve(plane.vertices.size());
		for (const Vec3& v : plane.vertices)
			plan.push_back(Vec2{v.x, v.y});
		const Vec3 origin = plane.vertices.front();

		// 屋根面（平面）上の点の天端 Z。ストーリ相対 → Elevation を足して絶対値にする
		// （法線の符号反転に対して不変: nx/ny/nz が揃って反転し比が変わらない）。
		const auto zAt = [&](double x, double y) {
			return origin.z - (((nx * (x - origin.x)) + (ny * (y - origin.y))) / nz) +
				   storeyElevation;
		};

		double eMin = (plan.front().x * ex) + (plan.front().y * ey);
		double eMax = eMin;
		for (const Vec2& p : plan)
		{
			const double e = (p.x * ex) + (p.y * ey);
			eMin = std::min(eMin, e);
			eMax = std::max(eMax, e);
		}
		if (eMax - eMin < kMinRafterLength)
			return {}; // 掃引方向の広がりが極小な面（退化した屋根版）

		const std::string label = rafterLabel();
		// 差し込み（支持点→壁外面）。M6 は横架材が未導入なので既定桁幅の半分
		// （ヘッダ「M6 のスコープ」。Python 版も members が空なら同じ値になる）。
		const double embedment = kDefaultGirderWidth / 2.0;

		const std::size_t vertexCount = plan.size();
		std::vector<RafterCommand> commands;
		for (const double t :
			 sweepPositions(eMin, eMax, kRafterInterval, kDefaultRafterWidth / 2.0))
		{
			// 掃引線 { p : p·e = t } と外形の交点を集め、勾配方向 d の座標を添える。
			std::vector<Hit> hits;
			for (std::size_t i = 0; i < vertexCount; ++i)
			{
				const Vec2& a = plan[i];
				const Vec2& b = plan[(i + 1) % vertexCount];
				const double f0 = (a.x * ex) + (a.y * ey) - t;
				const double f1 = (b.x * ex) + (b.y * ey) - t;
				// 半開区間の判定（f<=0<f' またはその逆）で、頂点を 2 度数えない。
				const bool crossesEdge = (f0 <= 0.0 && f1 > 0.0) || (f1 <= 0.0 && f0 > 0.0);
				if (!crossesEdge)
					continue;
				const double r = f0 / (f0 - f1);
				const double ix = a.x + (r * (b.x - a.x));
				const double iy = a.y + (r * (b.y - a.y));
				hits.push_back(Hit{(ix * dx) + (iy * dy), ix, iy});
			}
			if (hits.size() < 2)
				continue;

			// d 昇順（同値は x → y で安定）に並べ、[偶, 奇] の対が面内の区間になる
			// （Python 版が交点タプル (d, x, y) をそのままソートするのと同じ順序で、
			// 交点の検出順に依存しない決定的な並びになる）。
			std::ranges::sort(hits,
							  [](const Hit& a, const Hit& b)
							  {
								  if (a.d < b.d || b.d < a.d)
									  return a.d < b.d;
								  if (a.x < b.x || b.x < a.x)
									  return a.x < b.x;
								  return a.y < b.y;
							  });

			for (std::size_t j = 0; j + 1 < hits.size(); j += 2)
			{
				const Hit& high = hits[j];	  // d 最小 = 高い側 = 棟側
				const Hit& low = hits[j + 1]; // d 最大 = 低い側 = 軒先
				if (std::hypot(low.x - high.x, low.y - high.y) < kMinRafterLength)
					continue; // 隅木際の極小片・端で退化した区間は配置しない

				const double zTip = zAt(low.x, low.y);	   // 軒先の天端 Z
				const double zRidge = zAt(high.x, high.y); // 棟側の天端 Z

				// 支持点 = 屋根面が横架材天端（軒高）Z と交わる点。軒先→棟の線上で
				// z=beamTopZ となる位置 s を採る。軒先が既に beamTopZ 以上（s<=0）や面全体が
				// 下（s>=1）なら支持点は取れないので軒先のままにする。
				double supportX = low.x;
				double supportY = low.y;
				double supportZ = zTip;
				double supportToTip = 0.0;
				if (beamTopZ.has_value())
				{
					const double dz = zRidge - zTip;
					const double s = (dz > kFlatTol) ? ((*beamTopZ - zTip) / dz) : 0.0;
					if (s > 0.0 && s < 1.0)
					{
						supportX = low.x + (s * (high.x - low.x));
						supportY = low.y + (s * (high.y - low.y));
						supportZ = *beamTopZ;
						supportToTip = std::hypot(supportX - low.x, supportY - low.y);
					}
				}

				RafterCommand cmd;
				cmd.layer = layer;
				cmd.drawClass = CLASS_TARUKI;
				cmd.width = kDefaultRafterWidth;
				cmd.height = kDefaultRafterHeight;
				// start=軒側（支持点）、end=棟側（高い端）。座標はセンタリング済み。
				cmd.start = Vec2{supportX - center.x, supportY - center.y};
				cmd.end = Vec2{high.x - center.x, high.y - center.y};
				cmd.elevation = supportZ;
				cmd.endElevation = zRidge;
				// 壁外面から軒先の距離（overhang）＝ 支持点→軒先（supportToTip）から支持部分の
				// 差し込み（embedment ＝ 支持点→壁外面）を引いた残り。VW の垂木は軒先を
				// 支持点＋差し込み＋軒の出 に置くため、両者の和が supportToTip になるようにする。
				cmd.overhang = std::max(0.0, supportToTip - embedment);
				cmd.embedment = embedment;
				cmd.label = label;
				commands.push_back(std::move(cmd));
			}
		}
		return commands;
	}

	std::vector<RafterCommand> buildRafterCommands(const Model& model)
	{
		const std::vector<StoryInfo> stories = collectStories(model);
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ補正なし＝生の IFC 座標）。
		Vec2 center{0.0, 0.0};
		resolveGridCenter(model, center);

		std::vector<RafterCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerPrefix(i, story.isTop) + "-" + kLevelTaruki;
			// 支持点が乗る横架材天端の絶対 Z（最上階は軒高＝オフセット 0）。
			const double beamTopZ =
				story.isTop ? story.elevation : story.elevation + story.beamOffset;

			for (const int elementId : collectStoryElements(model, story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isRoofSlab(*element))
					continue;

				RoofPlane plane;
				if (!roofPlane(model, element, plane))
					continue; // 屋根面を解決できない屋根版はスキップ

				std::vector<RafterCommand> rafters =
					raftersForPlane(plane, layer, story.elevation, center, beamTopZ);
				for (RafterCommand& rafter : rafters)
					commands.push_back(std::move(rafter));
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
