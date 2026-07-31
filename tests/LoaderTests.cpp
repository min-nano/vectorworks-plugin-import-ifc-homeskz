//
//	LoaderTests.cpp
//
//	IFC 読み込み（src/parse/Loader）の単体テスト。無 SDK のテストハーネスで走る
//	（CLAUDE.md「テスト方針」）。自前 STEP リーダはスキーマ検証をしないので、
//	非正規エンティティ（IFC2X3 に混入する IFC4 専用 IFCFOOTINGTYPE 等）を除去せず
//	そのまま読める——ことと、実フィクスチャのファイル読み込みを確かめる。
//	フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"
#include "Fixtures.h"

#include "parse/Loader.h"

#include <string>

using namespace HomeskzIfcImport::parse;

// ---------------------------------------------------------------------------
// 非正規エンティティを除去せずに読む（サニタイズしない）
// ---------------------------------------------------------------------------

TEST(loads_nonstandard_entity_without_stripping)
{
	// IFC2X3 に無い IFC4 専用 IFCFOOTINGTYPE が混入していても、ifcopenshell と違い
	// 自前リーダは中断せずそのまま読む（本プラグインは参照しないので無害）。
	std::string const text = "DATA;\n"
							 "#1=IFCFOOTINGTYPE('g',$,'FT1',$);\n"
							 "#2=IFCGRIDAXIS('X1',#3,.T.);\n"
							 "#3=IFCPOLYLINE((#4,#5));\n"
							 "ENDSEC;\n";
	Model const model = loadIfcFromText(text);
	CHECK_EQ(model.byType("IFCFOOTINGTYPE").size(), static_cast<std::size_t>(1));
	CHECK_EQ(model.byType("IFCGRIDAXIS").size(), static_cast<std::size_t>(1));
	CHECK_EQ(model.byType("IFCPOLYLINE").size(), static_cast<std::size_t>(1));
}

TEST(reference_to_nonstandard_entity_resolves)
{
	// IFCFOOTINGTYPE を参照する IFCRELDEFINESBYTYPE の参照は、除去しないので
	// ちゃんと当該エンティティに解決する（宙ぶらりんにならない）。
	std::string const text = "#1=IFCFOOTINGTYPE('g',$,'FT1',$);\n"
							 "#2=IFCRELDEFINESBYTYPE('r',$,$,$,(#3),#1);\n";
	Model const model = loadIfcFromText(text);
	const Entity* rel = model.entity(2);
	CHECK(rel != nullptr);
	const Value& typeRef = rel->attribute(5);
	CHECK(typeRef.isReference());
	CHECK_EQ(typeRef.reference, 1);
	const Entity* footingType = model.resolve(typeRef);
	CHECK(footingType != nullptr);
	CHECK_EQ(footingType->type, std::string("IFCFOOTINGTYPE"));
}

TEST(unresolved_reference_is_tolerated)
{
	// 実在しない #id への参照は寛容に nullptr。読み込み自体は継続する。
	Model const model = loadIfcFromText("#2=IFCRELDEFINESBYTYPE('r',$,$,$,(#3),#999);\n");
	const Entity* rel = model.entity(2);
	CHECK(rel != nullptr);
	CHECK(model.resolve(rel->attribute(5)) == nullptr);
}

// ---------------------------------------------------------------------------
// 実フィクスチャのファイル読み込み
// ---------------------------------------------------------------------------

TEST(loads_fixture_file_and_reads_grid)
{
	std::string const path = std::string(HOMESKZ_FIXTURES_DIR) + "/minimal_grid.ifc";
	bool ok = false;
	Model const model = loadIfc(path, &ok);
	CHECK(ok);

	// 3 本の通り芯（IFCGRIDAXIS）と 3 本のポリライン、4 点、1 階。
	CHECK_EQ(model.byType("IFCGRIDAXIS").size(), static_cast<std::size_t>(3));
	CHECK_EQ(model.byType("IFCPOLYLINE").size(), static_cast<std::size_t>(3));
	CHECK_EQ(model.byType("IFCCARTESIANPOINT").size(), static_cast<std::size_t>(4));
	CHECK_EQ(model.byType("IFCBUILDINGSTOREY").size(), static_cast<std::size_t>(1));

	// 非正規エンティティ IFCFOOTINGTYPE も除去されず存在する。
	CHECK_EQ(model.byType("IFCFOOTINGTYPE").size(), static_cast<std::size_t>(1));

	// 通り芯の軸曲線（属性1）をたどって端点座標まで解決できる。
	const std::vector<int>& axes = model.byType("IFCGRIDAXIS");
	const Entity* axis = model.entity(axes[0]);
	CHECK(axis != nullptr);
	const Entity* poly = model.resolve(axis->attribute(1));
	CHECK(poly != nullptr);
	CHECK_EQ(poly->type, std::string("IFCPOLYLINE"));
	CHECK_EQ(poly->attribute(0).items.size(), static_cast<std::size_t>(2));
	const Entity* start = model.resolve(poly->attribute(0).items[0]);
	CHECK(start != nullptr);
	CHECK_EQ(start->type, std::string("IFCCARTESIANPOINT"));
}

TEST(missing_file_reports_not_ok)
{
	bool ok = true;
	Model const model = loadIfc(std::string(HOMESKZ_FIXTURES_DIR) + "/does_not_exist.ifc", &ok);
	CHECK(!ok);
	CHECK_EQ(model.size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Python 版から流用した実 IFC フィクスチャの読み込み
// ---------------------------------------------------------------------------
//
//	姉妹リポジトリ（Python 版）の tests/fixtures/ から流用したホームズ君 EX 出力の
//	実 IFC 群（tests/fixtures/README.md）を、自前 STEP リーダで丸ごと読めることを
//	確かめる。要素ごとの解析（通り芯・横架材…）は今後のマイルストーンで parse
//	モジュールのテストとして個別に検証するため、ここでは「実データを中断せず読み切り、
//	期待するエンティティ型が存在する」ことだけを確認する（CLAUDE.md「小さく機能追加」）。

TEST(loads_all_homeskz_fixtures)
{
	// フィクスチャの一覧は tests/Fixtures.h が唯一の定義（各テストが独自の一覧を持つと、
	// フィクスチャを足したときに一部のテストだけ素通りする）。
	for (const std::string& name : HomeskzIfcTests::allFixtures())
	{
		bool ok = false;
		Model const model = HomeskzIfcTests::fixture(name, ok);
		CHECK(ok);
		// 実データは数千エンティティ規模。空でないことを確かめる。
		CHECK(model.size() > 0);
		// ホームズ君 IFC の骨格をなす型が存在する（通り芯・ストーリ・横架材）。
		CHECK(!model.byType("IFCBUILDINGSTOREY").empty());
		CHECK(!model.byType("IFCGRIDAXIS").empty());
		CHECK(!model.byType("IFCBEAM").empty());
	}
}

TEST_MAIN();
