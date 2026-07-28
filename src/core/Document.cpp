//
//	core/Document.cpp
//
//	validateDocument の実装。Python 版 document.py の validateDocument に対応する。
//	SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//
//	骨組みの現状では Document は「バージョン＋空の器」なので、検証はバージョンの
//	妥当性だけを見る。各命令リスト（grids / stories / members …）が追加されるたびに、
//	対応する検証規則（必須フィールドの有無・参照整合性・値域）をここへ足していく。
//

#include "core/Document.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 2 点が実質同一か（縮退した通り芯＝始点と終点が同じ、を弾く判定に使う）。
		// 座標は mm。Python 版のように厳密一致ではなく微小許容で見る（丸め耐性）。
		bool isDegenerate(const Vec2& a, const Vec2& b)
		{
			constexpr double kEps = 1e-6;
			return std::abs(a.x - b.x) < kEps && std::abs(a.y - b.y) < kEps;
		}

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
	} // namespace

	bool validateDocument(const Document& document)
	{
		if (document.version != kDocumentVersion)
			return false;

		// ストーリ: 名前・接尾辞が非空で、各ストーリレベルの種別・レイヤ名が非空であること
		// （Python 版 _validate_story / _validate_level と同じ関門。ROADMAP.md M3）。
		if (!std::ranges::all_of(document.stories, isValidStory))
			return false;

		// 通り芯: 配置先レイヤ名が空でなく、始点と終点が異なる（縮退していない）こと。
		// クラス名は空でもよい（無クラス＝既定クラスへ）。1 本でも不正なら描画しない
		// （Python 版 validateDocument と同じ関門。ROADMAP.md M1）。
		//
		// TODO: 命令リストが増えたら、要素ごとの all_of を && で連ねてここに積む
		// （member … の検証。ROADMAP.md）。
		return std::ranges::all_of(
			document.grids, [](const GridCommand& grid)
			{ return !grid.layer.empty() && !isDegenerate(grid.start, grid.end); });
	}

	namespace
	{
		// スタック最下段（背面）へ回すレベル種別か（Python 版 _BACKGROUND_LEVEL_TYPES）。
		// 床（FL）・野地板のレイヤは伏図ビューポートで柱・梁を覆い隠さないよう全ストーリ
		// 分をまとめて背面へ集める（M9 床板で効く。FL は M3 から背面対象に含めておく）。
		bool isBackgroundLevel(const std::string& type)
		{
			return type == "FL" || type == "野地板";
		}
	} // namespace

	std::vector<std::string> desiredStoryLayerOrder(const std::vector<StoryCommand>& stories,
													const std::vector<std::string>& topLayers)
	{
		std::vector<std::string> order;
		// 通り芯レイヤ "共通"（Python 版 vw/story.py GRID_LAYER。ifc/grid.py の配置先と同じ）を
		// スタック最上段に置き、続けて topLayers を積む。
		order.emplace_back("共通");
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
