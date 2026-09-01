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
using HomeskzIfcImport::core::symbolRoles;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixturePath;
using HomeskzIfcTests::forEachFixtureDocument;

namespace
{
	// 差し替えたことが一目で分かる接頭辞（既定名とは 1 文字も重ならない）。
	constexpr const char* kTestPrefix = "試験_";

	// 役割ごとに見分けの付く名前を与えた設定。
	ImportOptions testOptions()
	{
		ImportOptions options;
		for (const auto& info : symbolRoles())
			options.setSymbol(info.role, std::string(kTestPrefix) +
											 std::to_string(static_cast<int>(info.role)));
		return options;
	}

	bool isDefaultName(const std::string& name)
	{
		for (const auto& info : symbolRoles())
			if (name == info.defaultSymbol)
				return true;
		return false;
	}

	// 命令セットの中で**シンボル名を持つ命令**をすべて回す（シンボル置換系 4 種＋伏図記号）。
	// 断面記号は絵ではなく実断面の対角線を引くので symbol が空——そこは飛ばす。
	template <class Body> void forEachSymbolName(const Document& document, Body&& body)
	{
		for (const std::vector<SymbolCommand>* list :
			 {&document.anchorBolts, &document.floorPosts, &document.fireBraces, &document.joints})
			for (const SymbolCommand& command : *list)
				body(command.symbol);
		for (const auto& mark : document.columnMarks)
			if (!mark.symbol.empty())
				body(mark.symbol);
	}
} // namespace

TEST(import_options_reach_every_symbol_command)
{
	// 設定を与えた解析は共有キャッシュ（fixtureDocument は既定の設定で組む）を通せないので、
	// ここだけはフィクスチャごとに組み立て直す。
	const ImportOptions options = testOptions();
	for (const std::string& name : allFixtures())
	{
		NullProgressReporter progress;
		const Document document =
			HomeskzIfcImport::parse::buildDocument(fixturePath(name), progress, options);
		forEachSymbolName(document,
						  [&](const std::string& symbol)
						  {
							  CHECK(!isDefaultName(symbol));
							  CHECK(symbol.starts_with(kTestPrefix));
						  });
	}
}

TEST(default_options_keep_the_previous_names)
{
	// 設定を触らない取り込みは従来どおり（＝共有キャッシュの命令セットがそのまま既定名）。
	bool sawSymbol = false;
	forEachFixtureDocument(
		[&](const std::string&, const Document& document)
		{
			forEachSymbolName(document,
							  [&](const std::string& symbol)
							  {
								  CHECK(isDefaultName(symbol));
								  sawSymbol = true;
							  });
		});
	CHECK(sawSymbol); // 1 つも無いなら、この確認は何も守っていない
}

TEST_MAIN();
