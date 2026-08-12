//
//	parse/ColumnMark.cpp
//
//	断面記号・伏図記号の解析の実装（parse/ColumnMark.h の意図・Python 版との差異は
//	そちらのヘッダを参照）。IFC は見ず、柱の命令だけから記号を組み立てる。
//

#include "parse/ColumnMark.h"
#include "parse/Story.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 柱 1 本の断面矩形（position を中心とする width×depth・軸平行）の 4 隅から、
		// 記号の対角線を作る。ホームズ君 IFC の柱はすべて軸平行の矩形断面で、命令にも
		// 回転を持たないので（core::ColumnCommand）、隅は中心 ± 半寸法で足りる。
		//
		// 小屋束は「／」＝左下→右上の 1 本、柱は「×」＝それに右下→左上を足した 2 本。
		std::vector<core::MarkSegment> markSegments(const core::ColumnCommand& column, bool cross)
		{
			const double halfWidth = column.width * 0.5;
			const double halfDepth = column.depth * 0.5;
			const double left = column.position.x - halfWidth;
			const double right = column.position.x + halfWidth;
			const double bottom = column.position.y - halfDepth;
			const double top = column.position.y + halfDepth;

			std::vector<core::MarkSegment> segments;
			segments.push_back({core::Vec2{left, bottom}, core::Vec2{right, top}});
			if (cross)
				segments.push_back({core::Vec2{right, bottom}, core::Vec2{left, top}});
			return segments;
		}
	} // namespace

	std::string planMarkLayerName(double toLevel)
	{
		return formatSpanLevel(toLevel) + "-" + kPlanMarkLayerSuffix;
	}

	std::vector<core::ColumnSectionMarkCommand>
	buildColumnSectionMarkCommands(const std::vector<core::ColumnCommand>& columns)
	{
		std::vector<core::ColumnSectionMarkCommand> commands;
		commands.reserve(columns.size());
		for (const core::ColumnCommand& column : columns)
		{
			// 断面が退化している柱には記号を作らない（縮退した線しか引けない）。
			if (column.width <= 0.0 || column.depth <= 0.0)
				continue;

			core::ColumnSectionMarkCommand command;
			command.layer = column.layer; // 柱と同じ span レイヤに重ねる
			command.drawClass = kSectionMarkClass;
			command.segments = markSegments(column, column.structuralUse != kStructuralUseKoyazuka);
			commands.push_back(std::move(command));
		}
		return commands;
	}

	std::vector<core::ColumnPlanMarkCommand>
	buildColumnPlanMarkCommands(const std::vector<core::ColumnCommand>& columns)
	{
		std::vector<core::ColumnPlanMarkCommand> commands;
		commands.reserve(columns.size());
		for (std::size_t i = 0; i < columns.size(); ++i)
		{
			const core::ColumnCommand& column = columns[i];

			// 配置先は span の to から決まる。span レイヤでない配置先の柱は記号を作らない
			// （ヘッダ「配置先レイヤ名は…」参照）。
			double from = 0.0;
			double to = 0.0;
			if (!parseSpanLayer(column.layer, from, to))
				continue;

			core::ColumnPlanMarkCommand command;
			command.layer = planMarkLayerName(to);
			command.styleName = column.structuralUse == kStructuralUseKoyazuka
									? kPlanMarkStyleKoyazuka
									: kPlanMarkStyleColumn;
			command.drawClass = kPlanMarkClass;
			command.columnIndex = i;
			command.position = column.position;
			commands.push_back(std::move(command));
		}
		return commands;
	}

	std::vector<PlanMarkLayer> collectPlanMarkLayers(const std::vector<ColumnSpan>& spans)
	{
		// spans は (from, to) 昇順なので to は単調ではない（"1to3" の次に "2to2.5" が来る）。
		// 同じ to をまとめたうえで to 昇順に並べ替える——伏図は「切断の直下で最大の to」を
		// 選ぶので、昇順に並んでいると走査が素直になる。
		std::vector<PlanMarkLayer> layers;
		for (const ColumnSpan& span : spans)
		{
			const std::string name = planMarkLayerName(span.to);
			bool known = false;
			for (const PlanMarkLayer& layer : layers)
				if (layer.layer == name)
				{
					known = true;
					break;
				}
			if (!known)
				layers.push_back({span.to, name});
		}
		std::ranges::sort(layers, [](const PlanMarkLayer& a, const PlanMarkLayer& b)
						  { return a.to < b.to; });
		return layers;
	}

	std::string planMarkLayerBelowCut(const std::vector<PlanMarkLayer>& layers, double cut)
	{
		// layers は to 昇順なので、cut を下回るうちの**最後**が「直下」になる。
		std::string best;
		for (const PlanMarkLayer& layer : layers)
		{
			if (layer.to >= cut)
				break;
			best = layer.layer;
		}
		return best;
	}
} // namespace HomeskzIfcImport::parse
