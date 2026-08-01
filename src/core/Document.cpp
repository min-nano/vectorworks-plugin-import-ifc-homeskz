//
//	core/Document.cpp
//
//	validateDocument の実装。Python 版 document.py の validateDocument に対応する。
//	SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//
//	現状はバージョンの妥当性と、stories（M3）・floors（M5）・rafters / roofs（M6）・
//	grids（M1）の各命令の必須フィールド・値域を見る。命令リスト（members / columns …）が追加されるたびに、
//	対応する検証規則（必須フィールドの有無・参照整合性・値域）をここへ足していく。
//

#include "core/Document.h"

#include <algorithm>
#include <ranges>
#include <string>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// ストーリレベル 1 つが妥当か（Python 版 _validate_level 相当）。種別・レイヤ名が
		// 非空であること。offset は数値（C++ では double なので常に成立）。
		bool isValidLevel(const LevelCommand& level)
		{
			return !level.type.empty() && !level.layer.empty();
		}

		// ストーリ 1 つが妥当か（Python 版 _validate_story 相当）。名前・接尾辞が非空で
		// （空 suffix は VW 2026 で 2 回目以降の CreateStory が失敗するため不可）、各レベルが
		// 妥当であること。elevation は数値（double なので常に成立）。
		bool isValidStory(const StoryCommand& story)
		{
			return !story.name.empty() && !story.suffix.empty() &&
				   std::ranges::all_of(story.levels, isValidLevel);
		}

		// スラブの構成層 1 枚が妥当か。名前が非空で、層厚が 0 以上（負の層は作れない）。
		bool isValidSlabComponent(const SlabComponentCommand& component)
		{
			return !component.name.empty() && component.thickness >= 0.0;
		}

		// 床板 1 枚が妥当か（Python 版 _validate_floor 相当）。配置先レイヤ名・クラス名・
		// スラブスタイル名が非空で、平面外形が 3 点以上（面になる）で、高さ基準のレベル種別が
		// 非空で、構成層が 1 枚以上あり総厚が正であること。elevation / bound.offset は数値
		// （double なので常に成立）。
		bool isValidFloor(const FloorCommand& floor)
		{
			if (floor.layer.empty() || floor.drawClass.empty() || floor.boundary.size() < 3 ||
				floor.bound.level.empty() || floor.styleName.empty() || floor.components.empty())
				return false;
			if (!std::ranges::all_of(floor.components, isValidSlabComponent))
				return false;

			double total = 0.0;
			for (const SlabComponentCommand& component : floor.components)
				total += component.thickness;
			return total > 0.0;
		}

		// 垂木 1 本が妥当か（Python 版 _validate_rafter 相当）。配置先レイヤ名・クラス名が
		// 非空で、断面（幅・せい）が正で、平面の始点（軒側＝支持点）と終点（棟側）が縮退して
		// いないこと（縮退＝始点と終点が同じ点。判定は core/Geometry の samePoint）。
		// elevation / endElevation / overhang / embedment は数値（double なので常に成立）。
		// Python 版は型だけを見るが、C++ は型が静的なので「描けない値」を弾く幾何の関門に
		// 読み替える（床板と同じ方針）。
		bool isValidRafter(const RafterCommand& rafter)
		{
			return !rafter.layer.empty() && !rafter.drawClass.empty() && rafter.width > 0.0 &&
				   rafter.height > 0.0 && !samePoint(rafter.start, rafter.end);
		}

		// 野地板 1 枚が妥当か（Python 版 _validate_roof 相当）。配置先レイヤ名・クラス名が
		// 非空で、平面外形が 3 点以上（面になる）で、厚みが正であること。勾配（rise/run）と
		// 高さは数値（double なので常に成立）で、退化した勾配は描画側がフォールバックで
		// 扱うためここでは弾かない（1 枚の異常で文書全体を描かないのは過剰）。
		bool isValidRoof(const RoofCommand& roof)
		{
			return !roof.layer.empty() && !roof.drawClass.empty() && roof.boundary.size() >= 3 &&
				   roof.thickness > 0.0;
		}
	} // namespace

	bool validateDocument(const Document& document)
	{
		if (document.version != kDocumentVersion)
			return false;

		// ストーリ: 名前・接尾辞が非空で、各ストーリレベルの種別・レイヤ名が非空であること
		// （Python 版 _validate_story / _validate_level と同じ関門。ROADMAP.md M3）。
		if (!std::ranges::all_of(document.stories, isValidStory))
			return false;

		// 床板: 配置先レイヤ名・クラス名・スタイル名が非空で、外形が 3 点以上、高さ基準の
		// レベル種別が非空、構成層が 1 枚以上あり総厚が正であること（isValidFloor 参照。
		// Python 版 _validate_floor と同じ関門。ROADMAP.md M5）。
		if (!std::ranges::all_of(document.floors, isValidFloor))
			return false;

		// 垂木・野地板: 配置先レイヤ名・クラス名が非空で、垂木は断面が正・平面が非縮退、
		// 野地板は外形 3 点以上・厚みが正であること（Python 版 _validate_rafter /
		// _validate_roof と同じ関門。ROADMAP.md M6）。
		if (!std::ranges::all_of(document.rafters, isValidRafter))
			return false;
		if (!std::ranges::all_of(document.roofs, isValidRoof))
			return false;

		// 通り芯: 配置先レイヤ名が空でなく、始点と終点が異なる（縮退していない）こと。
		// 同一判定は parse/Grid の重複線除去と同じ core/Geometry の samePoint を通す
		// （閾値がズレると「畳まれた線が検証では非縮退」のような食い違いが起こる）。
		// クラス名は空でもよい（無クラス＝既定クラスへ）。1 本でも不正なら描画しない
		// （Python 版 validateDocument と同じ関門。ROADMAP.md M1）。
		//
		// TODO: 命令リストが増えたら、要素ごとの all_of を && で連ねてここに積む
		// （member … の検証。ROADMAP.md）。
		return std::ranges::all_of(
			document.grids, [](const GridCommand& grid)
			{ return !grid.layer.empty() && !samePoint(grid.start, grid.end); });
	}

	namespace
	{
		// スタック最下段（背面）へ回すレベル種別か（Python 版 _BACKGROUND_LEVEL_TYPES）。
		// 床（FL）・野地板のレイヤは伏図ビューポートで柱・梁を覆い隠さないよう全ストーリ
		// 分をまとめて背面へ集める（野地板レベルは M6 で追加済み。この並びの適用先は
		// M13 の per-viewport 上書き。desiredStoryLayerOrder の doc コメント参照）。
		bool isBackgroundLevel(const std::string& type)
		{
			return type == kLevelFL || type == kLevelNojiita;
		}
	} // namespace

	std::vector<std::string> desiredStoryLayerOrder(const std::vector<StoryCommand>& stories,
													const std::vector<std::string>& topLayers)
	{
		std::vector<std::string> order;
		// 通り芯レイヤ "共通"（core::kGridLayer。GridCommand::layer の既定値と同じ）を
		// スタック最上段に置き、続けて topLayers を積む。
		order.emplace_back(kGridLayer);
		order.insert(order.end(), topLayers.begin(), topLayers.end());

		// stories は Elevation 昇順（最下階→最上階）。スタックは最上階→最下階なので逆順に辿る。
		std::vector<std::string> background;
		for (const StoryCommand& command : std::views::reverse(stories))
		{
			for (const LevelCommand& level : command.levels)
			{
				if (isBackgroundLevel(level.type))
					background.push_back(level.layer);
				else
					order.push_back(level.layer);
			}
		}
		order.insert(order.end(), background.begin(), background.end());
		return order;
	}
} // namespace HomeskzIfcImport::core
