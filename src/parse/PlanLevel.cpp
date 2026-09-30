//
//	parse/PlanLevel.cpp
//
//	伏図レベルの実装（意図は parse/PlanLevel.h）。【SDK 非依存】ここでは VectorWorks SDK を
//	include しない。
//

#include "parse/PlanLevel.h"
#include "parse/Context.h"
#include "parse/Sheet.h"

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
				levels[k].tag = levels[k].standard ? std::string()
												   : core::planLevelTag(levels[k].heights.front());
			}
		}
		return levels;
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
		const auto whole = static_cast<int>(std::floor(ordinal));
		for (const PlanLevel& level : levels)
		{
			if (level.ordinal == whole)
				return level.story;
		}
		return whole > 0 ? static_cast<std::size_t>(whole - 1) : 0;
	}

	std::string planLevelType(const PlanLevel& level, const std::string& levelType)
	{
		return levelType + level.tag;
	}

	std::string planLevelLayer(const PlanLevel& level, const StoryInfo& story,
							   const std::string& levelType)
	{
		return storyLayerName(level.story, story.isTop, planLevelType(level, levelType));
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
			suffix += core::planLevelHeightText(level.heights[k]);
		}
		return suffix + "）";
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
				// 水平な材は天端、傾いた材は水下（低い側の端）の天端で決める。
				const double z = std::min(member.elevation, member.endElevation);
				const PlanLevel* level = nearestPlanLevel(levels, i, z);
				if (level == nullptr)
					continue;
				// 登り梁の専用レイヤの材も水下側の伏図レベルへ（その伏図に映すため）。
				// 母屋伏図は階の登り梁レイヤを全部映す（parse/Sheet）。
				member.layer = beam ? planLevelBeamLayer(*level, story)
									: planLevelLayer(*level, story, kLevelNoboribari);
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
				choice.planTitle = floorPlanTitle(i, stories[i].isTop, stories.size());
				choice.canMerge = k > 0;
				choices.push_back(std::move(choice));
			}
		}
		return choices;
	}
} // namespace HomeskzIfcImport::parse
