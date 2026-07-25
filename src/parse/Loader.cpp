//
//	parse/Loader.cpp
//
//	IFC 読み込み＋サニタイズの実装。Python 版 ifc/loader.py に対応する。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	サニタイズは STEP テキストを文（トップレベルの ';' 区切り）単位に走査し、
//	除去対象の型（既定 IFCFOOTINGTYPE）のインスタンス文だけを落とす。文字列内の
//	';' は文末とみなさないよう、'' エスケープを考慮した文字列スキップを行う。
//

#include "parse/Loader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// サニタイズで除去する型名（大文字・完全一致）。ホームズ君 IFC2X3 に混入する
		// IFC4 専用エンティティ。増える場合はここに足す（Loader.h の docstring 参照）。
		constexpr std::array<std::string_view, 1> kDropTypes = {"IFCFOOTINGTYPE"};

		// 文 statement（例: "#12=IFCFOOTINGTYPE(...)"）が、除去対象型のインスタンス
		// 宣言かどうかを判定する。先頭の空白を飛ばし、'#' 数字 '=' 型名 の型名部分を
		// 大文字比較する。'#' で始まらない文（ヘッダ等）は決して除去しない。
		bool isDropStatement(std::string_view statement)
		{
			std::size_t pos = 0;
			while (pos < statement.size() &&
				   std::isspace(static_cast<unsigned char>(statement[pos])))
				++pos;
			if (pos >= statement.size() || statement[pos] != '#')
				return false;
			++pos;
			while (pos < statement.size() &&
				   std::isdigit(static_cast<unsigned char>(statement[pos])))
				++pos;
			while (pos < statement.size() &&
				   std::isspace(static_cast<unsigned char>(statement[pos])))
				++pos;
			if (pos >= statement.size() || statement[pos] != '=')
				return false;
			++pos;
			while (pos < statement.size() &&
				   std::isspace(static_cast<unsigned char>(statement[pos])))
				++pos;

			// 型名を大文字化しつつ読み取り、除去対象と突き合わせる。
			std::string name;
			while (pos < statement.size())
			{
				char const c = statement[pos];
				if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
				{
					name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
					++pos;
				}
				else
					break;
			}
			return std::ranges::any_of(kDropTypes,
									   [&name](std::string_view drop) { return name == drop; });
		}

		// ファイル全体を文字列に読み込む。開けなければ空文字列を返し ok=false。
		std::string readFile(const std::string& path, bool& ok)
		{
			const std::ifstream in(path, std::ios::binary);
			if (!in)
			{
				ok = false;
				return {};
			}
			std::ostringstream buffer;
			buffer << in.rdbuf();
			ok = true;
			return buffer.str();
		}
	} // namespace

	std::string sanitizeIfcText(const std::string& text)
	{
		std::string out;
		out.reserve(text.size());

		// 直近の文の開始位置。文末（トップレベル ';'）ごとに [start, i] を判定する。
		std::size_t start = 0;
		std::size_t i = 0;
		while (i < text.size())
		{
			char const c = text[i];
			if (c == '\'')
			{
				// 文字列は丸ごと読み飛ばす（内部の ';' は文末ではない）。'' は
				// 文字としての ' なので閉じ扱いしない。
				++i;
				while (i < text.size())
				{
					if (text[i] == '\'')
					{
						if (i + 1 < text.size() && text[i + 1] == '\'')
							i += 2;
						else
						{
							++i;
							break;
						}
					}
					else
						++i;
				}
			}
			else if (c == ';')
			{
				// [start, i] が 1 文（';' を含む）。除去対象でなければ出力へ写す。
				std::string_view const statement(text.data() + start, i - start + 1);
				if (!isDropStatement(statement))
					out.append(statement);
				++i;
				start = i;
			}
			else
				++i;
		}
		// 末尾に ';' で終わらない残り（通常は空白・改行のみ）はそのまま残す。
		out.append(text, start, text.size() - start);
		return out;
	}

	Model loadIfcFromText(const std::string& text)
	{
		return parseStep(sanitizeIfcText(text));
	}

	Model loadIfc(const std::string& path, bool* ok)
	{
		bool readOk = false;
		std::string const text = readFile(path, readOk);
		if (ok != nullptr)
			*ok = readOk;
		if (!readOk)
			return {};
		return loadIfcFromText(text);
	}
} // namespace HomeskzIfcImport::parse
