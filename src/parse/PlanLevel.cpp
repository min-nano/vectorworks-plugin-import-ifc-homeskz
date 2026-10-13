//
//	parse/PlanLevel.cpp
//
//	伏図レベルの実装（意図は parse/PlanLevel.h）。【SDK 非依存】ここでは VectorWorks SDK を
//	include しない。
//

#include "parse/PlanLevel.h"
#include "parse/Context.h"
#include "parse/Sheet.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 柱の上端が伏図レベルに「届いた」とみなす余裕（mm）。天端の高さは mm に丸めてある
		// ので、丸めの半分より大きく 1mm の段差より小さくはできない——1mm にしておく。
		constexpr double kPlanLevelReachTol = 1.0;

		long long roundedMm(double z)
		{
			return std::llround(z);
		}

		// 水平な材か（両端の天端が mm に丸めて同じ）。
		bool isHorizontal(const core::MemberCommand& member)
		{
			return roundedMm(member.elevation) == roundedMm(member.endElevation);
		}

		// 伏図レベルの天端のうち z にいちばん近いものまでの距離。
		double distanceTo(const PlanLevel& level, double z)
		{
			double best = std::numeric_limits<double>::max();
			for (const long long height : level.heights)
				best = std::min(best, std::abs(static_cast<double>(height) - z));
			return best;
		}
	} // namespace

	std::vector<std::vector<long long>>
	collectBeamHeights(const std::vector<StoryInfo>& stories,
					   const std::vector<core::MemberCommand>& members)
	{
		std::vector<std::vector<long long>> heights(stories.size());
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const std::string layer = beamTopLayerName(i, stories[i]);
			for (const core::MemberCommand& member : members)
			{
				if (member.layer == layer && isHorizontal(member))
					heights[i].push_back(roundedMm(member.elevation));
			}
			// 水平な横架材が 1 本も無い階は標準の天端 1 つ（伏図は従来どおり 1 枚）。
			if (heights[i].empty())
				heights[i].push_back(roundedMm(beamTopElevation(stories[i])));
			std::ranges::sort(heights[i]);
			const auto [first, last] = std::ranges::unique(heights[i]);
			heights[i].erase(first, last);
		}
		return heights;
	}

	std::vector<PlanLevel> buildPlanLevels(const std::vector<StoryInfo>& stories,
										   const std::vector<std::vector<long long>>& heights,
										   const core::ImportOptions& options)
	{
		std::vector<PlanLevel> levels;
		int ordinal = 0;
		for (std::size_t i = 0; i < stories.size() && i < heights.size(); ++i)
		{
			const std::size_t storyBegin = levels.size();
			for (const long long height : heights[i])
			{
				const core::PlanLevelKey key{static_cast<int>(i), height};
				// **まとめるのは同じ階の中だけ**（core::ImportOptions::mergedPlanLevels）。
				// 階の最初の高さには寄せる先が無いので、設定に挙がっていても新しく立てる。
				if (levels.size() > storyBegin && options.mergesWithPrevious(key))
				{
					levels.back().heights.push_back(height);
					continue;
				}
				PlanLevel level;
				level.story = i;
				level.heights.push_back(height);
				level.ordinal = ++ordinal;
				levels.push_back(std::move(level));
			}
			// 階のいちばん上の伏図レベルの次の番号は、その階の母屋のレベル（moyaOrdinal）に
			// 空けておく（小屋束の上端・母屋伏図。ヘッダの「通し番号」）。
			++ordinal;

			// 標準の天端にいちばん近い高さを含む伏図レベルを標準にする（同じ近さなら低い
			// 方＝決定的）。標準は名前を変えず、それ以外に高さの印を付ける。
			const double standardZ = beamTopElevation(stories[i]);
			std::size_t standard = storyBegin;
			for (std::size_t k = storyBegin; k < levels.size(); ++k)
			{
				if (distanceTo(levels[k], standardZ) < distanceTo(levels[standard], standardZ))
					standard = k;
			}
			for (std::size_t k = storyBegin; k < levels.size(); ++k)
			{
				levels[k].standard = (k == standard);
				// 段数は標準からの数（階の中は高さの昇順なので、下が負）。
				levels[k].step = static_cast<int>(k) - static_cast<int>(standard);
				// 高さは GL ではなくその階の FL（最上階は軒高）＝ストーリの高さから測る
				// （ご要望。core::planLevelTag）。
				levels[k].datum = std::llround(stories[i].elevation);
				levels[k].top = stories[i].isTop;
				levels[k].tag =
					levels[k].standard ? std::string() : core::planLevelTag(levels[k].step);
			}
		}
		return levels;
	}

	std::vector<long long> standardBeamHeights(const std::vector<StoryInfo>& stories,
											   const std::vector<PlanLevel>& levels)
	{
		std::vector<long long> result;
		result.reserve(stories.size());
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const double standardZ = beamTopElevation(stories[i]);
			long long best = roundedMm(standardZ);
			double bestDistance = std::numeric_limits<double>::max();
			// 伏図レベルは高さの昇順なので、厳密に近いときだけ差し替える＝同じ近さなら低い方。
			for (const PlanLevel& level : levels)
			{
				if (level.story != i)
					continue;
				for (const long long height : level.heights)
				{
					const double distance = std::abs(static_cast<double>(height) - standardZ);
					if (distance < bestDistance)
					{
						bestDistance = distance;
						best = height;
					}
				}
			}
			result.push_back(best);
		}
		return result;
	}

	std::vector<const PlanLevel*> storyPlanLevels(const std::vector<PlanLevel>& levels,
												  std::size_t story)
	{
		std::vector<const PlanLevel*> found;
		for (const PlanLevel& level : levels)
		{
			if (level.story == story)
				found.push_back(&level);
		}
		return found;
	}

	const PlanLevel* nearestPlanLevel(const std::vector<PlanLevel>& levels, std::size_t story,
									  double z)
	{
		const PlanLevel* best = nullptr;
		for (const PlanLevel& level : levels)
		{
			if (level.story != story)
				continue;
			// levels は高さの昇順なので、厳密に近いときだけ差し替える＝同じ近さなら低い方。
			if (best == nullptr || distanceTo(level, z) < distanceTo(*best, z))
				best = &level;
		}
		return best;
	}

	const PlanLevel* planLevelAbove(const std::vector<PlanLevel>& levels, std::size_t story,
									double z)
	{
		const PlanLevel* highest = nullptr;
		for (const PlanLevel& level : levels)
		{
			if (level.story != story)
				continue;
			if (static_cast<double>(level.heights.back()) >= z - kPlanLevelReachTol)
				return &level;
			highest = &level;
		}
		return highest;
	}

	std::size_t storyOfOrdinal(const std::vector<PlanLevel>& levels, double ordinal)
	{
		// 通し番号以下で最も高い伏図レベルの階（母屋のレベルはその階のいちばん上の
		// 伏図レベルの次なので、その階になる）。levels は通し番号の昇順。
		const PlanLevel* below = nullptr;
		for (const PlanLevel& level : levels)
		{
			if (static_cast<double>(level.ordinal) <= ordinal + kOrdinalTol)
				below = &level;
		}
		if (below != nullptr)
			return below->story;
		if (!levels.empty())
			return levels.front().story;
		// 伏図レベルが無い: 階 s の標準は 2s+1・母屋は 2s+2（standardOrdinalWithoutLevels）。
		const double whole = std::floor(ordinal + kOrdinalTol);
		return whole >= 1.0 ? static_cast<std::size_t>((whole - 1.0) / 2.0) : 0;
	}

	double moyaOrdinal(const std::vector<PlanLevel>& levels, std::size_t story)
	{
		const std::vector<const PlanLevel*> storyLevels = storyPlanLevels(levels, story);
		return storyLevels.empty() ? standardOrdinalWithoutLevels(story) + 1.0
								   : static_cast<double>(storyLevels.back()->ordinal) + 1.0;
	}

	namespace
	{
		// 階 story の標準の伏図レベル（無ければ nullptr）。
		const PlanLevel* standardLevelOf(const std::vector<PlanLevel>& levels, std::size_t story)
		{
			for (const PlanLevel& level : levels)
			{
				if (level.story == story && level.standard)
					return &level;
			}
			return nullptr;
		}
	} // namespace

	std::string ordinalLabel(const std::vector<PlanLevel>& levels, std::size_t story, double value)
	{
		if (const PlanLevel* standard = standardLevelOf(levels, story))
			return levelLabel(story, standard->top, value - standard->ordinal);
		// 伏図レベルが無い: 階の標準の通し番号は 2s+1（storyOfOrdinal と同じ対応）。
		return levelLabel(story, false, value - standardOrdinalWithoutLevels(story));
	}

	std::string spanLevelLabel(const std::vector<PlanLevel>& levels, double value)
	{
		return ordinalLabel(levels, storyOfOrdinal(levels, value), value);
	}

	bool labelOrdinal(const std::vector<PlanLevel>& levels, const std::string& label,
					  double& outOrdinal)
	{
		std::string prefix;
		double step = 0.0;
		if (!parseLevelLabel(label, prefix, step))
			return false;
		for (const PlanLevel& level : levels)
		{
			if (level.standard && storyLayerPrefix(level.story, level.top) == prefix)
			{
				outOrdinal = level.ordinal + step;
				return true;
			}
		}
		// 伏図レベルの無い階: 数字の接頭辞は階の番号で、標準の通し番号は 2s+1（ordinalLabel
		// の逆）。
		if (prefix.empty() || prefix.front() < '0' || prefix.front() > '9')
			return false;
		const auto story = static_cast<std::size_t>(std::stoll(prefix) - 1);
		outOrdinal = standardOrdinalWithoutLevels(story) + step;
		return true;
	}

	bool parseSpanOrdinals(const std::vector<PlanLevel>& levels, const std::string& layer,
						   const std::string& suffix, double& outFrom, double& outTo)
	{
		std::string fromLabel;
		std::string toLabel;
		double from = 0.0;
		double to = 0.0;
		if (!parseSpanLayer(layer, suffix, fromLabel, toLabel) ||
			!labelOrdinal(levels, fromLabel, from) || !labelOrdinal(levels, toLabel, to))
			return false;
		outFrom = from;
		outTo = to;
		return true;
	}

	std::string planLevelType(const PlanLevel& level, const std::string& levelType)
	{
		return levelType + level.tag;
	}

	std::string planLevelLabel(const PlanLevel& level, const StoryInfo& story)
	{
		return levelLabel(level.story, story.isTop, level.step);
	}

	std::string planLevelLayer(const PlanLevel& level, const StoryInfo& story,
							   const std::string& levelType)
	{
		return levelLayerName(level.story, story.isTop, level.step, levelType);
	}

	std::string planLevelBeamLayer(const PlanLevel& level, const StoryInfo& story)
	{
		return planLevelLayer(level, story, beamTopLevelType(story.isTop));
	}

	double planLevelShift(const PlanLevel& level, const StoryInfo& story)
	{
		if (level.standard)
			return 0.0;
		return level.height() - beamTopElevation(story);
	}

	std::string planLevelTitleSuffix(const std::vector<PlanLevel>& levels, const PlanLevel& level)
	{
		if (storyPlanLevels(levels, level.story).size() < 2)
			return {};
		std::string suffix = "（";
		for (std::size_t k = 0; k < level.heights.size(); ++k)
		{
			if (k > 0)
				suffix += "・";
			suffix += core::planLevelHeightText(level.heights[k], level.datum, level.top);
		}
		return suffix + "）";
	}

	bool noboribariSpan(const core::MemberCommand& member, const std::vector<PlanLevel>& levels,
						std::size_t story, double& from, double& to)
	{
		const std::vector<const PlanLevel*> storyLevels = storyPlanLevels(levels, story);
		if (storyLevels.empty())
			return false;
		const double top = core::memberTopZ(member);
		const double bottom = core::memberBottomZ(member);
		// 跨ぐ伏図レベル（下端 ≤ 高さ ≤ 上端）の通し番号の範囲。
		int low = 0;
		int high = 0;
		bool crosses = false;
		for (const PlanLevel* level : storyLevels)
		{
			const double height = level->height();
			if (bottom > height + kNoboribariLevelTol || top < height - kNoboribariLevelTol)
				continue;
			low = crosses ? std::min(low, level->ordinal) : level->ordinal;
			high = crosses ? std::max(high, level->ordinal) : level->ordinal;
			crosses = true;
		}
		// 母屋伏図: 上端がその階のいちばん上の伏図レベルの高さ以上。母屋伏図のレベルは
		// その階の母屋のレベル（moyaOrdinal）。
		const double moyaLevel = moyaOrdinal(levels, story);
		const bool moya = top >= storyLevels.back()->height() - kNoboribariLevelTol;
		if (crosses)
		{
			// 母屋伏図にも映るなら、いちばん上の伏図レベルも跨いでいる（high＝いちばん上）。
			from = low;
			to = moya ? moyaLevel : high;
			return true;
		}
		if (moya)
		{
			// いちばん上の伏図レベルより上にだけある: 母屋伏図だけ。
			from = moyaLevel;
			to = moyaLevel;
			return true;
		}
		// どの伏図レベルも跨がない（伏図レベルの間・いちばん下より下）: 低い側の端に近い
		// 伏図レベルの柱梁伏図に映す（従来どおり）。
		const PlanLevel* nearest =
			nearestPlanLevel(levels, story, std::min(member.elevation, member.endElevation));
		if (nearest == nullptr)
			return false;
		from = nearest->ordinal;
		to = nearest->ordinal;
		return true;
	}

	void assignMemberPlanLevels(std::vector<core::MemberCommand>& members,
								const std::vector<StoryInfo>& stories,
								const std::vector<PlanLevel>& levels)
	{
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const StoryInfo& story = stories[i];
			const std::string beamLayer = beamTopLayerName(i, story);
			const std::string noboribariLayer = storyLayerName(i, story.isTop, kLevelNoboribari);
			for (core::MemberCommand& member : members)
			{
				const bool beam = member.layer == beamLayer;
				if (!beam && member.layer != noboribariLayer)
					continue;
				// 軒桁は母屋伏図に薄く重ねるため、横架材レイヤから専用の "2±0-軒桁" へ分ける
				// （ご要望。core::kLevelNokigeta）。伏図レベルの分け方は横架材と同じ。
				const bool eaves = beam && member.drawClass == CLASS_NOKIGETA;
				// 水平な材は天端、傾いた材は水下（低い側の端）の天端で決める。
				const double z = std::min(member.elevation, member.endElevation);
				// 伏図レベルはどの階にも 1 つ以上ある（collectBeamHeights）。渡されなかった
				// 階の材は動かさない。
				const PlanLevel* level = nearestPlanLevel(levels, i, z);
				if (level == nullptr)
					continue;
				if (eaves)
					member.layer = planLevelLayer(*level, story, kLevelNokigeta);
				else if (beam)
					member.layer = planLevelBeamLayer(*level, story);
				else if (double from = 0.0, to = 0.0; noboribariSpan(member, levels, i, from, to))
				{
					// 登り梁は柱と同じく、映す伏図レベルの範囲の span レイヤ
					// （"2-1_2+1-登り梁"）へ。端の符号は材の階で書く（母屋伏図まで映す材の
					// 上端は母屋のレベル "2+1"）。高さ基準もそのレベルへ（高さは登り梁レベルと
					// 同じ。parse/Story）。
					member.layer = spanLayerName(ordinalLabel(levels, i, from),
												 ordinalLabel(levels, i, to), kLevelNoboribari);
					member.startBound.level = member.layer;
					member.endBound.level = member.layer;
				}
			}
		}
	}

	std::vector<core::PlanLevelChoice> collectPlanLevelChoices(Context& context)
	{
		const std::vector<StoryInfo>& stories = context.stories();
		// **まとめる前**の高さを並べる（ダイアログは高さ 1 つずつに「まとめる」を問う）。
		const std::vector<std::vector<long long>> heights =
			collectBeamHeights(stories, context.rawMembers());
		std::vector<core::PlanLevelChoice> choices;
		for (std::size_t i = 0; i < stories.size() && i < heights.size(); ++i)
		{
			for (std::size_t k = 0; k < heights[i].size(); ++k)
			{
				core::PlanLevelChoice choice;
				choice.key = core::PlanLevelKey{static_cast<int>(i), heights[i][k]};
				// ダイアログに出す高さは FL（最上階は軒高）から（core::planLevelHeightText）。
				choice.heightText = core::planLevelHeightText(
					heights[i][k], std::llround(stories[i].elevation), stories[i].isTop);
				choice.planTitle = floorPlanTitle(i, stories[i].isTop, stories.size());
				choice.canMerge = k > 0;
				choices.push_back(std::move(choice));
			}
		}
		return choices;
	}
} // namespace HomeskzIfcImport::parse
