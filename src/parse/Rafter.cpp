//
//	parse/Rafter.cpp
//
//	垂木解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。
//

#include "parse/Rafter.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
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
	using core::StoryBoundCommand;
	using core::Vec2;

	namespace
	{
		// クリップした垂木の平面投影長がこれ未満（隅木際の極小片等）なら配置しない（mm）。
		constexpr double kMinRafterLength = 100.0;

		// 掃引線と外形辺の交点判定の許容（mm）。両端の掃引線を半幅内側へ寄せた実効幅がこの
		// 2 倍以下なら、区間が取れない極小面として中央 1 本にする。
		constexpr double kEdgeTol = 1.0;

		// 走査線と外形辺の交点 1 つ（勾配方向 d の座標＋平面座標）。d 昇順に並べると
		// [偶, 奇] の対が面内の区間になる（非凸面も走査線法で正しく分割される）。
		struct Hit
		{
			double d = 0.0;
			double x = 0.0;
			double y = 0.0;
		};
	} // namespace

	bool isRoofSlab(const Entity& element)
	{
		if (element.type != "IFCSLAB")
			return false;
		// IfcSlab で Name が "屋根版" で始まるものを屋根版とみなす。
		const std::string name = entityName(element);
		const std::string prefix(kRoofSlabPrefix);
		return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
	}

	bool storyHasRoofSlab(Context& context, int storeyId)
	{
		const Model& model = context.model();
		return std::ranges::any_of(context.storyElements(storeyId),
								   [&model](int elementId)
								   {
									   const Entity* element = model.entity(elementId);
									   return element != nullptr && isRoofSlab(*element);
								   });
	}

	bool storyHasRoofSlab(const Model& model, int storeyId)
	{
		Context context(model);
		return storyHasRoofSlab(context, storeyId);
	}

	std::string rafterLabel()
	{
		// 断面・間隔が決め打ちなので全垂木で共通のラベル（"45×45@455"）。整数へ丸めて組み立て
		// る（表示用のラベルなので端数は要らない）。
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

		// interval 以下に割る最小の区間数（1e-9 は「ちょうど整数倍」を切り上げない保険）。
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

	double girderWidthAt(double px, double py, double rdx, double rdy,
						 const std::vector<core::MemberCommand>& members)
	{
		const double rafterLength = std::hypot(rdx, rdy);
		double bestDistance = kGirderSearchTol;
		double bestWidth = kDefaultGirderWidth;
		bool found = false;
		for (const core::MemberCommand& member : members)
		{
			const double mdx = member.end.x - member.start.x;
			const double mdy = member.end.y - member.start.y;
			const double memberLength = std::hypot(mdx, mdy);
			if (memberLength <= 0.0)
				continue;
			const double ux = mdx / memberLength;
			const double uy = mdy / memberLength;

			// 芯線に沿う射影位置 t が区間内か、芯線からの直交距離が許容内かを判定する。
			const double t = ((px - member.start.x) * ux) + ((py - member.start.y) * uy);
			if (t < -kGirderAlongTol || t > memberLength + kGirderAlongTol)
				continue;
			const double perpendicular =
				std::abs((-(px - member.start.x) * uy) + ((py - member.start.y) * ux));
			if (perpendicular > bestDistance)
				continue;
			// 垂木と平行に走る材は軒桁でないため除外する（なす角の sin が小さい）。
			if (rafterLength > 0.0 &&
				std::abs((rdx * uy) - (rdy * ux)) / rafterLength < kGirderPerpSin)
				continue;

			bestDistance = perpendicular;
			bestWidth = member.width;
			found = true;
		}
		return found ? bestWidth : kDefaultGirderWidth;
	}

	std::vector<RafterCommand> raftersForPlane(const RoofPlane& plane, const std::string& layer,
											   double storeyElevation, const Vec2& center,
											   std::optional<double> beamTopZ,
											   const std::vector<core::MemberCommand>& storyMembers)
	{
		// 勾配の座標系（勾配方向 down・掃引方向 along・平面上の天端 Z）は野地板と共有する
		// （parse/IfcGeometry の RoofSlope）。
		//
		// ［共有に伴う挙動差・意図的］ほぼ水平な面（法線の水平成分が極小）を弾くのは従来と
		// 同じだが、**鉛直な面（法線の鉛直成分 nz が極小）も弾くようになった**。従来の垂木は
		// nz を見ずに平面式の分母へ渡していたため、退化した鉛直の屋根版に当たると天端 Z が
		// 発散して無意味な垂木を並べていた。野地板（parse/Roof）は元から nz を弾いており、
		// 屋根面を共有する以上こちらへ揃えるのが正しい（実フィクスチャには該当する屋根版が
		// 無いため、実データでの出力は従来と一致する）。退化とみなす閾値（kRoofFlatTol）も
		// 野地板と共有する——roofSlope の既定値なので明示的に渡さない。
		RoofSlope slope;
		if (!roofSlope(plane, slope))
			return {};

		// 平面外形の XY と、掃引方向への広がり。
		const std::vector<Vec2> plan = RoofSlope::plan(plane);
		double eMin = 0.0;
		double eMax = 0.0;
		RoofSlope::projectionRange(plan, slope.along, eMin, eMax);
		if (eMax - eMin < kMinRafterLength)
			return {}; // 掃引方向の広がりが極小な面（退化した屋根版）

		const double ex = slope.along.x;
		const double ey = slope.along.y;
		const double dx = slope.down.x;
		const double dy = slope.down.y;

		const std::string label = rafterLabel();
		// 高さ基準（StoryBoundCommand）の基準になる垂木レベルの絶対 Z。垂木レベルは横架材
		// 天端（最上階は軒高）に揃えてあるので（parse/Story.cpp の insertAboveBeamTop）、
		// buildRafterCommands が渡す beamTopZ がそのままレベルの Z になる。beamTopZ を
		// 渡さない呼び方（単体テストの直接呼び出し）ではストーリ高さを基準にする。
		const double levelZ = beamTopZ.value_or(storeyElevation);
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

			// d 昇順（同値は x → y で安定）に並べ、[偶, 奇] の対が面内の区間になる。
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
				const double segmentRun = std::hypot(low.x - high.x, low.y - high.y);
				if (segmentRun < kMinRafterLength)
					continue; // 隅木際の極小片・端で退化した区間は配置しない

				// 天端 Z（絶対値）。ストーリ相対の平面式に Elevation を足す。
				const double zTip = slope.zAt(low.x, low.y, storeyElevation);	  // 軒先
				const double zRidge = slope.zAt(high.x, high.y, storeyElevation); // 棟側

				// 支持点 = 屋根面が横架材天端（軒高）Z と交わる点。軒先→棟の線上で
				// z=beamTopZ となる位置 s を採る。
				//
				// **軒桁に乗らない垂木は軒先を高さの基準（軒高）にする。** 軒先が既に
				// beamTopZ 以上（s <= 0）・面全体が軒高より下（s >= 1）・支持点が棟側の端へ
				// 寄り切って部材が残らない（隅棟際の三角形の先端。下記）——これらは受ける
				// 軒桁が無いので、支持点を採らず**軒先そのものを挿入点＝高さの基準**にし、
				// 差し込み・軒の出を 0 にして**長さと高さを実形状に合わせる**
				// （docs/DEV-NOTES.md M6「ローカル確認」の指示）。描画側は start の XY と
				// elevation をパスの始端に、start→end の水平投影長をスパンにするので、これで
				// OIP の長さ・高さが実形状どおりになる（draw/Rafter.cpp）。
				//
				// **s の丸めに頼らない。** 屋根面の棟側の端が軒高ちょうどに来る面では
				// zRidge == beamTopZ となり s は本来ちょうど 1.0 だが、割り算の丸めで 1−ε に
				// なることがある。`s < 1.0` だけで判定すると支持点が棟側の端に重なり、長さが
				// ほぼ 0 の垂木が出てしまう（実測: 区間 850mm に対し支持点→棟側が 3.4e-8mm。
				// core::samePoint から見れば縮退で、validateDocument が Document 全体を弾く＝
				// その IFC が 1 つも描かれない）。そこで s ではなく**支持点→棟側に部材が
				// 残るか**で判定する。
				bool restsOnGirder = false;
				double supportX = low.x;
				double supportY = low.y;
				double supportZ = zTip;
				double supportToTip = 0.0;
				if (beamTopZ.has_value())
				{
					const double dz = zRidge - zTip;
					const double s = (dz > kRoofFlatTol) ? ((*beamTopZ - zTip) / dz) : 0.0;
					if (s > 0.0 && (1.0 - s) * segmentRun >= kMinRafterLength)
					{
						restsOnGirder = true;
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
				// start=軒側（支持点。軒桁に乗らない垂木は軒先そのもの）、end=棟側（高い端）。
				// 座標はセンタリング済み。
				cmd.start = Vec2{supportX - center.x, supportY - center.y};
				cmd.end = Vec2{high.x - center.x, high.y - center.y};
				cmd.elevation = supportZ;
				cmd.endElevation = zRidge;
				// 差し込み（支持点→壁外面）＝支持点の真下にある軒桁の桁幅の半分。受ける
				// 軒桁が見つからなければ既定桁幅（M6 の挙動と同じ値）。**軒桁に乗らない
				// 垂木は 0**——差し込む相手が無く、描画側は軒先を「支持点＋差し込み＋軒の出」に
				// 置くので、0 でなければ実形状より長く描かれてしまう。
				const double embedment =
					restsOnGirder ? girderWidthAt(cmd.start.x, cmd.start.y, cmd.end.x - cmd.start.x,
												  cmd.end.y - cmd.start.y, storyMembers) /
										2.0
								  : 0.0;
				// 壁外面から軒先の距離（overhang）＝ 支持点→軒先（supportToTip）から支持部分の
				// 差し込み（embedment ＝ 支持点→壁外面）を引いた残り。描画側は軒先を
				// 支持点＋差し込み＋軒の出 に置く（core::rafterEaveEnd）ため、両者の和が
				// supportToTip になるようにする
				// （軒桁に乗らない垂木は supportToTip も embedment も 0 なので軒の出も 0）。
				cmd.overhang = std::max(0.0, supportToTip - embedment);
				cmd.embedment = embedment;
				cmd.label = label;
				// 高さ基準は配置先レイヤ（"n-垂木"）の垂木レベル。offset はそのレベルの絶対 Z
				// から下面 Z までの距離で、支持点は横架材天端（軒高）ちょうどなのでふつう 0、
				// 棟側は勾配ぶんの正の値になる（軒桁に乗らない垂木は支持点側も 0 でない）。
				// 垂木レベルの Z は横架材天端（最上階は軒高）に揃えてあり、それが
				// buildRafterCommands の渡す beamTopZ そのもの（parse/Story.cpp の
				// insertAboveBeamTop）。
				cmd.startBound = StoryBoundCommand{0, kLevelTaruki, supportZ - levelZ};
				cmd.endBound = StoryBoundCommand{0, kLevelTaruki, zRidge - levelZ};
				commands.push_back(std::move(cmd));
			}
		}
		return commands;
	}

	std::vector<RafterCommand> buildRafterCommands(Context& context,
												   const std::vector<core::MemberCommand>& members)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo> stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();

		std::vector<RafterCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string layer = storyLayerName(i, story.isTop, kLevelTaruki);
			// 支持点が乗る横架材天端の絶対 Z（最上階は軒高＝オフセット 0）。
			const double beamTopZ =
				story.isTop ? story.elevation : story.elevation + story.beamOffset;
			// 桁幅の参照先は同じ階の横架材だけ（レイヤ接頭辞 "{n}-" で絞る）。
			const std::string layerPrefix = storyLayerPrefix(i, story.isTop) + "-";
			std::vector<core::MemberCommand> storyMembers;
			for (const core::MemberCommand& member : members)
			{
				if (member.layer.compare(0, layerPrefix.size(), layerPrefix) == 0)
					storyMembers.push_back(member);
			}

			for (const int elementId : context.storyElements(story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isRoofSlab(*element))
					continue;

				// 屋根面は野地板（parse/Roof）と共有する（コンテキストが 1 度だけ解決する）。
				const RoofPlane* plane = context.roofPlane(elementId);
				if (plane == nullptr)
					continue; // 屋根面を解決できない屋根版はスキップ

				std::vector<RafterCommand> rafters =
					raftersForPlane(*plane, layer, story.elevation, center, beamTopZ, storyMembers);
				for (RafterCommand& rafter : rafters)
					commands.push_back(std::move(rafter));
			}
		}
		return commands;
	}

	std::vector<RafterCommand> buildRafterCommands(Context& context)
	{
		return buildRafterCommands(context, context.members());
	}

	std::vector<RafterCommand> buildRafterCommands(const Model& model)
	{
		Context context(model);
		return buildRafterCommands(context);
	}
} // namespace HomeskzIfcImport::parse
