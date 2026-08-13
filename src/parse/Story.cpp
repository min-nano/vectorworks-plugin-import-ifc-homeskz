//
//	parse/Story.cpp
//
//	ストーリ解析の実装。Python 版 ifc/story.py の build_story_commands ほかに対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Story.h"
#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/Floor.h"
#include "parse/IfcAttr.h"
#include "parse/Member.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::LevelCommand;
	using core::StoryCommand;

	namespace
	{
		// 最上階のストーリ名。Python 版 STORY_ROOF と一致。
		constexpr const char* kStoryRoof = "屋根";
		// 最上階のストーリ接尾辞・レイヤ接頭辞（Roof）。Python 版 story_suffix_for と一致。
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

		// 名前が（大文字化して）"FL" で終わるか（Python 版 (Name or '').upper().endswith('FL')）。
		bool nameEndsWithFL(const std::string& name)
		{
			if (name.size() < 2)
				return false;
			const std::string upper = asciiUpper(name);
			return upper.compare(upper.size() - 2, 2, "FL") == 0;
		}

		// index（0 始まり）と最上階フラグから VectorWorks のストーリ名を返す
		// （Python 版 story_name_for）。最上階は "屋根"、それ以外は "{index+1}階"。
		std::string storyNameFor(std::size_t index, bool isTop)
		{
			if (isTop)
				return kStoryRoof;
			return std::to_string(index + 1) + "階";
		}

		// 横架材命令のどれかが layer を配置先に指しているか。母屋・登り梁レベルを足すかの
		// 判定に使う（buildStoryCommands の「Python 版との差異」参照）。
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
		// "to" が 2 つ以上あるレイヤ名（"1to2to3-柱"）は分解できないので span でないとする
		// （Python 版の core.split('to') が 2 要素でなければ None を返すのと同じ関門）。
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

	bool getLocalPlacementZ(const Model& model, const Entity& element, double& outZ)
	{
		// element.ObjectPlacement（IfcLocalPlacement）→ RelativePlacement（IfcAxis2Placement3D）
		// → Location（IfcCartesianPoint）の Z。親 PlacementRelTo は辿らない（M2 と同じ規約）。
		const Entity* placement = model.resolve(element.attribute(attr::kProductObjectPlacement));
		if (placement == nullptr || placement->type != "IFCLOCALPLACEMENT")
			return false;
		const Entity* axis =
			model.resolve(placement->attribute(attr::kLocalPlacementRelativePlacement));
		if (axis == nullptr || axis->type != "IFCAXIS2PLACEMENT3D")
			return false;
		const Entity* point = model.resolve(axis->attribute(attr::kAxis2PlacementLocation));
		if (point == nullptr || point->type != "IFCCARTESIANPOINT")
			return false;
		// IfcCartesianPoint.Coordinates は実数のリスト。Z は 3 番目。
		const Value& coords = point->attribute(attr::kCartesianPointCoordinates);
		if (!coords.isList() || coords.items.size() < 3)
			return false;
		outZ = coords.items[2].asReal();
		return true;
	}

	std::vector<int> collectStoryElements(const Model& model, int storeyId)
	{
		// storey を RelatingStructure に持つ IfcRelContainedInSpatialStructure を、逆参照
		// （referrers）から辿る（Python 版は storey.ContainsElements。同じ逆関係）。
		// 誰からも参照されていない階（要素を 1 つも持たない階）は即座に空を返す。
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
		const std::vector<StoryInfo> stories = context.stories();
		// 母屋・登り梁レベルの有無は、実際に組み立てた横架材命令の配置先レイヤから決める
		// （下記「Python 版との差異」）。コンテキストが 1 度だけ解析するので、垂木・登り梁の
		// 補正と同じ結果を共有する。
		const std::vector<core::MemberCommand>& members = context.members();
		// span 柱レイヤ（"{from}to{to}-柱"）は実在する柱から決まる。コンテキストが柱命令を
		// 1 度だけ組み立てるので、ここと Document の columns は同じ結果を共有する
		// （parse/Context.h の columns）。
		const std::map<int, std::vector<std::string>> columnLayers =
			collectColumnLayersByStory(context.columns());

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
				// 最上階（屋根）は軒高（オフセット 0）。ロフト（小屋裏収納）の床がある
				// ときだけ、その標準床レベル FL（軒高 + kLoftFloorLevelOffset）を足す
				// （床の無い屋根に空の FL レイヤを作らない。Python 版が story_has_moya /
				// story_has_roof で条件付きにレベルを足すのと同じ枠組み）。この FL が
				// ロフト床の配置先レイヤ "R-FL" になる。ロフトの床は床版（IfcSlab）でも
				// 床梁から合成した領域でもよい（parse/Floor の storyHasLoftFloor）。
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
				[&cmd, &layerFor, upperOffset, beamTopIndex](const char* levelType)
			{
				cmd.levels.insert(cmd.levels.begin() + beamTopIndex,
								  LevelCommand{levelType, upperOffset, layerFor(levelType)});
			};

			// M7 横架材: 母屋・棟木（"n-母屋"）と登り梁（"n-登り梁"）は、梁（小屋梁・軒桁）と
			// 重なって見にくいため専用レイヤへ分離する（parse/Member）。そのレイヤはここで作る。
			// スタックは 横架材天端/軒高 ← 登り梁 ← 母屋 なので、登り梁 → 母屋 の順に挿入する。
			//
			// ［Python 版との差異・意図的］Python 版は名前判定（story_has_moya /
			// story_has_noboribari）でレベルを足し、さらに最上階には母屋レベルを無条件で足す。
			// 本移植は**実際に組み立てた横架材命令の配置先レイヤ**で判定する。理由は 2 つ:
			//   * 名前判定は「名前では判別できないが高さで母屋と推定された最上階の材」
			//     （隅木谷木等）を取りこぼす。Python 版はそれを最上階の無条件追加で救っている。
			//   * その無条件追加は、母屋を持たない最上階に空レイヤを残す（本移植は空レイヤを
			//     作らない方針。M5 ロフト FL・M6 垂木/野地板と同じ）。
			// 命令の配置先で判定すれば、**レイヤは命令があるときだけ・命令があれば必ず**でき、
			// 両方の齟齬が構造的に起きない。
			for (const char* levelType : {kLevelNoboribari, kLevelMoya})
			{
				if (anyMemberOnLayer(members, layerFor(levelType)))
					insertAboveBeamTop(levelType);
			}

			// M6 屋根組: 屋根版（屋根面）を含む階に 垂木 → 野地板 レベル（"n-垂木" /
			// "n-野地板" レイヤ）を足す。スタックは 横架材天端/軒高 ← 登り梁 ← 母屋 ← 垂木 ←
			// 野地板（上ほど上段）なので、垂木・野地板の順に挿入する。
			//
			// ［Python 版との差異・意図的］Python 版は最上階（屋根）には屋根版の有無に関わらず
			// 垂木・野地板レベルを持たせる（is_top or roof_flags[i]）。本移植は**屋根版がある階
			// だけ**に絞る: 垂木・野地板の命令は屋根版からのみ生まれるので、屋根版の無い階に
			// レベルを作ると空レイヤが残るだけになる（ロフトの FL レベルを床版の有無で絞るのと
			// 同じ方針）。ホームズ君の出力では最上階は必ず主屋根の屋根版を含むため、実データでの
			// 結果は Python 版と一致する。
			if (storyHasRoofSlab(context, info.id))
			{
				insertAboveBeamTop(kLevelTaruki);
				insertAboveBeamTop(kLevelNojiita);
			}

			// M8 柱: この階を base（from = i+1）とする span レイヤ（"{from}to{to}-柱"）の
			// レベルを、levels の**先頭＝スタック最上段**（FL／軒高レイヤの直上）へ (from, to)
			// 昇順で積む。レベル種別はレイヤ名そのもの（span ごとに一意な文字列が要るため。
			// Python 版と同じ）。高さは横架材天端（最上階は軒高）に揃えるが、柱の上下端は
			// bottomBound / topBound が指すレベルで決まるのでこのオフセットには依存しない。
			//
			// レイヤは**実在する柱から決まる**ので、母屋・登り梁と同じく「命令があるときだけ・
			// 命令があれば必ず」できる（空レイヤを作らない。Python 版も span レイヤは
			// collect_column_layers_by_story 由来で同じ）。
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
