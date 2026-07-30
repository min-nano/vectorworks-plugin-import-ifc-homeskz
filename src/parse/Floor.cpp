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
		// 高さ基準にするレベル種別名。parse/Story が作るレベル名と一致させる（Python 版
		// LEVEL_FL / LEVEL_EAVES）。ここがズレると SetObjectStoryBound が解決できないレベルを
		// 指してしまう。FL レイヤ名の接尾辞（"1-FL" / "R-FL" の "FL"）も同じ名前。
		constexpr const char* kLevelFL = "FL";
		constexpr const char* kLevelEaves = "軒高";

		// IfcRoot の Name 属性インデックス（GlobalId, OwnerHistory, Name=2, …）。
		constexpr std::size_t kNameAttr = 2;

		// スラブスタイル名の接尾辞と、最上階（屋根）の接頭辞。
		constexpr const char* kStyleSuffix = "-床スタイル";
		constexpr const char* kRoofStylePrefix = "屋根";

		// 要素が床板（IfcSlab かつ Name が "床版"）か（Python 版 _is_floor_slab）。
		bool isFloorSlab(const Entity& element)
		{
			if (element.type != "IFCSLAB")
				return false;
			const Value& name = element.attribute(kNameAttr);
			return name.type == ValueType::String && name.text == kFloorSlabName;
		}
	} // namespace

	bool storyHasFloorSlab(const Model& model, int storeyId)
	{
		// 戻り値を一度束縛してから走査する（一時オブジェクトを直接 ranges へ渡さない）。
		const std::vector<int> elementIds = collectStoryElements(model, storeyId);
		return std::ranges::any_of(elementIds,
								   [&model](int elementId)
								   {
									   const Entity* element = model.entity(elementId);
									   return element != nullptr && isFloorSlab(*element);
								   });
	}

	std::string floorSlabStyleName(std::size_t index, bool isTop)
	{
		// 一般階は "{階}F-床スタイル"、最上階は "屋根-床スタイル"（小屋裏収納・ロフトの床）。
		if (isTop)
			return std::string(kRoofStylePrefix) + kStyleSuffix;
		return std::to_string(index + 1) + "F" + kStyleSuffix;
	}

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

			// 配置先レイヤは parse/Story のレイヤ名規約と同じ "{接頭辞}-FL"（一般階は
			// "1-FL"…、屋根階は "R-FL"）。屋根階の FL レベルは床版があるときだけ
			// parse/Story が作る（ロフト＝小屋裏収納の床）。
			const std::string layer = storyLayerPrefix(i, story.isTop) + "-" + kLevelFL;

			// 床を受ける基準高さ（横架材天端＝床下地の下端）の絶対 Z。段差床はここからの
			// 高低差でずれる。屋根階は軒高（ストーリ原点）がその高さにあたる。
			const double beamTopAbs =
				story.isTop ? story.elevation : story.elevation + story.beamOffset;

			// スラブ構成: 上から 床仕上げ ＋ 床下地（24mm 固定）。総厚は
			//   一般階 … FL 高さ − 横架材天端高さ（下端が横架材天端・上端が FL に収まる）
			//   屋根階 … ロフトの標準床レベル（軒高 + 36mm。仮定値）
			// 横架材天端オフセットを取れない（＝柱も床版も無い）階では合計が床下地だけに
			// なるよう仕上げを 0 に丸める（負の層は作れない。1 階の欠損で全体を止めない）。
			const double slabThickness =
				story.isTop ? kLoftFloorLevelOffset : story.elevation - beamTopAbs;
			const double finishThickness = std::max(slabThickness - kSubfloorThickness, 0.0);

			// 高さ基準の面とバインド先レベル:
			//   一般階 … 床仕上げ上端（Top）を FL レベルへ。FL は IFC 由来の確かな高さ。
			//   屋根階 … 床下地下端（Bottom）を軒高レベルへ。ロフトの FL は仮定値なので、
			//            確かな構造面（軒高＝横架材天端）を基準にする。
			const core::SlabDatum datum =
				story.isTop ? core::SlabDatum::Bottom : core::SlabDatum::Top;
			const char* const boundLevel = story.isTop ? kLevelEaves : kLevelFL;
			// 基準面のストーリレベル上の絶対 Z（段差 0 のときの基準面の高さ）。
			const double datumBaseAbs = story.isTop ? beamTopAbs : story.elevation;

			// スラブスタイルは階ごとに 1 つ（階により構成が異なることが多いため）。
			const std::string styleName = floorSlabStyleName(i, story.isTop);

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
				// 最下端 Z）が床を受ける位置で、横架材天端（屋根階は軒高）からの高低差が
				// 段差＝スキップフロアになる。命令が持つ高さは基準面（一般階＝床仕上げ
				// 上端、屋根階＝床下地下端）なので、その高低差を基準レベルへ足す
				// （段差が無ければ一般階は FL ちょうど、屋根階は軒高ちょうど）。
				double topLocal = 0.0;
				double thicknessLocal = 0.0;
				zTopAndThickness(solid, topLocal, thicknessLocal);
				const double bottomAbs = story.elevation + (topLocal - thicknessLocal);
				const double levelDelta = bottomAbs - beamTopAbs;

				FloorCommand cmd;
				cmd.layer = layer;
				cmd.drawClass = CLASS_FLOOR;
				cmd.boundary = std::move(boundary);
				cmd.styleName = styleName;
				cmd.components = {SlabComponentCommand{kFloorFinishName, finishThickness},
								  SlabComponentCommand{kSubfloorName, kSubfloorThickness}};
				cmd.datum = datum;
				cmd.elevation = datumBaseAbs + levelDelta;
				cmd.bound = StoryBoundCommand{0, boundLevel, levelDelta};
				commands.push_back(std::move(cmd));
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
