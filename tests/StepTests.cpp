//
//	StepTests.cpp
//
//	最小 STEP リーダ（src/parse/Step）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md
//	「テスト方針」: core/ parse/ は無 SDK で単体テスト）。
//
//	ホームズ君 IFC が使う STEP 記法——参照 #N・文字列・数値・列挙・入れ子リスト・
//	型付き値——を小さな合成テキストで網羅し、byType / attribute / referrers /
//	resolve のグラフ読み取り API が期待どおり動くことを確かめる。
//

#include "TestFramework.h"

#include "parse/Step.h"

using namespace HomeskzIfcImport::parse;

// ---------------------------------------------------------------------------
// 基本: インスタンスの型別インデックスと属性アクセス
// ---------------------------------------------------------------------------

TEST(parses_instances_and_indexes_by_type)
{
	Model const model = parseStep("DATA;\n"
								  "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
								  "#2=IFCCARTESIANPOINT((1.,2.,3.));\n"
								  "#3=IFCPOLYLINE((#1,#2));\n"
								  "ENDSEC;\n");

	CHECK_EQ(model.size(), static_cast<std::size_t>(3));
	CHECK_EQ(model.byType("IFCCARTESIANPOINT").size(), static_cast<std::size_t>(2));
	CHECK_EQ(model.byType("IFCPOLYLINE").size(), static_cast<std::size_t>(1));
	// 未知の型は空。
	CHECK(model.byType("IFCWALL").empty());

	// byType は #id 昇順（決定的）。
	CHECK_EQ(model.byType("IFCCARTESIANPOINT")[0], 1);
	CHECK_EQ(model.byType("IFCCARTESIANPOINT")[1], 2);
}

TEST(entity_lookup_and_missing_id)
{
	Model const model = parseStep("#7=IFCCARTESIANPOINT((0.,0.,0.));");
	const Entity* p = model.entity(7);
	CHECK(p != nullptr);
	CHECK_EQ(p->type, std::string("IFCCARTESIANPOINT"));
	CHECK_EQ(p->id, 7);
	// 未知の #id は nullptr。
	CHECK(model.entity(999) == nullptr);
}

// ---------------------------------------------------------------------------
// 値の種別: 参照・数値（整数/実数）・文字列・列挙・Null・入れ子リスト
// ---------------------------------------------------------------------------

TEST(reads_reference_number_string_enum_null)
{
	Model const model = parseStep("#5=IFCGRIDAXIS('X1',#6,.T.);\n"
								  "#6=IFCPOLYLINE((#7));\n"
								  "#8=IFCQUANTITYLENGTH('L',$,$,4550.);\n"
								  "#9=IFCPROPERTYSINGLEVALUE('N',$,42,$);\n");

	const Entity* axis = model.entity(5);
	CHECK(axis != nullptr);
	// 属性0: 文字列 'X1'
	CHECK(axis->attribute(0).type == ValueType::String);
	CHECK_EQ(axis->attribute(0).text, std::string("X1"));
	// 属性1: 参照 #6
	CHECK(axis->attribute(1).isReference());
	CHECK_EQ(axis->attribute(1).reference, 6);
	// 属性2: 列挙 .T.
	CHECK(axis->attribute(2).type == ValueType::Enum);
	CHECK_EQ(axis->attribute(2).text, std::string("T"));

	// 実数と整数を種別ごとに正しく読む。
	const Entity* q = model.entity(8);
	CHECK(q != nullptr);
	CHECK(q->attribute(3).type == ValueType::Real);
	CHECK(q->attribute(3).asReal() > 4549.9 && q->attribute(3).asReal() < 4550.1);

	const Entity* pv = model.entity(9);
	CHECK(pv != nullptr);
	CHECK(pv->attribute(2).type == ValueType::Integer);
	CHECK_EQ(pv->attribute(2).integer, static_cast<long long>(42));
	// Null（$）。
	CHECK(pv->attribute(1).isNull());
	// 範囲外アクセスは Null（寛容）。
	CHECK(pv->attribute(99).isNull());
}

TEST(reads_nested_lists)
{
	Model const model = parseStep("#1=IFCPOLYLINE((#2,#3,#4));");
	const Entity* poly = model.entity(1);
	CHECK(poly != nullptr);
	const Value& pts = poly->attribute(0);
	CHECK(pts.isList());
	CHECK_EQ(pts.items.size(), static_cast<std::size_t>(3));
	CHECK(pts.items[0].isReference());
	CHECK_EQ(pts.items[0].reference, 2);
	CHECK_EQ(pts.items[2].reference, 4);
}

TEST(reads_coordinate_list_of_reals)
{
	Model const model = parseStep("#1=IFCCARTESIANPOINT((3640.,-4550.,0.));");
	const Value& coords = model.entity(1)->attribute(0);
	CHECK(coords.isList());
	CHECK_EQ(coords.items.size(), static_cast<std::size_t>(3));
	CHECK(coords.items[0].asReal() > 3639.9 && coords.items[0].asReal() < 3640.1);
	// 負値も読める。
	CHECK(coords.items[1].asReal() < -4549.9 && coords.items[1].asReal() > -4550.1);
}

