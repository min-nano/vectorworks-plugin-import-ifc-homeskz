//
//	core/ImportOptions.cpp
//
//	取り込み設定の実装（意図と規約は core/ImportOptions.h を参照）。役割の表はここ 1 つ。
//	図面枠のスタイルは表を持たない（既定名が無く、選択肢は図面から集める。M28）。
//

#include "core/ImportOptions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 添字と enum の値がずれていないことを、表を引くたびに確かめずに済ませるための
		// 変換。SymbolRole は std::size_t を基底に持つ（core/ImportOptions.h）。
		std::size_t indexOf(SymbolRole role)
		{
			return static_cast<std::size_t>(role);
		}
	} // namespace

	const std::array<SymbolRoleInfo, kSymbolRoleCount>& symbolRoles()
	{
		// **既定のシンボル名は「この設定を入れる前に解析側が固定で書いていた名前」**で、
		// 変えると既定の取り込み結果が変わる。ホームズ君のテンプレート／リソースライブラリが
		// 供給するハイブリッドシンボルの名前そのもの（draw/Symbol.cpp「シンボル定義は
		// プラグインが作らない」）。
		static const std::array<SymbolRoleInfo, kSymbolRoleCount> kRoles = {{
			{SymbolRole::AnchorBoltM12, "アンカーボルト（座金付き）", "アンカーボルト_M12"},
			{SymbolRole::AnchorBoltM16, "アンカーボルト（座金なし）", "アンカーボルト_M16"},
			{SymbolRole::FloorPost, "床束", "床束"},
			{SymbolRole::FireBrace, "火打", "鋼製火打"},
			{SymbolRole::Joint, "仕口", "仕口"},
			{SymbolRole::PlanMarkColumn, "伏図記号（柱）", "柱伏図記号"},
			{SymbolRole::PlanMarkKoyazuka, "伏図記号（小屋束）", "束伏図記号"},
			{SymbolRole::Splice, "継手", "継手"},
		}};
		return kRoles;
	}

	const char* defaultSymbolName(SymbolRole role)
	{
		return symbolRoles()[indexOf(role)].defaultSymbol;
	}

	const char* symbolRoleLabel(SymbolRole role)
	{
		return symbolRoles()[indexOf(role)].label;
	}

	ImportOptions::ImportOptions()
	{
		for (const SymbolRoleInfo& info : symbolRoles())
		{
			symbols[indexOf(info.role)] = info.defaultSymbol;
			enabled[indexOf(info.role)] = true; // 既定は全要素を取り込む（従来どおり）
		}
	}

	const std::string& ImportOptions::symbol(SymbolRole role) const
	{
		return symbols[indexOf(role)];
	}

	bool ImportOptions::isEnabled(SymbolRole role) const
	{
		return enabled[indexOf(role)];
	}

	void ImportOptions::setSymbol(SymbolRole role, const std::string& name)
	{
		symbols[indexOf(role)] = name.empty() ? defaultSymbolName(role) : name;
	}

	void ImportOptions::setEnabled(SymbolRole role, bool enable)
	{
		enabled[indexOf(role)] = enable;
	}

	const std::string& ImportOptions::titleBlockStyle() const
	{
		return titleBlock;
	}

	bool ImportOptions::hasTitleBlock() const
	{
		return !titleBlock.empty();
	}

	void ImportOptions::setTitleBlockStyle(const std::string& name)
	{
		// **空はそのまま入れる**（＝置かない）。既定名が無いので、空を何かへ読み替える
		// 余地が無い（core/ImportOptions.h の titleBlock）。
		titleBlock = name;
	}

	const std::string& ImportOptions::dimensionStandard() const
	{
		return dimension;
	}

	bool ImportOptions::hasDimensions() const
	{
		return !dimension.empty();
	}

	void ImportOptions::setDimensionStandard(const std::string& name)
	{
		// 図面枠と同じく**空はそのまま入れる**（＝寸法を入れない）。
		dimension = name;
	}

	bool ImportOptions::mergesWithPrevious(const PlanLevelKey& key) const
	{
		return std::ranges::binary_search(mergedPlanLevels, key);
	}

	void ImportOptions::setMergeWithPrevious(const PlanLevelKey& key, bool merge)
	{
		// 昇順・重複なしを保つ（mergesWithPrevious が二分探索する）。
		const auto at = std::ranges::lower_bound(mergedPlanLevels, key);
		const bool present = at != mergedPlanLevels.end() && *at == key;
		if (merge && !present)
			mergedPlanLevels.insert(at, key);
		else if (!merge && present)
			mergedPlanLevels.erase(at);
	}

	bool ImportOptions::isSectionSkipped(const std::string& drawingNumber) const
	{
		// 並びに頼らず線形に探す（フィールドは公開なので、setSkippedSections を通さずに
		// 足されても正しく答える。数は通りの本数＝数十まで）。
		return std::find(skippedSections.begin(), skippedSections.end(), drawingNumber) !=
			   skippedSections.end();
	}

	void ImportOptions::setSkippedSections(const std::vector<std::string>& drawingNumbers)
	{
		std::vector<std::string> names;
		for (const std::string& name : drawingNumbers)
		{
			if (!name.empty())
				names.push_back(name);
		}
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		skippedSections = std::move(names);
	}
} // namespace HomeskzIfcImport::core
