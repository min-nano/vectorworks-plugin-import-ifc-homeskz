//
//	ParseImportOptionsTests.cpp
//
//	取り込み設定（配置するシンボルの対応）が解析の結果まで届くかの単体テスト。
//	VectorWorks SDK を一切 include せず、無 SDK のテストハーネスで走る。
//
//	【何を守っているか】シンボル名は 5 つのモジュール（アンカーボルト・床束・火打・仕口・
//	伏図記号）がそれぞれ命令へ書き込む。設定を通す経路が 1 つでも欠けると「ダイアログで
//	選んだのに既定のまま置かれる」——絵を見ても気付きにくい壊れ方なので、**全フィクスチャの
//	通しで「既定名がひとつも残っていないこと」**を確かめる（docs/DEV-NOTES.md M20）。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "core/Progress.h"
#include "parse/BuildDocument.h"

#include <string>
#include <vector>

using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::ImportOptions;
using HomeskzIfcImport::core::NullProgressReporter;
using HomeskzIfcImport::core::SymbolCommand;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::symbolRoles;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixturePath;

namespace
{
	// 役割ごとに「見分けの付く」名前を与えた設定（既定名とは 1 文字も重ならない）。
	ImportOptions testOptions()
	{
		ImportOptions options;
		for (const auto& info : symbolRoles())
			options.setSymbol(info.role, "試験_" + std::to_string(static_cast<int>(info.role)));
		return options;
	}

	bool isDefaultName(const std::string& name)
	{
		for (const auto& info : symbolRoles())
			if (name == info.defaultSymbol)
				return true;
		return false;
	}
} // namespace

TEST(import_options_reach_every_symbol_command)
{
	const ImportOptions options = testOptions();
	for (const std::string& name : allFixtures())
	{
		NullProgressReporter progress;
		const Document document =
			HomeskzIfcImport::parse::buildDocument(fixturePath(name), progress, options);

		// シンボル置換系 4 種＋伏図記号。**どれも設定の名前だけ**になっている。
		const std::vector<const std::vector<SymbolCommand>*> lists = {
			&document.anchorBolts, &document.floorPosts, &document.fireBraces, &document.joints};
		for (const std::vector<SymbolCommand>* list : lists)
			for (const SymbolCommand& command : *list)
			{
				CHECK(!isDefaultName(command.symbol));
				CHECK(command.symbol.rfind("試験_", 0) == 0);
			}

		for (const auto& mark : document.columnMarks)
		{
			// 断面記号はシンボルを使わない（symbol は空）。伏図記号だけが名前を持つ。
			if (mark.symbol.empty())
				continue;
			CHECK(!isDefaultName(mark.symbol));
			CHECK(mark.symbol.rfind("試験_", 0) == 0);
		}
	}
}

TEST(default_options_keep_the_previous_names)
{
	// 設定を触らない取り込みは従来どおり（既定の Document と一致する）。
	bool sawSymbol = false;
	for (const std::string& name : allFixtures())
	{
		NullProgressReporter progress;
		const Document document =
			HomeskzIfcImport::parse::buildDocument(fixturePath(name), progress, ImportOptions{});
		const std::vector<const std::vector<SymbolCommand>*> lists = {
			&document.anchorBolts, &document.floorPosts, &document.fireBraces, &document.joints};
		for (const std::vector<SymbolCommand>* list : lists)
			for (const SymbolCommand& command : *list)
			{
				CHECK(isDefaultName(command.symbol));
				sawSymbol = true;
			}
	}
	CHECK(sawSymbol); // 1 つも無いなら、この確認は何も守っていない
}

TEST_MAIN();