TEST(reads_typed_value)
{
	// IFCLABEL('x') のような型付き単純値。
	Model const model = parseStep("#1=IFCPROPERTYSINGLEVALUE('P',$,IFCLABEL('yes'),$);");
	const Value& v = model.entity(1)->attribute(2);
	CHECK(v.type == ValueType::Typed);
	CHECK_EQ(v.text, std::string("IFCLABEL"));
	CHECK_EQ(v.items.size(), static_cast<std::size_t>(1));
	CHECK(v.items[0].type == ValueType::String);
	CHECK_EQ(v.items[0].text, std::string("yes"));
}

// ---------------------------------------------------------------------------
// 文字列のエスケープ・コメント・空白の頑健性
// ---------------------------------------------------------------------------

TEST(handles_escaped_quotes_and_semicolon_in_string)
{
	// '' は ' のエスケープ。文字列内の ';' は文末ではない。
	Model const model = parseStep("#1=IFCLABEL('a''b;c');");
	CHECK_EQ(model.size(), static_cast<std::size_t>(1));
	CHECK_EQ(model.entity(1)->attribute(0).text, std::string("a'b;c"));
}

TEST(skips_comments_and_extra_whitespace)
{
	Model const model = parseStep("/* header comment */\n"
								  "#1 = IFCCARTESIANPOINT ( ( 0. , 0. , 0. ) ) ;\n"
								  "/* trailing */\n");
	CHECK_EQ(model.size(), static_cast<std::size_t>(1));
	CHECK_EQ(model.entity(1)->attribute(0).items.size(), static_cast<std::size_t>(3));
}

TEST(tolerates_malformed_statement_and_continues)
{
	// 壊れた文（閉じ括弧欠落）は読み飛ばし、後続の正しい文は読める。
	Model const model = parseStep("#1=IFCBROKEN((#2,#3 ;\n"
								  "#4=IFCCARTESIANPOINT((0.,0.,0.));\n");
	CHECK(model.entity(4) != nullptr);
	CHECK_EQ(model.entity(4)->type, std::string("IFCCARTESIANPOINT"));
}

// ---------------------------------------------------------------------------
// 逆参照（referrers）と参照解決（resolve）
// ---------------------------------------------------------------------------

TEST(referrers_are_sorted_and_deduplicated)
{
	// #10 を #1・#2 が参照。#3 は #10 を 2 回参照するが被参照は 1 回。
	Model const model = parseStep("#1=IFCPOLYLINE((#10));\n"
								  "#2=IFCPOLYLINE((#10));\n"
								  "#3=IFCLINE(#10,#10);\n"
								  "#10=IFCCARTESIANPOINT((0.,0.,0.));\n");
	const std::vector<int>& refs = model.referrers(10);
	CHECK_EQ(refs.size(), static_cast<std::size_t>(3));
	// #id 昇順（決定的）。
	CHECK_EQ(refs[0], 1);
	CHECK_EQ(refs[1], 2);
	CHECK_EQ(refs[2], 3);
	// 誰も参照しない #id は空。
	CHECK(model.referrers(1).empty());
}

TEST(referrers_deterministic_regardless_of_declaration_order)
{
	// 被参照 #5 より後に参照元 #2 を宣言しても、referrers は #id 昇順で返る。
	Model const model = parseStep("#9=IFCPOLYLINE((#5));\n"
								  "#5=IFCCARTESIANPOINT((0.,0.,0.));\n"
								  "#2=IFCPOLYLINE((#5));\n");
	const std::vector<int>& refs = model.referrers(5);
	CHECK_EQ(refs.size(), static_cast<std::size_t>(2));
	CHECK_EQ(refs[0], 2);
	CHECK_EQ(refs[1], 9);
}

TEST(resolve_follows_reference_to_entity)
{
	Model const model = parseStep("#1=IFCGRIDAXIS('X1',#2,.T.);\n"
								  "#2=IFCPOLYLINE((#3,#4));\n");
	const Entity* axis = model.entity(1);
	const Entity* curve = model.resolve(axis->attribute(1));
	CHECK(curve != nullptr);
	CHECK_EQ(curve->type, std::string("IFCPOLYLINE"));
	// 参照でない値の resolve は nullptr。
	CHECK(model.resolve(axis->attribute(0)) == nullptr);
	// 未解決参照（宣言のない #999）の resolve も nullptr（寛容）。
	CHECK(model.entity(2)->attribute(0).items.size() == static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------------------
// 決定性・重複宣言
// ---------------------------------------------------------------------------

TEST(duplicate_id_keeps_first_declaration)
{
	Model const model = parseStep("#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
								  "#1=IFCPOLYLINE((#2));\n");
	CHECK_EQ(model.size(), static_cast<std::size_t>(1));
	CHECK_EQ(model.entity(1)->type, std::string("IFCCARTESIANPOINT"));
}

TEST_MAIN();
