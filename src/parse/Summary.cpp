//
//	parse/Summary.cpp
//
//	IFC 読み取りサマリの実装。Model の型別インデックス（parse/Step の byType）を数え、
//	結果を日本語テキストへ整形する。【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Summary.h"
#include "parse/Loader.h"

#include <array>
#include <sstream>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// ホームズ君 IFC が使う主要エンティティ型の一覧（表示順）。key は byType へ渡す
		// 大文字の型名（parse/Step は型名を常に大文字で保持する）、displayType は
		// ダイアログに出すキャメルケース名、label はホームズ君での役割を表す日本語。
		// 対応: CLAUDE.md「移植の基本方針」が挙げる IfcGridAxis / IfcBeam / IfcColumn /
		// IfcFooting / IfcSlab / IfcBuildingStorey / IfcMechanicalFastener。
		struct TypeDef
		{
			const char* key;		 // byType のキー（大文字）
			const char* displayType; // 表示用 IFC 型名
			const char* label;		 // 日本語ラベル
		};

		constexpr std::array<TypeDef, 7> kTypes = {{
			{"IFCGRIDAXIS", "IfcGridAxis", "通り芯"},
			{"IFCBUILDINGSTOREY", "IfcBuildingStorey", "階（ストーリ）"},
			{"IFCBEAM", "IfcBeam", "横架材（梁・桁・土台）"},
			{"IFCCOLUMN", "IfcColumn", "柱・束"},
			{"IFCFOOTING", "IfcFooting", "基礎"},
			{"IFCSLAB", "IfcSlab", "スラブ・床版"},
			{"IFCMECHANICALFASTENER", "IfcMechanicalFastener", "金物"},
		}};
	} // namespace

	IfcSummary summarizeModel(const Model& model)
	{
		IfcSummary summary;
		summary.loaded = true;
		summary.entityCount = model.size();
		summary.counts.reserve(kTypes.size());
		for (const TypeDef& def : kTypes)
		{
			summary.counts.push_back(
				IfcTypeCount{def.displayType, def.label, model.byType(def.key).size()});
		}
		return summary;
	}

	IfcSummary summarizeIfc(const std::string& path)
	{
		bool ok = false;
		const Model model = loadIfc(path, &ok);
		if (!ok)
			return IfcSummary{}; // loaded=false・counts 空（読み込み失敗）
		return summarizeModel(model);
	}

	std::string formatSummary(const IfcSummary& summary)
	{
		if (!summary.loaded)
			return "IFC ファイルを読み込めませんでした。";

		std::ostringstream out;
		out << "IFC を読み込みました。検出した要素:\n";
		for (const IfcTypeCount& c : summary.counts)
			out << "\n  " << c.label << " (" << c.ifcType << "): " << c.count;
		out << "\n\nエンティティ総数: " << summary.entityCount;
		return out.str();
	}
} // namespace HomeskzIfcImport::parse
