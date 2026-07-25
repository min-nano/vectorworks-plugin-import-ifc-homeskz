//
//	LoaderTests.cpp
//
//	IFC 読み込み＋サニタイズ（src/parse/Loader）の単体テスト。無 SDK のテスト
//	ハーネスで走る（CLAUDE.md「テスト方針」）。テキストベースのサニタイズと、
//	実フィクスチャ（tests/fixtures/minimal_grid.ifc）のファイル読み込みの両方を
//	確かめる。フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "TestFramework.h"

#include "parse/Loader.h"

#include <string>

using namespace HomeskzIfcImport::parse;

// ---------------------------------------------------------------------------
// サニタイズ: IFCFOOTINGTYPE の除去（IFC4 専用エンティティ）
// ---------------------------------------------------------------------------

TEST(sanitize_drops_footing_type_statement)
{
	std::string const text = "#1=IFCFOOTINGTYPE('g',$,'FT1',$,$,$,$,$,$,.PAD_FOOTING.);\n"
							 "#2=IFCCARTESIANPOINT((0.,0.,0.));\n";
	std::string const clean = sanitizeIfcText(text);
	// IFCFOOTINGTYPE 文は消え、他は残る。
	CHECK(clean.find("IFCFOOTINGTYPE") == std::string::npos);
	CHECK(clean.find("IFCCARTESIANPOINT") != std::string::npos);
}

TEST(sanitize_is_case_insensitive_on_type_name)
{
	std::string const text = "#1=ifcfootingtype('g',$,'FT1',$);\n#2=IFCWALL($);\n";
	std::string const clean = sanitizeIfcText(text);
	CHECK(clean.find("footingtype") == std::string::npos);
	CHECK(clean.find("IFCWALL") != std::string::npos);
}

TEST(sanitize_keeps_similarly_named_types)
{
	// IFCFOOTING（型ではなく実体）は残す。前方一致で誤除去しないこと。
	std::string const text = "#1=IFCFOOTING('g',$,'F1',$,$,$,$);\n";
	std::string const clean = sanitizeIfcText(text);
	CHECK(clean.find("IFCFOOTING(") != std::string::npos);
}

TEST(sanitize_does_not_split_on_semicolon_inside_string)
{
	// 文字列内の ';' を文末と誤認しないこと。IFCFOOTINGTYPE 文の直前に、';' を
	// 含む文字列を持つ別の文があっても、除去は当該文だけに限る。
	std::string const text = "#1=IFCLABEL('a;b;c');\n"
							 "#2=IFCFOOTINGTYPE('g',$,'FT;1',$);\n"
							 "#3=IFCWALL($);\n";
	std::string const clean = sanitizeIfcText(text);
	CHECK(clean.find("IFCLABEL('a;b;c')") != std::string::npos);
	CHECK(clean.find("IFCFOOTINGTYPE") == std::string::npos);
	CHECK(clean.find("IFCWALL") != std::string::npos);
}

TEST(sanitize_leaves_ordinary_text_unchanged)
{
	std::string const text = "#1=IFCWALL($);\n#2=IFCSLAB($);\n";
	CHECK_EQ(sanitizeIfcText(text), text);
}

// ---------------------------------------------------------------------------
// テキストからのロード（サニタイズ込み）とグラフ整合
// ---------------------------------------------------------------------------

TEST(load_from_text_sanitizes_and_parses)
{
	std::string const text = "DATA;\n"
							 "#1=IFCFOOTINGTYPE('g',$,'FT1',$);\n"
							 "#2=IFCGRIDAXIS('X1',#3,.T.);\n"
							 "#3=IFCPOLYLINE((#4,#5));\n"
							 "ENDSEC;\n";
	Model const model = loadIfcFromText(text);
	// 除去後: IFCFOOTINGTYPE は Model に存在しない。
	CHECK(model.byType("IFCFOOTINGTYPE").empty());
	CHECK_EQ(model.byType("IFCGRIDAXIS").size(), static_cast<std::size_t>(1));
	CHECK_EQ(model.byType("IFCPOLYLINE").size(), static_cast<std::size_t>(1));
}

TEST(dangling_reference_after_drop_is_tolerated)
{
	// IFCFOOTINGTYPE(#1) を除去すると #1 は宙ぶらりんになるが、それを参照する
	// IFCRELDEFINESBYTYPE は残り、未解決参照は nullptr で寛容に扱える。
	std::string const text = "#1=IFCFOOTINGTYPE('g',$,'FT1',$);\n"
							 "#2=IFCRELDEFINESBYTYPE('r',$,$,$,(#3),#1);\n";
	Model const model = loadIfcFromText(text);
	const Entity* rel = model.entity(2);
	CHECK(rel != nullptr);
	// 最終属性 #1 は未解決。
	const Value& typeRef = rel->attribute(5);
	CHECK(typeRef.isReference());
	CHECK_EQ(typeRef.reference, 1);
	CHECK(model.resolve(typeRef) == nullptr);
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

	// 3 本の通り芯（IFCGRIDAXIS）と 3 本のポリライン、4 点。
	CHECK_EQ(model.byType("IFCGRIDAXIS").size(), static_cast<std::size_t>(3));
	CHECK_EQ(model.byType("IFCPOLYLINE").size(), static_cast<std::size_t>(3));
	CHECK_EQ(model.byType("IFCCARTESIANPOINT").size(), static_cast<std::size_t>(4));
	CHECK_EQ(model.byType("IFCBUILDINGSTOREY").size(), static_cast<std::size_t>(1));

	// サニタイズで IFCFOOTINGTYPE は消えている。
	CHECK(model.byType("IFCFOOTINGTYPE").empty());

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

TEST_MAIN();
