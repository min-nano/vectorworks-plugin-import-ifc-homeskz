//
//	parse/ColumnMark.cpp
//
//	断面記号・伏図記号の解析の実装（意図と規約は parse/ColumnMark.h を参照）。IFC は見ず、
//	柱の命令だけから記号を組み立てる。
//

#include "parse/ColumnMark.h"
#include "parse/Story.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// span レイヤの種別（構造用途）から伏図記号のシンボル名を選ぶ。span レイヤは単一種別
		// なので、そのレイヤの柱 1 本で決まる。
		const char* spanSymbol(const std::string& structuralUse)
		{
			return structuralUse == kStructuralUseKoyazuka ? kPlanMarkSymbolKoyazuka
														   : kPlanMarkSymbolColumn;
		}
	} // namespace

	std::string planMarkLayerName(double toLevel)
	{
		return formatSpanLevel(toLevel) + "-" + kPlanMarkLayerSuffix;
	}

	std::vector<core::ColumnMarkCommand>
	buildColumnMarkCommands(const std::vector<core::ColumnCommand>& columns)
	{
		// span レイヤ → そのレイヤの構造用途（単一種別なので最初に見つかった値でよい）。
		std::map<std::string, std::string> useByLayer;
		for (const core::ColumnCommand& column : columns)
			useByLayer.emplace(column.layer, column.structuralUse);

		const std::vector<ColumnSpan> spans = collectColumnSpans(columns);

		// 断面記号をすべて先に、続けて伏図記号をすべて。
		std::vector<core::ColumnMarkCommand> commands;
		commands.reserve(spans.size() * 2);
		for (const ColumnSpan& span : spans)
		{
			core::ColumnMarkCommand mark;
			mark.layer = span.layer; // 配置先＝その span レイヤ自身
			mark.drawClass = kSectionMarkClass;
			mark.targetLayer = span.layer;
			// targetClass は空＝全クラス（構造用途 4/5 で絞れるのでクラスまで指定する必要が無
			// く、クラス名を変えても記号が消えない）。
			mark.style = core::ColumnMarkStyle::Section;
			commands.push_back(std::move(mark));
		}
		for (const ColumnSpan& span : spans)
		{
			const auto use = useByLayer.find(span.layer);

			core::ColumnMarkCommand mark;
			mark.layer = planMarkLayerName(span.to); // 配置先＝専用の伏図記号レイヤ
			mark.drawClass = kPlanMarkClass;
			mark.targetLayer = span.layer;
			mark.style = core::ColumnMarkStyle::Plan;
			mark.symbol = spanSymbol(use != useByLayer.end() ? use->second : std::string());
			commands.push_back(std::move(mark));
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
