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
#include "core/Region.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
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

	std::vector<LoftFloorRegion> loftFloorRegions(const Model& model, int storeyId)
	{
		// 屋根階の床梁（床大梁・床小梁・甲乙梁）の平面外形を集める。種別は IFC Name の
		// 記録だけで判定する（resolveMemberClass の高さ推定は使わない。屋根階の無名部材
		// ＝火打・隅木谷木まで床梁に化けると、床でない領域まで囲ってしまうため）。
		std::vector<std::vector<Vec2>> parts;
		double beamTop = 0.0;
		bool hasBeamTop = false;
		for (const int elementId : collectStoryElements(model, storeyId))
		{
			const Entity* element = model.entity(elementId);
			if (element == nullptr)
				continue;
			if (element->type != "IFCBEAM" && element->type != "IFCMEMBER")
				continue;
			const std::optional<std::string> memberClass =
				memberClassFromName(element->attribute(kNameAttr).text);
			if (!memberClass.has_value() || *memberClass != CLASS_YUKABARI)
				continue;

			WorldSolid solid;
			if (!resolveElementWorldSolid(model, element, solid))
				continue; // 押し出しを解決できない床梁はスキップ

			// 床下地はこれらの梁の天端に載る。天端は最大値を採るので、部材の列挙順に
			// 依存しない（梁せいが違っても最も高い天端が床を受ける）。
			double topLocal = 0.0;
			double thicknessLocal = 0.0;
			zTopAndThickness(solid, topLocal, thicknessLocal);
			if (!hasBeamTop || topLocal > beamTop)
				beamTop = topLocal;
			hasBeamTop = true;

			parts.push_back(footprint(solid));
		}

		std::vector<LoftFloorRegion> regions;
		for (std::vector<Vec2>& outline : core::filledUnionOutlines(parts))
			regions.push_back(LoftFloorRegion{std::move(outline), beamTop});
		return regions;
	}

	bool storyHasLoftFloor(const Model& model, int storeyId)
	{
		return storyHasFloorSlab(model, storeyId) || !loftFloorRegions(model, storeyId).empty();
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

			// 床 1 枚の命令を組み立てる。boundary は IFC の生座標（ここで通り芯センタリング
			// を掛ける）、levelDelta は基準レベル（一般階＝FL／屋根階＝軒高）からの高低差。
			const auto makeCommand = [&](std::vector<Vec2> boundary, double levelDelta)
			{
				for (Vec2& p : boundary)
				{
					p.x -= center.x;
					p.y -= center.y;
				}
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
				return cmd;
			};

			const std::size_t before = commands.size();
			for (const int elementId : collectStoryElements(model, story.id))
			{
				const Entity* element = model.entity(elementId);
				if (element == nullptr || !isFloorSlab(*element))
					continue;

				WorldSolid solid;
				if (!resolveElementWorldSolid(model, element, solid))
					continue; // 押し出しを解決できない床版はスキップ

				// IFC の床位置を尊重する: 床版ソリッドの最下端（ストーリ高さ ＋ ローカル
				// 最下端 Z）が床を受ける位置で、横架材天端（屋根階は軒高）からの高低差が
				// 段差＝スキップフロアになる。命令が持つ高さは基準面（一般階＝床仕上げ
				// 上端、屋根階＝床下地下端）なので、その高低差を基準レベルへ足す
				// （段差が無ければ一般階は FL ちょうど、屋根階は軒高ちょうど）。
				double topLocal = 0.0;
				double thicknessLocal = 0.0;
				zTopAndThickness(solid, topLocal, thicknessLocal);
				const double bottomAbs = story.elevation + (topLocal - thicknessLocal);

				// 外形は必ず 3 点以上になる（resolveExtrudedAreaSolid が成功した時点で
				// プロファイルは非空、かつ resolveProfile は矩形＝4 点・任意断面＝3 点以上しか
				// 返さない。水平押し出しの掃引矩形も 4 点）。validateDocument の 3 点以上の
				// 関門と整合する。
				commands.push_back(makeCommand(footprint(solid), bottomAbs - beamTopAbs));
			}

			// 屋根階に床版が 1 枚も無いときだけ、床梁が囲む領域をロフト床として補う
			// （ホームズ君はロフトの床版を出力しない。ヘッダ「ロフトの外形は床梁から
			// 合成する」参照）。床版があるならそちらが正で、合成は行わない。
			if (story.isTop && commands.size() == before)
			{
				for (LoftFloorRegion& region : loftFloorRegions(model, story.id))
					commands.push_back(
						makeCommand(std::move(region.boundary), region.beamTopOffset));
			}
		}
		return commands;
	}
} // namespace HomeskzIfcImport::parse
