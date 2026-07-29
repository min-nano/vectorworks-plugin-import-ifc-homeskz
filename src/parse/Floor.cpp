//
//	parse/Floor.cpp
//
//	床板解析の実装。Python 版 ifc/floor.py の build_floor_commands に対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（core/parse のみ依存）。
//

#include "parse/Floor.h"
#include "parse/Grid.h"
#include "parse/IfcGeometry.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::FloorCommand;
	using core::StoryBoundCommand;
	using core::Vec2;

	namespace
	{
		// 床の高さ基準にするレベル種別名。parse/Story が一般階に作る "横架材天端" レベルと
		// 一致させる（Python 版 LEVEL_BEAM_TOP）。ここがズレると SetObjectStoryBound が
		// 解決できないレベルを指してしまう。
		constexpr const char* kLevelBeamTop = "横架材天端";
		// FL レイヤ名の接尾辞（"1-FL" の "FL"。Python 版 LEVEL_FL）。
		constexpr const char* kLevelFL = "FL";

		// IfcRoot の Name 属性インデックス（GlobalId, OwnerHistory, Name=2, …）。
		constexpr std::size_t kNameAttr = 2;

		// 要素が床板（IfcSlab かつ Name が "床版"）か（Python 版 _is_floor_slab）。
		bool isFloorSlab(const Entity& element)
		{
			if (element.type != "IFCSLAB")
				return false;
			const Value& name = element.attribute(kNameAttr);
			return name.type == ValueType::String && name.text == kFloorSlabName;
		}
	} // namespace

	std::vector<FloorCommand> buildFloorCommands(const Model& model)
	{
		const std::vector<StoryInfo> stories = collectStories(model);
		if (stories.empty())
			return {};

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ補正なし＝生の IFC 座標）。
		Vec2 center{0.0, 0.0};
		resolveGridCenter(model, center);

		std::vector<FloorCommand> commands;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			// 最上階（屋根）は FL レイヤを持たない（軒高のみ）ため床板を配置しない。
			if (story.isTop)
				continue;

			// FL レイヤ名は parse/Story のレイヤ名規約（layer_prefix_for）と同じ "{階}-FL"。
			const std::string layer = std::to_string(i + 1) + "-" + kLevelFL;
			// 標準の床高＝横架材天端（基準高さ）の絶対 Z。段差床はここからの高低差でずれる。
			const double beamTopAbs = story.elevation + story.beamOffset;

			for (const int elementId : collectStoryElements(model, story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isFloorSlab(*element))
					continue;

				WorldSolid solid;
				if (!resolveElementWorldSolid(model, element, solid))
					continue; // 押し出しを解決できない床版はスキップ

				std::vector<Vec2> boundary = footprint(solid);
				if (boundary.size() < 3)
					continue; // 面にならない外形はスキップ（validateDocument も弾く）
				for (Vec2& p : boundary)
				{
					p.x -= center.x;
					p.y -= center.y;
				}

				// IFC の床位置を尊重する: 床下端の絶対 Z は床版ソリッドの最下端
				// （ストーリ高さ ＋ ローカル最下端 Z）そのまま。標準の床高（横架材天端）
				// からの高低差は bound.offset に表れる（段差＝スキップフロア）。
				double topLocal = 0.0;
				double thicknessLocal = 0.0;
				zTopAndThickness(solid, topLocal, thicknessLocal);
				const double bottomAbs = story.elevation + (topLocal - thicknessLocal);

				FloorCommand cmd;
				cmd.layer = layer;
				cmd.drawClass = CLASS_FLOOR;
				cmd.boundary = std::move(boundary);
				cmd.thickness = kFloorThickness;
				cmd.elevation = bottomAbs;
				cmd.bound = StoryBoundCommand{0, kLevelBeamTop, bottomAbs - beamTopAbs};
				commands.push_back(std::move(cmd));
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
