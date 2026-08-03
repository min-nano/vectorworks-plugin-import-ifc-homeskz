//
//	parse/StructuralClass.cpp
//
//	構造クラス判定の実装。Python 版 ifc/structural_class.py に対応する。
//	【SDK 非依存・幾何非依存】純粋な文字列／整数ロジックだけで完結する（core/parse の
//	他モジュールにも依存しない。ヘッダの <optional> / <string> のみ）。
//

#include "parse/StructuralClass.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 小屋束を識別する Name の接頭辞（Python 版 COLUMN_KOYAZUKA_NAME_PREFIX）。
		// ObjectType による判定（kStandColumnObjectType）はヘッダ側にあり、parse/Column の
		// 柱種別名の変換と共有する。
		constexpr const char* kKoyazukaNamePrefix = "小屋束";

		// 文字列を区切り文字 delim で分割する（Python の str.split(delim) と同じ挙動）。
		// 空文字は要素 1 個の [""] を返す（Python の ''.split(':') == [''] に合わせる。
		// これにより memberTypeOfName("") が空文字を返す）。delim は ASCII 1 文字（':'）で、
		// UTF-8 では継続バイトに現れないため日本語トークンを壊さず分割できる。
		std::vector<std::string> split(const std::string& text, char delim)
		{
			std::vector<std::string> parts;
			std::string current;
			for (const char c : text)
			{
				if (c == delim)
				{
					parts.push_back(current);
					current.clear();
				}
				else
				{
					current.push_back(c);
				}
			}
			parts.push_back(current);
			return parts;
		}

		// text が prefix で始まるか（Python の str.startswith 相当）。
		bool startsWith(const std::string& text, const std::string& prefix)
		{
			return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
		}
	} // namespace

	std::string memberTypeOfName(const std::string& name)
	{
		// "木梁:{種別}:{連番}" は中央（parts[1]）の種別を、それ以外は接頭辞（parts[0]）を返す。
		const std::vector<std::string> parts = split(name, ':');
		if (parts.size() >= 3 && parts[0] == "木梁")
			return parts[1];
		return parts[0];
	}

	std::optional<std::string> memberClassFromName(const std::string& name)
	{
		// IFC Name の種別トークン → 横架材クラス（ホームズ君 IFC の記録を信用する直接対応）。
		// 床小梁・床大梁・甲乙梁はいずれも床組の梁なので床梁クラスにまとめる。Python 版
		// _MEMBER_CLASS_BY_TYPE と一致させる。直接対応が無ければ std::nullopt。
		const std::string type = memberTypeOfName(name);
		if (type == "土台")
			return CLASS_DODAI;
		if (type == "大引")
			return CLASS_OOBIKI;
		if (type == "根太")
			return CLASS_NEDA;
		if (type == "軒桁")
			return CLASS_NOKIGETA;
		if (type == "胴差")
			return CLASS_DOUSASHI;
		if (type == "床小梁" || type == "床大梁" || type == "甲乙梁")
			return CLASS_YUKABARI;
		if (type == "小屋梁")
			return CLASS_KOYABARI;
		if (type == "母屋")
			return CLASS_MOYA;
		if (type == "棟木")
			return CLASS_MUNAGI;
		if (type == "登り梁")
			return CLASS_NOBORIBARI;
		return std::nullopt;
	}

	std::string resolveMemberClass(const std::string& name, int index, int topIndex,
								   bool aboveEaves)
	{
		// 名前で判別できればそれを信用する。
		if (const std::optional<std::string> cls = memberClassFromName(name))
			return *cls;
		// 判別できない部材は階と高さで推定する（Python 版 resolve_member_class と同じ順序）。
		if (index >= topIndex)
			// 最上階（屋根）: 軒高付近は小屋梁、それより高ければ母屋。
			return aboveEaves ? CLASS_MOYA : CLASS_KOYABARI;
		if (index <= 0)
			return CLASS_DODAI;
		return CLASS_YUKABARI;
	}

	std::string resolveColumnClass(const std::string& objectType, const std::string& name,
								   int index, int topIndex, bool isThrough)
	{
		if (objectType == kStandColumnObjectType || startsWith(name, kKoyazukaNamePrefix) ||
			index >= topIndex)
			return CLASS_KOYAZUKA;
		return isThrough ? CLASS_TOSHIBASHIRA : CLASS_KUDABASHIRA;
	}
} // namespace HomeskzIfcImport::parse
