//
//	parse/Loader.cpp
//
//	IFC 読み込みの実装。ファイルを文字列に読み込み、最小 STEP リーダ（parse/Step）
//	でエンティティグラフへ変換する。**サニタイズ（非正規エンティティの除去）は
//	行わない**（理由は Loader.h の docstring 参照）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Loader.h"

#include <fstream>
#include <sstream>

namespace HomeskzIfcImport::parse
{
	namespace
	{
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

	Model loadIfcFromText(const std::string& text)
	{
		// 自前リーダは非正規エンティティも読めるので、テキストをそのまま解析する。
		return parseStep(text);
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
