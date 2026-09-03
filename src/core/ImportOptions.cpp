//
//	core/ImportOptions.cpp
//
//	取り込み設定の実装（意図と規約は core/ImportOptions.h を参照）。役割の表はここ 1 つ。
//

#include "core/ImportOptions.h"

#include <array>
#include <cstddef>
#include <string>

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
} // namespace HomeskzIfcImport::core
