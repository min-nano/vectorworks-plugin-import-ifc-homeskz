//
//	parse/Story.cpp
//
//	ストーリ解析の実装。Python 版 ifc/story.py の build_story_commands ほかに対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Story.h"
#include "parse/Floor.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::LevelCommand;
	using core::StoryCommand;

	namespace
	{
		// レベル種別名。Python 版 ifc/story.py の LEVEL_FL / LEVEL_BEAM_TOP / LEVEL_EAVES と
		// 一致させる。CreateLayerLevelType へ登録し GetLayerForStory でレイヤを取り直す鍵。
		constexpr const char* kLevelFL = "FL";
		constexpr const char* kLevelBeamTop = "横架材天端";
		constexpr const char* kLevelEaves = "軒高";
		// 最上階のストーリ名。Python 版 STORY_ROOF と一致。
		constexpr const char* kStoryRoof = "屋根";
		// 最上階のストーリ接尾辞・レイヤ接頭辞（Roof）。Python 版 story_suffix_for と一致。
		constexpr const char* kRoofSuffix = "R";

		// IfcProduct（IfcColumn / IfcSlab 等）の ObjectPlacement 属性インデックス
		// （GlobalId, OwnerHistory, Name, Description, ObjectType, ObjectPlacement=5, …）。
		// parse/IfcGeometry の kObjectPlacementAttr と同値（フェーズ内で二重定義を避けたいが、
		// Story は IfcGeometry に依存させないため局所に持つ）。
		constexpr std::size_t kObjectPlacementAttr = 5;

		// IfcBuildingStorey の属性インデックス（… CompositionType=8, Elevation=9）。
		constexpr std::size_t kStoreyNameAttr = 2;
		constexpr std::size_t kStoreyElevationAttr = 9;

		// IfcRelContainedInSpatialStructure の属性インデックス
		// （… RelatedElements=4, RelatingStructure=5）。
		constexpr std::size_t kRelRelatedElementsAttr = 4;
		constexpr std::size_t kRelRelatingStructureAttr = 5;

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

		// エンティティの指定属性（Name なら kStoreyNameAttr=2）を文字列で返す
		// （未設定・非文字列なら空）。
		std::string entityName(const Entity& entity, std::size_t attrIndex)
		{
			const Value& name = entity.attribute(attrIndex);
			return (name.type == ValueType::String) ? name.text : std::string();
		}

		// index（0 始まり）と最上階フラグから VectorWorks のストーリ名を返す
		// （Python 版 story_name_for）。最上階は "屋根"、それ以外は "{index+1}階"。
		std::string storyNameFor(std::size_t index, bool isTop)
		{
			if (isTop)
				return kStoryRoof;
			return std::to_string(index + 1) + "階";
		}

	} // namespace

	std::string storyLayerPrefix(std::size_t index, bool isTop)
	{
		// CreateStory の接尾辞（＝レイヤ接頭辞）。最上階は "R"、それ以外は "{index+1}"。
		// 空文字は 2 回目以降の CreateStory が失敗するため必ず非空。
		if (isTop)
			return kRoofSuffix;
		return std::to_string(index + 1);
	}

	bool getLocalPlacementZ(const Model& model, const Entity& element, double& outZ)
	{
		// element.ObjectPlacement（IfcLocalPlacement）→ RelativePlacement（IfcAxis2Placement3D）
		// → Location（IfcCartesianPoint）の Z。親 PlacementRelTo は辿らない（M2 と同じ規約）。
		const Entity* placement = model.resolve(element.attribute(kObjectPlacementAttr));
		if (placement == nullptr || placement->type != "IFCLOCALPLACEMENT")
			return false;
		// IfcLocalPlacement(PlacementRelTo, RelativePlacement)。属性 1 が RelativePlacement。
		const Entity* axis = model.resolve(placement->attribute(1));
		if (axis == nullptr || axis->type != "IFCAXIS2PLACEMENT3D")
			return false;
		// IfcAxis2Placement3D(Location, Axis, RefDirection)。属性 0 が Location。
		const Entity* point = model.resolve(axis->attribute(0));
		if (point == nullptr || point->type != "IFCCARTESIANPOINT")
			return false;
		// IfcCartesianPoint.Coordinates は実数のリスト（属性 0）。Z は 3 番目。
		const Value& coords = point->attribute(0);
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
			if (rel->attribute(kRelRelatingStructureAttr).reference != storeyId)
				continue;

			const Value& related = rel->attribute(kRelRelatedElementsAttr);
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

	double resolveBeamTopOffset(const Model& model, int storeyId)
	{
		// 階に属する IfcColumn / IfcSlab のローカル Z 負値の最大を採る（最初に見つかった
		// 値ではなく最大値なので、列挙順に依存しない決定的な結果になる）。
		double best = 0.0;
		bool found = false;
		for (const int elementId : collectStoryElements(model, storeyId))
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

	std::vector<StoryInfo> collectStories(const Model& model)
	{
		// 名前が "FL" で終わる IfcBuildingStorey だけを対象にする。
		std::vector<StoryInfo> stories;
		for (const int id : model.byType("IFCBUILDINGSTOREY"))
		{
			const Entity* storey = model.entity(id);
			if (storey == nullptr)
				continue;
			if (!nameEndsWithFL(entityName(*storey, kStoreyNameAttr)))
				continue;
			StoryInfo info;
			info.id = id;
			info.elevation = storey->attribute(kStoreyElevationAttr).asReal();
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
				stories[i].beamOffset = resolveBeamTopOffset(model, stories[i].id);
		}
		return stories;
	}

	std::vector<StoryCommand> buildStoryCommands(const Model& model)
	{
		const std::vector<StoryInfo> stories = collectStories(model);

		std::vector<StoryCommand> commands;
		commands.reserve(stories.size());
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& info = stories[i];
			const std::string prefix = storyLayerPrefix(i, info.isTop);

			StoryCommand cmd;
			cmd.name = storyNameFor(i, info.isTop);
			cmd.suffix = prefix;
			cmd.elevation = info.elevation;

			// 基本レベル（M3）＋屋根組の垂木・野地板（M6）。登り梁・母屋・span 柱は後続 M で
			// 追加する（ヘッダ参照）。levels の並び順は希望するデザインレイヤのスタック順（上→下）。
			if (info.isTop)
			{
				// 最上階（屋根）は軒高（オフセット 0）。ロフト（小屋裏収納）の床がある
				// ときだけ、その標準床レベル FL（軒高 + kLoftFloorLevelOffset）を足す
				// （床版の無い屋根に空の FL レイヤを作らない。Python 版が story_has_moya /
				// story_has_roof で条件付きにレベルを足すのと同じ枠組み）。この FL が
				// ロフト床の配置先レイヤ "R-FL" になる。
				if (storyHasFloorSlab(model, info.id))
				{
					cmd.levels.push_back(
						LevelCommand{kLevelFL, kLoftFloorLevelOffset, prefix + "-" + kLevelFL});
				}
				cmd.levels.push_back(LevelCommand{kLevelEaves, 0.0, prefix + "-" + kLevelEaves});
			}
			else
			{
				// 一般階は FL（0）＋横架材天端（負オフセット）。FL を上段に積む。
				cmd.levels.push_back(LevelCommand{kLevelFL, 0.0, prefix + "-" + kLevelFL});
				cmd.levels.push_back(
					LevelCommand{kLevelBeamTop, info.beamOffset, prefix + "-" + kLevelBeamTop});
			}

			// M6 屋根組: 屋根版（屋根面）を含む階に 垂木 → 野地板 レベル（"n-垂木" /
			// "n-野地板" レイヤ）を足す。スタックは 横架材天端/軒高 ← （母屋）← 垂木 ←
			// 野地板（上ほど上段）なので、横架材天端（最上階は軒高）レベルの**直前**へ
			// 垂木・野地板の順に挿入する（後から挿入したものが 1 段上に来る）。高さはいずれも
			// 横架材天端（最上階は軒高）に揃える（実描画の Z は屋根版由来の絶対値を垂木・
			// 野地板の要素が持つため、このオフセットには依存しない）。
			//
			// ［Python 版との差異・意図的］Python 版は最上階（屋根）には屋根版の有無に関わらず
			// 垂木・野地板レベルを持たせる（is_top or roof_flags[i]）。本移植は**屋根版がある階
			// だけ**に絞る: 垂木・野地板の命令は屋根版からのみ生まれるので、屋根版の無い階に
			// レベルを作ると空レイヤが残るだけになる（ロフトの FL レベルを床版の有無で絞るのと
			// 同じ方針）。ホームズ君の出力では最上階は必ず主屋根の屋根版を含むため、実データでの
			// 結果は Python 版と一致する。
			if (storyHasRoofSlab(model, info.id))
			{
				// 横架材天端（最上階は軒高）レベルは常に末尾なので、その直前が挿入位置。
				const auto tail = static_cast<std::ptrdiff_t>(cmd.levels.size()) - 1;
				const double roofOffset = info.isTop ? 0.0 : info.beamOffset;
				cmd.levels.insert(
					cmd.levels.begin() + tail,
					LevelCommand{kLevelTaruki, roofOffset, prefix + "-" + kLevelTaruki});
				cmd.levels.insert(
					cmd.levels.begin() + tail,
					LevelCommand{kLevelNojiita, roofOffset, prefix + "-" + kLevelNojiita});
			}
			commands.push_back(std::move(cmd));
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
