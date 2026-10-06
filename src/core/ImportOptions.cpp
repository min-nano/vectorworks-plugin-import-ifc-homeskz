//
//	core/ImportOptions.cpp
//
//	取り込み設定の実装（意図と規約は core/ImportOptions.h を参照）。役割の表はここ 1 つ。
//	図面枠のスタイルは表を持たない（既定名が無く、選択肢は図面から集める。M28）。
//

#include "core/ImportOptions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
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

		// 全角の数字・小数点（UTF-8 で EF BC 90〜99 / EF BC 8E）を半角へ読み替える。
		// それ以外の文字はそのまま残す（後段が数字列でないとして受け付けない）。
		std::string toHalfWidthDigits(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			for (std::size_t i = 0; i < text.size(); ++i)
			{
				const auto lead = static_cast<unsigned char>(text[i]);
				if (lead == 0xEF && i + 2 < text.size() &&
					static_cast<unsigned char>(text[i + 1]) == 0xBC)
				{
					const auto tail = static_cast<unsigned char>(text[i + 2]);
					if (tail >= 0x90 && tail <= 0x99)
					{
						out.push_back(static_cast<char>('0' + (tail - 0x90)));
						i += 2;
						continue;
					}
					if (tail == 0x8E)
					{
						out.push_back('.');
						i += 2;
						continue;
					}
				}
				out.push_back(text[i]);
			}
			return out;
		}
	} // namespace

	bool isValidRafterSize(double mm)
	{
		return std::isfinite(mm) && mm > 0.0 && mm <= kMaxRafterSize;
	}

	std::optional<double> parseRafterSize(const std::string& text)
	{
		const std::string half = toHalfWidthDigits(text);
		std::size_t begin = 0;
		std::size_t end = half.size();
		while (begin < end && (half[begin] == ' ' || half[begin] == '\t'))
			++begin;
		while (end > begin && (half[end - 1] == ' ' || half[end - 1] == '\t'))
			--end;
		if (begin == end)
			return std::nullopt;

		double whole = 0.0;
		double fraction = 0.0;
		double scale = 1.0;
		bool seenDot = false;
		bool seenDigit = false;
		for (std::size_t i = begin; i < end; ++i)
		{
			const char c = half[i];
			if (c == '.')
			{
				if (seenDot)
					return std::nullopt; // 小数点は 1 つまで
				seenDot = true;
				continue;
			}
			if (c < '0' || c > '9')
				return std::nullopt;
			seenDigit = true;
			const auto digit = static_cast<double>(c - '0');
			if (seenDot)
			{
				scale /= 10.0;
				fraction += digit * scale;
			}
			else
			{
				whole = (whole * 10.0) + digit;
			}
		}
		if (!seenDigit)
			return std::nullopt;
		const double value = whole + fraction;
		if (!isValidRafterSize(value))
			return std::nullopt;
		return value;
	}

	std::string formatRafterSize(double mm)
	{
		// 0.1mm 単位に丸めた整数で組み立てる（浮動小数の既定の書式に頼らない）。
		const long long tenths = std::llround(mm * 10.0);
		std::string text = std::to_string(tenths / 10);
		const long long rest = tenths % 10;
		if (rest != 0)
			text += "." + std::to_string(rest < 0 ? -rest : rest);
		return text;
	}

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
		// 追加されても正しく答える。数は通りの本数＝数十まで）。
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

	void ImportOptions::setRafterSize(double width, double height)
	{
		rafterWidth = isValidRafterSize(width) ? width : kDefaultRafterWidth;
		rafterHeight = isValidRafterSize(height) ? height : kDefaultRafterHeight;
	}
	std::size_t defaultDimensionStandardIndex(const std::vector<std::string>& names)
	{
		const auto jis = std::ranges::find(names, "JIS");
		return jis == names.end() ? 0 : static_cast<std::size_t>(jis - names.begin());
	}

	ImportOptions presetImportOptions(const std::vector<std::string>& symbolNames,
									  const std::vector<std::string>& titleBlockStyles,
									  const std::vector<std::string>& dimensionStandards)
	{
		ImportOptions options;
		for (const SymbolRoleInfo& info : symbolRoles())
		{
			const bool present =
				std::ranges::find(symbolNames, info.defaultSymbol) != symbolNames.end();
			options.setEnabled(info.role, present);
		}
		if (!titleBlockStyles.empty())
			options.setTitleBlockStyle(titleBlockStyles.front());
		if (!dimensionStandards.empty())
			options.setDimensionStandard(
				dimensionStandards[defaultDimensionStandardIndex(dimensionStandards)]);
		return options;
		// 閉じ括弧は setter が例外を投げたときの後始末（options の破棄）にしか通らず、
		// テストでは通らない（gcov の "====="。parse/ShearWall.cpp と同じ）。
	} // GCOVR_EXCL_LINE
} // namespace HomeskzIfcImport::core
