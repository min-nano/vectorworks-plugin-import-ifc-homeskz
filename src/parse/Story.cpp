//
//	parse/Story.cpp
//
//	ストーリ解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse
//	のみ依存）。
//

#include "parse/Story.h"
#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/Floor.h"
#include "parse/IfcAttr.h"
#include "parse/IfcGeometry.h"
#include "parse/Member.h"
#include "parse/PlanLevel.h"
#include "parse/Rafter.h"
#include "parse/ShearWall.h"
#include "parse/Roof.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
#include <ranges>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::LevelCommand;
	using core::StoryCommand;

	namespace
	{
		// 最上階のストーリ名。
		constexpr const char* kStoryRoof = "屋根";
		// 最上階のストーリ接尾辞・レイヤ接頭辞（Roof）。
		constexpr const char* kRoofSuffix = "R";

		// 1 バイトずつ std::toupper を掛けた文字列を返す（"FL" 判定は ASCII なのでこれで
		// 十分。C ロケールでは 0x80 以上のバイトは変換されないため、UTF-8 の日本語部分＝
		// マルチバイト列はそのまま残る）。
		std::string asciiUpper(const std::string& s)
		{
			std::string out = s;
			for (char& c : out)
			{
				const auto uc = static_cast<unsigned char>(c);
				c = static_cast<char>(std::toupper(uc));
			}
			return out;
		}

		// 名前が（大文字化して）"FL" で終わるか。
		bool nameEndsWithFL(const std::string& name)
		{
			if (name.size() < 2)
				return false;
			const std::string upper = asciiUpper(name);
			return upper.compare(upper.size() - 2, 2, "FL") == 0;
		}

		// index（0 始まり）と最上階フラグから VectorWorks のストーリ名を返す。最上階は "屋根"、
		// それ以外は "{index+1}階"。
		std::string storyNameFor(std::size_t index, bool isTop)
		{
			if (isTop)
				return kStoryRoof;
			return std::to_string(index + 1) + "階";
		}

		// 横架材命令のどれかが layer を配置先に指しているか。母屋・登り梁レベルを足すかの判定
		// に使う（buildStoryCommands の「レベルを足す条件」参照）。
		bool anyMemberOnLayer(const std::vector<core::MemberCommand>& members,
							  const std::string& layer)
		{
			return std::ranges::any_of(members, [&layer](const core::MemberCommand& member)
									   { return member.layer == layer; });
		}

		// 文字列全体が実数として読めれば outValue に入れて true（"1" / "2.5"）。末尾に
		// 余りがある・空文字・数値でないなら false（parseSpanLayer の関門）。
		bool parseNumber(const std::string& text, double& outValue)
		{
			if (text.empty())
				return false;
			try
			{
				std::size_t consumed = 0;
				const double value = std::stod(text, &consumed);
				if (consumed != text.size())
					return false;
				outValue = value;
				return true;
			}
			catch (...)
			{
				// std::stod は数値でない／範囲外で例外を投げる。span レイヤでないだけなので
				// 呼び出し側へは false で返す（例外はここに閉じ込める。CLAUDE.md「例外は
				// parse 内部の局所処理に留める」）。
				return false;
			}
		}
	} // namespace

	// span レベルは resolveColumnToLevel が返す整数／半整数だけなので、一般の実数書式
	// （std::to_string の 6 桁固定小数）は使わない。
	std::string formatSpanLevel(double value)
	{
		const double rounded = std::floor(value);
		const auto whole = static_cast<long long>(rounded);
		if (value == rounded)
			return std::to_string(whole);
		return std::to_string(whole) + ".5";
	}

	std::string spanLayerName(double fromLevel, double toLevel)
	{
		return formatSpanLevel(fromLevel) + "to" + formatSpanLevel(toLevel) + "-" +
			   kColumnLayerSuffix;
	}

	bool parseSpanLayer(const std::string& name, double& outFrom, double& outTo)
	{
		const std::string suffix = std::string("-") + kColumnLayerSuffix;
		if (name.size() <= suffix.size() ||
			name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
			return false;
		const std::string core = name.substr(0, name.size() - suffix.size());
		const std::size_t separator = core.find("to");
		if (separator == std::string::npos)
			return false;
		// "to" が 2 つ以上あるレイヤ名（"1to2to3-柱"）は分解できないので span でないとする。
		if (core.find("to", separator + 2) != std::string::npos)
			return false;

		double from = 0.0;
		double to = 0.0;
		if (!parseNumber(core.substr(0, separator), from) ||
			!parseNumber(core.substr(separator + 2), to))
			return false;
		outFrom = from;
		outTo = to;
		return true;
	}

	std::string storyLayerPrefix(std::size_t index, bool isTop)
	{
		// CreateStory の接尾辞（＝レイヤ接頭辞）。最上階は "R"、それ以外は "{index+1}"。
		// 空文字は 2 回目以降の CreateStory が失敗するため必ず非空。
		if (isTop)
			return kRoofSuffix;
		return std::to_string(index + 1);
	}

	std::string storyLayerName(std::size_t index, bool isTop, const std::string& levelType)
	{
		return storyLayerPrefix(index, isTop) + "-" + levelType;
	}

	std::string beamGroupLayer(const std::string& layer)
	{
		// 伏図レベルの印（"(FL-872)"）は残す（同じ伏図レベルの横架材レイヤへ読み替える）。
		const std::string base = core::stripPlanLevelTag(layer);
		const std::string suffix = std::string("-") + kLevelNokigeta;
		if (!base.ends_with(suffix))
			return layer;
		const std::string prefix = base.substr(0, base.size() - suffix.size());
		return prefix + "-" + (prefix == kRoofSuffix ? kLevelEaves : kLevelBeamTop) +
			   layer.substr(base.size());
	}

	bool getLocalPlacementZ(const Model& model, const Entity& element, double& outZ)
	{
		// ローカル配置原点の Z（鎖の解決は parse/IfcGeometry の resolveLocalPlacementOrigin。
		// 親 PlacementRelTo は辿らない＝ M2 と同じ規約）。Z を持たない 2D 点は false。
		LocalOrigin origin;
		if (!resolveLocalPlacementOrigin(model, element, origin) || !origin.hasZ)
			return false;
		outZ = origin.z;
		return true;
	}

	bool storyHasElement(Context& context, int storeyId, bool (*pred)(const Entity&))
	{
		const Model& model = context.model();
		return std::ranges::any_of(context.storyElements(storeyId),
								   [&model, pred](int elementId)
								   {
									   const Entity* element = model.entity(elementId);
									   return element != nullptr && pred(*element);
								   });
	}

	std::vector<int> collectStoryElements(const Model& model, int storeyId)
	{
		// storey を RelatingStructure に持つ IfcRelContainedInSpatialStructure を、
		// 逆参照（referrers）から辿る（同じ逆関係）。誰からも参照されていない階（要素を
		// 1 つも持たない階）は即座に空を返す。
		if (model.referrers(storeyId).empty())
			return {};

		std::vector<int> elements;
		for (const int relId : model.referrers(storeyId))
		{
			const Entity* rel = model.entity(relId);
			if (rel == nullptr || rel->type != "IFCRELCONTAINEDINSPATIALSTRUCTURE")
				continue;
			// この rel の RelatingStructure が当該 storey であることを確認する（storey が
			// RelatedElements 側に現れる別の rel を巻き込まないため）。
			if (rel->attribute(attr::kRelContainedRelatingStructure).reference != storeyId)
				continue;

			const Value& related = rel->attribute(attr::kRelContainedRelatedElements);
			if (!related.isList())
				continue;
			for (const Value& ref : related.items)
			{
				if (model.resolve(ref) != nullptr)
					elements.push_back(ref.reference);
			}
		}
		return elements;
	}

	double resolveBeamTopOffset(Context& context, int storeyId)
	{
		// 階に属する IfcColumn / IfcSlab のローカル Z 負値の最大を採る（最初に見つかった
		// 値ではなく最大値なので、列挙順に依存しない決定的な結果になる）。
		const Model& model = context.model();
		double best = 0.0;
		bool found = false;
		for (const int elementId : context.storyElements(storeyId))
		{
			const Entity* element = model.entity(elementId);
			if (element == nullptr)
				continue;
			if (element->type != "IFCCOLUMN" && element->type != "IFCSLAB")
				continue;
			double z = 0.0;
			if (getLocalPlacementZ(model, *element, z) && z < 0.0)
			{
				if (!found || z > best)
					best = z;
				found = true;
			}
		}
		return best; // 候補が無ければ 0.0
	}

	double resolveBeamTopOffset(const Model& model, int storeyId)
	{
		Context context(model);
		return resolveBeamTopOffset(context, storeyId);
	}

	std::vector<StoryInfo> collectStories(Context& context)
	{
		// 名前が "FL" で終わる IfcBuildingStorey だけを対象にする。
		const Model& model = context.model();
		std::vector<StoryInfo> stories;
		for (const int id : model.byType("IFCBUILDINGSTOREY"))
		{
			const Entity* storey = model.entity(id);
			if (storey == nullptr)
				continue;
			if (!nameEndsWithFL(entityName(*storey)))
				continue;
			StoryInfo info;
			info.id = id;
			info.elevation = storey->attribute(attr::kBuildingStoreyElevation).asReal();
			info.name = entityName(*storey);
			stories.push_back(info);
		}

		if (stories.empty())
			return {};

		// Elevation 昇順に安定ソート（同値は byType 由来の #id 昇順を保つ＝決定的）。
		std::stable_sort(stories.begin(), stories.end(), [](const StoryInfo& a, const StoryInfo& b)
						 { return a.elevation < b.elevation; });

		// 末尾（Elevation 最大）を最上階とする。beamOffset は最上階以外にだけ求める
		// （最上階は軒高のみで横架材天端オフセットを使わない）。
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const bool isTop = (i + 1 == stories.size());
			stories[i].isTop = isTop;
			if (!isTop)
				stories[i].beamOffset = resolveBeamTopOffset(context, stories[i].id);
		}
		return stories;
	}

	std::vector<StoryInfo> collectStories(const Model& model)
	{
		Context context(model);
		return collectStories(context);
	}

	std::vector<StoryCommand> buildStoryCommands(Context& context)
	{
		const std::vector<StoryInfo>& stories = context.stories();
		// 母屋・登り梁レベルの有無は、実際に組み立てた横架材命令の配置先レイヤから決める（下
		// 記「レベルを足す条件」）。コンテキストが 1 度だけ解析するので、垂木・登り梁の補正と
		// 同じ結果を共有する。
		const std::vector<core::MemberCommand>& members = context.members();
		// span 柱レイヤ（"{from}to{to}-柱"）は実在する柱から決まる。コンテキストが柱命令を
		// 1 度だけ組み立てるので、ここと Document の columns は同じ結果を共有する
		// （parse/Context.h の columns）。
		const std::map<int, std::vector<std::string>> columnLayers =
			collectColumnLayersByStory(context.columns(), context.planLevels());
		// 横架材の高さごとの伏図（parse/PlanLevel）。標準でない伏図レベルは、元のレベルを
		// その高さのぶんずらした別のレベル（＝レイヤ）を持つ（下の「伏図レベルのレベル」）。
		const std::vector<PlanLevel>& planLevels = context.planLevels();
		const std::vector<core::FloorCommand>& floors = context.floors();

		std::vector<StoryCommand> commands;
		commands.reserve(stories.size());
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& info = stories[i];

			StoryCommand cmd;
			cmd.name = storyNameFor(i, info.isTop);
			cmd.suffix = storyLayerPrefix(i, info.isTop);
			cmd.elevation = info.elevation;

			// レイヤ名は要素側の配置先探索と同じ規約で組み立てる（parse/Story storyLayerName）。
			const auto layerFor = [i, &info](const char* levelType)
			{ return storyLayerName(i, info.isTop, levelType); };

			// 基本レベル（M3）＋屋根組の垂木・野地板（M6）。登り梁・母屋・span 柱は後続 M で
			// 追加する（ヘッダ参照）。levels の並び順は希望するデザインレイヤのスタック順（上→下）。
			if (info.isTop)
			{
				// 最上階（屋根）は軒高（オフセット 0）。ロフト（小屋裏収納）の床があるときだ
				// け、その標準床レベル FL（軒高 + kLoftFloorLevelOffset）を足す（床の無い屋根
				// に空の FL レイヤを作らない）。この FL がロフト床の配置先レイヤ "R-FL"
				// になる。ロフトの床は床版（IfcSlab）でも床梁から合成した領域でもよい
				// （parse/Floor の storyHasLoftFloor）。
				if (storyHasLoftFloor(context, info.id))
				{
					cmd.levels.push_back(
						LevelCommand{kLevelFL, kLoftFloorLevelOffset, layerFor(kLevelFL)});
				}
				cmd.levels.push_back(LevelCommand{kLevelEaves, 0.0, layerFor(kLevelEaves)});
			}
			else
			{
				// 一般階は FL（0）＋横架材天端（負オフセット）。FL を上段に積む。
				cmd.levels.push_back(LevelCommand{kLevelFL, 0.0, layerFor(kLevelFL)});
				cmd.levels.push_back(
					LevelCommand{kLevelBeamTop, info.beamOffset, layerFor(kLevelBeamTop)});
			}

			// 小屋組のレベル（登り梁・母屋・垂木・野地板）は、横架材天端（最上階は軒高）
			// レベルの**直前**へ順に挿入して積み上げる。挿入位置は「最初の挿入前の横架材天端
			// レベルの索引」で固定し、そこへ挿し続けることで**後から挿入したものが 1 段上**に
			// 来る（＝スタックは 横架材天端/軒高 ← 登り梁 ← 母屋 ← 垂木 ← 野地板）。高さは
			// いずれも横架材天端（最上階は軒高）に揃える（実描画の Z は各材／屋根版由来の
			// 絶対値を要素自身が持つため、このオフセットには依存しない）。
			const double upperOffset = info.isTop ? 0.0 : info.beamOffset;
			const auto beamTopIndex = static_cast<std::ptrdiff_t>(cmd.levels.size()) - 1;
			const auto insertAboveBeamTop =
				[&cmd, &i, &info, beamTopIndex](const std::string& levelType, double offset)
			{
				cmd.levels.insert(
					cmd.levels.begin() + beamTopIndex,
					LevelCommand{levelType, offset, storyLayerName(i, info.isTop, levelType)});
			};
			const std::vector<const PlanLevel*> storyLevels = storyPlanLevels(planLevels, i);

			// M7 横架材: 母屋・棟木（"n-母屋"）と登り梁（"n-登り梁"）は、梁（小屋梁・軒桁）と
			// 重なって見にくいため専用レイヤへ分離する（parse/Member）。そのレイヤはここで作る。
			// スタックは 横架材天端/軒高 ← 登り梁 ← 母屋 なので、登り梁 → 母屋 の順に挿入する。
			//
			// ［レベルを足す条件］**実際に組み立てた横架材命令の配置先レイヤ**で判定する
			// （IFC の名前で「母屋がある階か」を見ない）。理由は 2 つ:
			//   * 名前判定は「名前では判別できないが高さで母屋と推定された材」（隅木谷木等）を
			//     取りこぼし、その材だけ置き場所を失う。
			//   * 名前で拾えない材を救おうと最上階へ無条件に足すと、母屋を持たない最上階に
			//     空レイヤが残る（空レイヤを作らない方針。ロフト FL・垂木/野地板と同じ）。
			// 命令の配置先で判定すれば、**レイヤは命令があるときだけ・命令があれば必ず**でき、
			// 両方の齟齬が構造的に起きない。
			// 軒桁: 母屋伏図に軒桁だけを薄く重ねるため、横架材天端（最上階は軒高）から分けた
			// "n-軒桁" レイヤ（parse/PlanLevel）。高さは横架材天端と同じなので、そのすぐ上へ
			// **最初に**挿す（耐力壁・小屋組より下）。条件は母屋・登り梁と同じく命令の配置先で、
			// 登り梁と同じく伏図レベルごとに確かめる（その高さのぶんずらす）。
			if (storyLevels.empty())
			{
				if (anyMemberOnLayer(members, layerFor(kLevelNokigeta)))
					insertAboveBeamTop(kLevelNokigeta, upperOffset);
			}
			for (const PlanLevel* level : storyLevels)
			{
				if (anyMemberOnLayer(members, planLevelLayer(*level, info, kLevelNokigeta)))
					insertAboveBeamTop(planLevelType(*level, kLevelNokigeta),
									   upperOffset + planLevelShift(*level, info));
			}

			// M19 耐力壁: 筋かい・面材の PIO を載せる "n-耐力壁" レイヤ。**横架材天端の
			// すぐ上**（小屋組より下）へ置く。挿入は「後から挿したものが 1 段上」なので、
			// 小屋組（登り梁・母屋・垂木・野地板）より**先に**挿す。
			//
			// ［レベルを足す条件］母屋・登り梁と同じく**実際に組み立てた耐力壁命令の配置先
			// レイヤ**で判定する（命令があるときだけ・命令があれば必ずレイヤができる）。
			//
			// 耐力壁は立つ天端の伏図レベルのレイヤへ置かれる（parse/ShearWall）ので、伏図
			// レベルごとに確かめ、そのレベルの高さのぶんずらす（伏図レベルが無い階は従来の
			// 1 つだけ）。
			if (storyLevels.empty())
			{
				if (anyShearWallOnLayer(context.shearWalls(), layerFor(kLevelShearWall)))
					insertAboveBeamTop(kLevelShearWall, upperOffset);
			}
			for (const PlanLevel* level : storyLevels)
			{
				if (anyShearWallOnLayer(context.shearWalls(),
										planLevelLayer(*level, info, kLevelShearWall)))
					insertAboveBeamTop(planLevelType(*level, kLevelShearWall),
									   upperOffset + planLevelShift(*level, info));
			}

			// 登り梁は水下側の伏図レベルのレイヤへ分かれている（parse/PlanLevel）ので、
			// 耐力壁と同じく伏図レベルごとに確かめる。母屋は分けない。
			if (storyLevels.empty())
			{
				if (anyMemberOnLayer(members, layerFor(kLevelNoboribari)))
					insertAboveBeamTop(kLevelNoboribari, upperOffset);
			}
			for (const PlanLevel* level : storyLevels)
			{
				if (anyMemberOnLayer(members, planLevelLayer(*level, info, kLevelNoboribari)))
					insertAboveBeamTop(planLevelType(*level, kLevelNoboribari),
									   upperOffset + planLevelShift(*level, info));
			}
			if (anyMemberOnLayer(members, layerFor(kLevelMoya)))
				insertAboveBeamTop(kLevelMoya, upperOffset);

			// M6 屋根組: 屋根版（屋根面）を含む階に 垂木 → 野地板 レベル（"n-垂木" /
			// "n-野地板" レイヤ）を足す。スタックは 横架材天端/軒高 ← 登り梁 ← 母屋 ← 垂木 ←
			// 野地板（上ほど上段）なので、垂木・野地板の順に挿入する。
			//
			// ［レベルを足す条件］**屋根版がある階だけ**に絞る（最上階だから足す、とはしない）:
			// 垂木・野地板の命令は屋根版からのみ生まれるので、屋根版の無い階にレベルを作ると
			// 空レイヤが残るだけになる（ロフトの FL レベルを床版の有無で絞るのと同じ方針）。
			// ホームズ君の出力では最上階が必ず主屋根の屋根版を含むので、実データでは
			// 「最上階＋下屋根のある階」に落ち着く。
			if (storyHasRoofSlab(context, info.id))
			{
				insertAboveBeamTop(kLevelTaruki, upperOffset);
				insertAboveBeamTop(kLevelNojiita, upperOffset);
			}

			// 伏図レベルのレベル（横架材の高さごとの伏図。parse/PlanLevel）: 標準でない伏図
			// レベルの横架材・床は、元のレベル（横架材天端／軒高・FL）をその高さのぶんずらした
			// 別のレベルのレイヤ（"2-横架材天端(FL-872)" / "2-FL(FL-872)"）へ置かれる。
			// **元のレベルの直下**へ積む（同じ種類のレイヤが並ぶので、重ね順の決まり
			// ——床は背面へ・耐力壁は前面へ——は印を外した種別で効く。core の
			// desiredStoryLayerOrder）。命令があるときだけ作る（空のレイヤを作らない）。
			const auto insertBelow = [&cmd, &i, &info](const std::string& baseType,
													   const std::string& levelType, double offset)
			{
				auto at = std::ranges::find(cmd.levels, baseType, &LevelCommand::type);
				// 元のレベルが無い（屋根階にロフト床が無い）ときは先頭（＝最上段）へ。
				at = at == cmd.levels.end() ? cmd.levels.begin() : at + 1;
				cmd.levels.insert(
					at, LevelCommand{levelType, offset, storyLayerName(i, info.isTop, levelType)});
			};
			const char* const beamType = beamTopLevelType(info.isTop);
			const double floorOffset = info.isTop ? kLoftFloorLevelOffset : 0.0;
			// 後から挿したものが元のレベルのすぐ下に来るので、低い方から挿すと元のレベルの
			// 下に高い順に並ぶ（重なりの無い高さどうしなので見え方には効かないが、並びは
			// 決定的にしておく）。
			for (const PlanLevel* level : storyLevels)
			{
				if (level->standard)
					continue;
				const double shift = planLevelShift(*level, info);
				if (anyMemberOnLayer(members, planLevelBeamLayer(*level, info)))
					insertBelow(beamType, planLevelType(*level, beamType), upperOffset + shift);
				const std::string floorLayer = planLevelLayer(*level, info, kLevelFL);
				if (std::ranges::any_of(floors, [&floorLayer](const core::FloorCommand& floor)
										{ return floor.layer == floorLayer; }))
					insertBelow(kLevelFL, planLevelType(*level, kLevelFL), floorOffset + shift);
			}

			// M8 柱: この階を base（from = i+1）とする span レイヤ（"{from}to{to}-柱"）
			// のレベルを、levels の**先頭＝スタック最上段**（FL／軒高レイヤの直上）へ (from,
			// to)昇順で積む。レベル種別はレイヤ名そのもの（span ごとに一意な文字列が要るため）。
			// 高さは横架材天端（最上階は軒高）に揃えるが、柱の上下端は bottomBound /
			// topBound が指すレベルで決まるのでこのオフセットには依存しない。
			//
			// レイヤは**実在する柱から決まる**ので、母屋・登り梁と同じく「命令があるときだけ
			// ・命令があれば必ず」できる（空レイヤを作らない）。
			const auto spanLayers = columnLayers.find(static_cast<int>(i));
			if (spanLayers != columnLayers.end())
			{
				std::vector<LevelCommand> spanLevels;
				spanLevels.reserve(spanLayers->second.size());
				for (const std::string& layer : spanLayers->second)
					spanLevels.push_back(LevelCommand{layer, upperOffset, layer});
				cmd.levels.insert(cmd.levels.begin(), spanLevels.begin(), spanLevels.end());
			}
			commands.push_back(std::move(cmd));
		}
		return commands;
	}

	std::vector<StoryCommand> buildStoryCommands(const Model& model)
	{
		Context context(model);
		return buildStoryCommands(context);
	}
} // namespace HomeskzIfcImport::parse
