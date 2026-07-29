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

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::FloorCommand;
	using core::SlabComponentCommand;
	using core::StoryBoundCommand;
	using core::Vec2;

	namespace
	{
		// 床仕上げ上端の高さ基準にするレベル種別名。parse/Story が一般階に作る "FL" レベルと
		// 一致させる（Python 版 LEVEL_FL）。ここがズレると SetObjectStoryBound が解決できない
		// レベルを指してしまう。FL レイヤ名の接尾辞（"1-FL" の "FL"）も同じ名前。
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
			// 横架材天端（床を受ける基準高さ）の絶対 Z。段差床はここからの高低差でずれる。
			const double beamTopAbs = story.elevation + story.beamOffset;

			// スラブ構成: 上から 床仕上げ（FL 高さ − 横架材天端高さ − 床下地厚）＋
			// 床下地（24mm 固定）。合計＝FL 高さ − 横架材天端高さなので、段差の無い床は
			// 下端が横架材天端・上端が FL にちょうど収まる。横架材天端オフセットを取れない
			// （＝柱も床版も無い）階では合計が床下地だけになるよう仕上げを 0 に丸める
			// （負の層は作れない。1 階の欠損で全体を止めない）。
			const double slabThickness = story.elevation - beamTopAbs;
			const double finishThickness = std::max(slabThickness - kSubfloorThickness, 0.0);

			for (const int elementId : collectStoryElements(model, story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isFloorSlab(*element))
					continue;

				WorldSolid solid;
				if (!resolveElementWorldSolid(model, element, solid))
					continue; // 押し出しを解決できない床版はスキップ

				// 外形は必ず 3 点以上になる（resolveExtrudedAreaSolid が成功した時点で
				// プロファイルは非空、かつ resolveProfile は矩形＝4 点・任意断面＝3 点以上しか
				// 返さない。水平押し出しの掃引矩形も 4 点）。validateDocument の 3 点以上の
				// 関門と整合する。
				std::vector<Vec2> boundary = footprint(solid);
				for (Vec2& p : boundary)
				{
					p.x -= center.x;
					p.y -= center.y;
				}

				// IFC の床位置を尊重する: 床版ソリッドの最下端（ストーリ高さ ＋ ローカル
				// 最下端 Z）が床を受ける位置で、横架材天端からの高低差が段差＝スキップ
				// フロアになる。命令が持つ高さは**床仕上げ上端**なので、その高低差を FL に
				// 足した値（一般部は FL そのもの、床レベル指定時は FL ± 差分）にする。
				double topLocal = 0.0;
				double thicknessLocal = 0.0;
				zTopAndThickness(solid, topLocal, thicknessLocal);
				const double bottomAbs = story.elevation + (topLocal - thicknessLocal);
				const double levelDelta = bottomAbs - beamTopAbs;

				FloorCommand cmd;
				cmd.layer = layer;
				cmd.drawClass = CLASS_FLOOR;
				cmd.boundary = std::move(boundary);
				cmd.components = {SlabComponentCommand{kFloorFinishName, finishThickness},
								  SlabComponentCommand{kSubfloorName, kSubfloorThickness}};
				cmd.elevation = story.elevation + levelDelta;
				cmd.bound = StoryBoundCommand{0, kLevelFL, levelDelta};
				commands.push_back(std::move(cmd));
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
