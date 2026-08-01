//
//	parse/Context.cpp
//
//	共有コンテキストの実装。各アクセサは「まだ無ければ計算して覚え、以後はそれを返す」
//	だけで、計算そのものは従来どおり各 parse モジュールの関数が行う（振る舞いを変えない）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Context.h"

#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	const std::vector<StoryInfo>& Context::stories()
	{
		if (!fStories.has_value())
			fStories = collectStories(*this);
		return *fStories;
	}

	const std::vector<GridLine>& Context::gridLines()
	{
		if (!fGridLines.has_value())
			fGridLines = collectGridLines(*fModel);
		return *fGridLines;
	}

	const core::Vec2& Context::gridCenter()
	{
		if (!fGridCenter.has_value())
		{
			// 通り芯が 1 本も取れなければ補正なし＝生の IFC 座標（各要素の従来の挙動と同じ）。
			core::Vec2 center{0.0, 0.0};
			gridCenterOf(gridLines(), center);
			fGridCenter = center;
		}
		return *fGridCenter;
	}

	const std::vector<int>& Context::storyElements(int storeyId)
	{
		const auto found = fStoryElements.find(storeyId);
		if (found != fStoryElements.end())
			return found->second;
		return fStoryElements.emplace(storeyId, collectStoryElements(*fModel, storeyId))
			.first->second;
	}

	const std::vector<LoftFloorRegion>& Context::loftFloorRegions(int storeyId)
	{
		const auto found = fLoftFloorRegions.find(storeyId);
		if (found != fLoftFloorRegions.end())
			return found->second;
		// 先に計算してから emplace する（loftFloorRegions は storyElements を引くので、
		// 計算中に別のキャッシュが埋まってもこの map は触られない）。
		std::vector<LoftFloorRegion> regions = parse::loftFloorRegions(*this, storeyId);
		return fLoftFloorRegions.emplace(storeyId, std::move(regions)).first->second;
	}

	const RoofPlane* Context::roofPlane(int elementId)
	{
		auto found = fRoofPlanes.find(elementId);
		if (found == fRoofPlanes.end())
		{
			// 解決できなかったことも覚える（同じ屋根版で 2 度目の解決を試みない）ので、
			// 空の optional もそのまま格納する。
			std::optional<RoofPlane> resolved;
			RoofPlane plane;
			if (parse::roofPlane(*fModel, fModel->entity(elementId), plane))
				resolved = std::move(plane);
			found = fRoofPlanes.emplace(elementId, std::move(resolved)).first;
		}

		const std::optional<RoofPlane>& cached = found->second;
		if (!cached.has_value())
			return nullptr;
		return &cached.value();
	}

	const std::vector<core::MemberCommand>& Context::members()
	{
		if (!fMembers.has_value())
			fMembers = buildMemberCommands(*this);
		return *fMembers;
	}
} // namespace HomeskzIfcImport::parse
