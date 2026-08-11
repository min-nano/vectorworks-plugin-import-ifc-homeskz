//
//	parse/Column.cpp
//
//	柱解析の実装。Python 版 ifc/column.py の build_column_commands ほかに対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/IfcAttr.h"
#include "parse/Member.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::ColumnCommand;
	using core::MemberCommand;
	using core::StoryBoundCommand;
	using core::Vec2;

	namespace
	{
		// 小屋束の断面幅を合わせる対象＝小屋組の上端材（Python 版
		// _KOYAZUKA_TOP_MEMBER_CLASSES）。母屋・棟木・登り梁だけを見る（軒桁等は対象外）。
		bool isKoyazukaTopMemberClass(const std::string& memberClass)
		{
			return memberClass == CLASS_MOYA || memberClass == CLASS_MUNAGI ||
				   memberClass == CLASS_NOBORIBARI;
		}

		// 金物と柱を対応付ける XY 位置キー（Python 版 _position_key の round(…, 3) 相当）。
		// 0.001mm 単位の整数へ丸めるので、浮動小数の最下位ビット差で照合が外れない。
		using PositionKey = std::pair<long long, long long>;

		PositionKey positionKey(double x, double y)
		{
			return PositionKey{std::llround(x * 1000.0), std::llround(y * 1000.0)};
		}

		// 階に属する柱頭・柱脚金物を XY 位置で索引する（Python 版 _collect_column_hardware）。
		// 柱頭・柱脚金物は柱と同じストーリに含まれ、柱と同じ平面座標へ立方体として置かれる。
		struct ColumnHardware
		{
			std::map<PositionKey, std::string> heads; // 柱頭金物
			std::map<PositionKey, std::string> bases; // 柱脚金物
		};

		ColumnHardware collectColumnHardware(Context& context, int storeyId)
		{
			const Model& model = context.model();
			ColumnHardware hardware;
			for (const int elementId : context.storyElements(storeyId))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || element->type != "IFCMECHANICALFASTENER")
					continue;

				const std::string name = entityName(*element);
				std::map<PositionKey, std::string>* target = nullptr;
				if (name.find(kHardwareTopKeyword) != std::string::npos)
					target = &hardware.heads;
				else if (name.find(kHardwareBottomKeyword) != std::string::npos)
					target = &hardware.bases;
				else
					continue;

				Vec2 position;
				if (!columnPosition2D(model, *element, position))
					continue;
				const std::string spec = columnHardwareSpec(fastenerTypeName(model, *element));
				if (spec.empty())
					continue;
				// 同じ位置に複数あれば**先に見つけたものを残す**（Python 版 setdefault。
				// storyElements の並びは決定的なので結果も決定的）。
				target->emplace(positionKey(position.x, position.y), spec);
			}
			return hardware;
		}

		// 索引から仕様を引く（無ければ空文字）。
		std::string hardwareAt(const std::map<PositionKey, std::string>& index,
							   const PositionKey& key)
		{
			const auto found = index.find(key);
			return found == index.end() ? std::string() : found->second;
		}
	} // namespace

	// 型は IfcRelDefinesByType 経由で辿る（Python 版は逆方向属性名がスキーマで異なるため
	// IsTypedBy / IsDefinedBy の両方を走査するが、こちらは逆参照を辿るので 1 度で済む）。
	std::string fastenerTypeName(const Model& model, const Entity& fastener)
	{
		for (const int relId : model.referrers(fastener.id))
		{
			const Entity* rel = model.entity(relId);
			if (rel == nullptr || rel->type != "IFCRELDEFINESBYTYPE")
				continue;
			const Value& related = rel->attribute(attr::kRelDefinesRelatedObjects);
			if (!related.isList())
				continue;
			const bool definesThis =
				std::ranges::any_of(related.items, [&fastener](const Value& ref)
									{ return ref.reference == fastener.id; });
			if (!definesThis)
				continue;

			const Entity* type = model.resolve(rel->attribute(attr::kRelDefinesByTypeRelatingType));
			if (type == nullptr)
				continue;
			return entityName(*type);
		}
		return {};
	}

	bool isColumnElement(const Entity& element)
	{
		return element.type == "IFCCOLUMN";
	}

	std::string resolveColumnType(const std::string& objectType)
	{
		return objectType == kStandColumnObjectType ? kColumnTypeKoyazuka : kColumnTypeDefault;
	}

	std::string columnHardwareSpec(const std::string& typeName)
	{
		return typeName;
	}

	std::string makeColumnMemberId(double width, double depth, const std::string& columnType,
								   const std::string& topHardware,
								   const std::string& bottomHardware)
	{
		std::string memberId = std::to_string(std::llround(width)) + "×" +
							   std::to_string(std::llround(depth)) + " - " + columnType;
		for (const std::string& hardware : {topHardware, bottomHardware})
		{
			if (!hardware.empty())
				memberId += " / " + hardware;
		}
		return memberId;
	}

	bool columnPosition2D(const Model& model, const Entity& element, Vec2& out)
	{
		const Entity* placement = model.resolve(element.attribute(attr::kProductObjectPlacement));
		if (placement == nullptr || placement->type != "IFCLOCALPLACEMENT")
			return false;
		const Entity* axis =
			model.resolve(placement->attribute(attr::kLocalPlacementRelativePlacement));
		if (axis == nullptr || axis->type != "IFCAXIS2PLACEMENT3D")
			return false;
		const Entity* point = model.resolve(axis->attribute(attr::kAxis2PlacementLocation));
		if (point == nullptr)
			return false;
		const Value& coords = point->attribute(attr::kCartesianPointCoordinates);
		if (!coords.isList() || coords.items.size() < 2)
			return false;
		out = Vec2{coords.items[0].asReal(), coords.items[1].asReal()};
		return true;
	}

	std::optional<double> memberWidthOnTop(double px, double py, double topAbs,
										   const std::vector<MemberCommand>& members)
	{
		// 最良候補のキー（|材下端 − 小屋束上端|, 直交距離, 幅）を辞書順で比べる（Python 版と
		// 同じ順序）。同点は幅の小さい方＝入力順に依存しない決定的な選択になる。
		bool found = false;
		double bestGap = 0.0;
		double bestPerp = 0.0;
		double bestWidth = 0.0;

		for (const MemberCommand& member : members)
		{
			if (!isKoyazukaTopMemberClass(member.drawClass))
				continue;
			const double dx = member.end.x - member.start.x;
			const double dy = member.end.y - member.start.y;
			const double length = std::hypot(dx, dy);
			if (length <= 0.0)
				continue;

			const double ux = dx / length;
			const double uy = dy / length;
			const double along = ((px - member.start.x) * ux) + ((py - member.start.y) * uy);
			if (along < -kKoyazukaMatchAlongTol || along > length + kKoyazukaMatchAlongTol)
				continue;
			const double perp =
				std::abs(((px - member.start.x) * -uy) + ((py - member.start.y) * ux));
			if (perp > (member.width / 2.0) + kKoyazukaMatchPerpTol)
				continue;

			// 傾斜梁（登り梁）は天端 Z が軸方向に変化するため、小屋束位置の比で補間する。
			const double fraction = std::min(1.0, std::max(0.0, along / length));
			const double memberTop =
				member.elevation + (fraction * (member.endElevation - member.elevation));
			const double memberBottom = memberTop - member.height;
			if (topAbs < memberBottom - kKoyazukaMatchZTol ||
				topAbs > memberTop + kKoyazukaMatchZTol)
				continue;

			const double gap = std::abs(memberBottom - topAbs);
			const bool better =
				!found || gap < bestGap ||
				(gap == bestGap &&
				 (perp < bestPerp || (perp == bestPerp && member.width < bestWidth)));
			if (better)
			{
				found = true;
				bestGap = gap;
				bestPerp = perp;
				bestWidth = member.width;
			}
		}
		if (!found)
			return std::nullopt;
		return bestWidth;
	}

	bool isThroughColumn(double topAbs, const std::optional<double>& nextFloorElevation)
	{
		if (!nextFloorElevation.has_value())
			return false;
		return topAbs > *nextFloorElevation + kThroughColumnTol;
	}

	double resolveColumnToLevel(int baseIndex, double topAbs,
								const std::vector<double>& beamBottoms,
								const std::vector<double>& beamTops)
	{
		// 到達した最上階（0 起点）。初期値は自階＝どの上階にも未到達。
		int reached = baseIndex;
		for (std::size_t s = static_cast<std::size_t>(baseIndex) + 1; s < beamBottoms.size(); ++s)
		{
			if (topAbs < beamBottoms[s] - kSpanLevelTol)
				break;
			reached = static_cast<int>(s);
		}

		const auto reachedIndex = static_cast<std::size_t>(reached);
		const bool aboveReachedTop =
			reachedIndex < beamTops.size() && topAbs > beamTops[reachedIndex] + kSpanLevelTol;
		if (reached == baseIndex || aboveReachedTop)
		{
			// 直上階の横架材にも達しない、または到達階の横架材天端（軒高）を超えて突き出す
			// 屋根束（小屋束等）→ 到達階 +0.5 の半整数レベル。
			return static_cast<double>(reached + 1) + 0.5;
		}
		return static_cast<double>(reached + 1);
	}

	std::vector<ColumnSpan> collectColumnSpans(const std::vector<ColumnCommand>& columns)
	{
		// レイヤ名で重複を除いてから (from, to) 昇順に並べる。map なのでレイヤ名順に
		// 走査され、命令の並び順に依存しない決定的な結果になる。
		std::map<std::string, std::pair<double, double>> seen;
		for (const ColumnCommand& column : columns)
		{
			double from = 0.0;
			double to = 0.0;
			if (parseSpanLayer(column.layer, from, to))
				seen.emplace(column.layer, std::pair<double, double>{from, to});
		}

		std::vector<ColumnSpan> spans;
		spans.reserve(seen.size());
		for (const auto& [layer, range] : seen)
			spans.push_back(ColumnSpan{range.first, range.second, layer});
		std::stable_sort(spans.begin(), spans.end(), [](const ColumnSpan& a, const ColumnSpan& b)
						 { return a.from != b.from ? a.from < b.from : a.to < b.to; });
		return spans;
	}

	std::map<int, std::vector<std::string>>
	collectColumnLayersByStory(const std::vector<ColumnCommand>& columns)
	{
		std::map<int, std::vector<std::string>> result;
		for (const ColumnSpan& span : collectColumnSpans(columns))
			result[static_cast<int>(span.from) - 1].push_back(span.layer);
		return result;
	}

	std::vector<ColumnCommand> buildColumnCommands(Context& context,
												   const std::vector<MemberCommand>& members)
	{
		const Model& model = context.model();
		const std::vector<StoryInfo> stories = context.stories();
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();
		const auto topIndex = static_cast<int>(stories.size()) - 1;

		// 各階の横架材天端（最上階は軒高）の絶対 Z。柱の上下端をこの高さへバインドする。
		std::vector<double> beamTopAbs;
		beamTopAbs.reserve(stories.size());
		for (const StoryInfo& story : stories)
			beamTopAbs.push_back(story.isTop ? story.elevation
											 : story.elevation + story.beamOffset);

		// 各階の横架材（床梁）下端の最小値。span の to レベル判定の境界に使う（母屋・登り梁の
		// 専用レイヤは含めず、横架材天端／軒高レイヤの床梁だけを見る）。梁が無い階は天端で代用。
		std::vector<double> beamBottoms;
		beamBottoms.reserve(stories.size());
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const std::string beamLayer =
				storyLayerName(i, stories[i].isTop, stories[i].isTop ? kLevelEaves : kLevelBeamTop);
			bool found = false;
			double lowest = 0.0;
			for (const MemberCommand& member : members)
			{
				if (member.layer != beamLayer)
					continue;
				const double bottom =
					std::min(member.elevation, member.endElevation) - member.height;
				if (!found || bottom < lowest)
					lowest = bottom;
				found = true;
			}
			beamBottoms.push_back(found ? lowest : beamTopAbs[i]);
		}

		std::vector<ColumnCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			// 柱頭・柱脚金物を XY 位置で索引し、各柱と対応付ける。
			const ColumnHardware hardware = collectColumnHardware(context, story.id);

			for (const int elementId : context.storyElements(story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isColumnElement(*element))
					continue;

				Vec2 position;
				if (!columnPosition2D(model, *element, position))
					continue;
				// 断面の抽出は横架材と同じ関門を通す（parse/Member の memberProfileDims）。
				// 柱は XDim=幅・YDim=成・Depth=柱高さ。
				MemberProfile profile;
				if (!memberProfileDims(model, *element, profile))
					continue;

				double width = profile.width;
				double depth = profile.height;
				const double height = profile.length;

				// 柱頭・柱脚金物を同一平面座標から対応付ける（無ければ空文字）。
				const PositionKey key = positionKey(position.x, position.y);
				const std::string topHardware = hardwareAt(hardware.heads, key);
				const std::string bottomHardware = hardwareAt(hardware.bases, key);

				double localZ = 0.0;
				getLocalPlacementZ(model, *element, localZ);
				const double bottomAbs = story.elevation + localZ;
				const double topAbs = bottomAbs + height;

				const double px = position.x - center.x;
				const double py = position.y - center.y;

				// span（またぐレベル区間）ごとの専用レイヤに配置する。
				const double toLevel =
					resolveColumnToLevel(static_cast<int>(i), topAbs, beamBottoms, beamTopAbs);

				// クラスは小屋束（IFC 記録）／通し柱・管柱（上下端の高さ）で判別する。
				const std::optional<double> nextFloorElevation =
					story.isTop ? std::nullopt : std::optional<double>(stories[i + 1].elevation);
				const bool through = isThroughColumn(topAbs, nextFloorElevation);
				const std::string objectType = entityString(*element, attr::kProductObjectType);
				const std::string columnClass = resolveColumnClass(
					objectType, entityName(*element), static_cast<int>(i), topIndex, through);

				// 小屋束は構造用途を小屋束にする（柱用途だと VW の柱高さモデルで上端高さが
				// 崩れる。ヘッダ冒頭参照）。
				const bool isKoyazuka = columnClass == CLASS_KOYAZUKA;

				// ホームズ君 IFC の小屋束の断面寸法は適当な値なので、直上に乗る横架材
				// （母屋・棟木・登り梁）の断面幅に合わせた正方形へ置き換える。上に乗る材が
				// 見つからない小屋束は IFC の断面のまま。構造材 ID も補正後の寸法で作る。
				if (isKoyazuka)
				{
					const std::optional<double> onTop = memberWidthOnTop(px, py, topAbs, members);
					if (onTop.has_value())
					{
						width = *onTop;
						depth = *onTop;
					}
				}

				// 上下端高さを横架材天端（最上階は軒高）のストーリレベルへバインドする。
				// offset はバインド先レベルの絶対 Z から実際の下端／上端 Z までの距離。
				// **実体の高さはパス（下端 → 上端の鉛直線）が担い**、バインドは高さの基準を
				// 与える（ヘッダ冒頭「高さはパスのジオメトリ…」）。
				const char* currentLevel = story.isTop ? kLevelEaves : kLevelBeamTop;
				const double bottomOffset = bottomAbs - beamTopAbs[i];

				ColumnCommand cmd;
				cmd.layer = spanLayerName(static_cast<double>(i + 1), toLevel);
				cmd.memberId = makeColumnMemberId(width, depth, resolveColumnType(objectType),
												  topHardware, bottomHardware);
				cmd.drawClass = columnClass;
				cmd.structuralUse = isKoyazuka ? kStructuralUseKoyazuka : kStructuralUseColumn;
				cmd.position = Vec2{px, py};
				cmd.width = width;
				cmd.depth = depth;
				cmd.height = height;
				cmd.elevation = bottomAbs;
				cmd.topHardware = topHardware;
				cmd.bottomHardware = bottomHardware;
				cmd.bottomBound = StoryBoundCommand{0, currentLevel, bottomOffset};
				if (isKoyazuka || story.isTop)
				{
					// 小屋束（および上階の無い最上階の柱）は上下端とも当階の横架材天端
					// （最上階は軒高）へバインドし、**上端 offset には実際の上端 Z までの
					// 距離**（＝下端 offset ＋ 柱高さ）を入れる。バウンドの差が柱高さに
					// なるので、管柱・通し柱と同じ形になる（ヘッダ冒頭「高さは…」）。
					cmd.topBound = StoryBoundCommand{0, currentLevel, topAbs - beamTopAbs[i]};
				}
				else
				{
					// 柱（管柱・通し柱）は上端を上階（次階）の横架材天端へバインドする。
					const bool nextIsTop = (i + 1 == static_cast<std::size_t>(topIndex));
					const char* nextLevel = nextIsTop ? kLevelEaves : kLevelBeamTop;
					cmd.topBound = StoryBoundCommand{1, nextLevel, topAbs - beamTopAbs[i + 1]};
				}
				commands.push_back(std::move(cmd));
			}
		}
		return commands;
	}

	std::vector<ColumnCommand> buildColumnCommands(Context& context)
	{
		return buildColumnCommands(context, context.members());
	}

	std::vector<ColumnCommand> buildColumnCommands(const Model& model,
												   const std::vector<MemberCommand>& members)
	{
		Context context(model);
		return buildColumnCommands(context, members);
	}

	std::vector<ColumnCommand> buildColumnCommands(const Model& model)
	{
		Context context(model);
		return buildColumnCommands(context);
	}
} // namespace HomeskzIfcImport::parse
