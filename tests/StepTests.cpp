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

// ---------------------------------------------------------------------------
// 値の網羅: asReal / 派生値 / 空リスト / 指数 / 裸キーワード / 不明文字
// ---------------------------------------------------------------------------

TEST(as_real_normalizes_integer_and_non_number)
{
	Model const model = parseStep("#1=IFCX(42,'text');");
	const Entity* e = model.entity(1);
	CHECK(e != nullptr);
	// 整数は double へ正規化。
	CHECK(e->attribute(0).type == ValueType::Integer);
	CHECK(e->attribute(0).asReal() > 41.9 && e->attribute(0).asReal() < 42.1);
	// 非数値（文字列）・範囲外（Null）の asReal は 0。
	CHECK_EQ(e->attribute(1).asReal(), 0.0);
	CHECK_EQ(e->attribute(99).asReal(), 0.0);
}

TEST(reads_derived_value)
{
	Model const model = parseStep("#1=IFCX(*);");
	CHECK(model.entity(1)->attribute(0).type == ValueType::Derived);
}

TEST(reads_empty_argument_list)
{
	Model const model = parseStep("#1=IFCX();");
	const Entity* e = model.entity(1);
	CHECK(e != nullptr);
	CHECK_EQ(e->type, std::string("IFCX"));
	CHECK(e->attributes.empty());
}

TEST(reads_number_with_exponent)
{
	Model const model = parseStep("#1=IFCX(1.5E-3,2E3,-4.0e2);");
	const Entity* e = model.entity(1);
	CHECK(e->attribute(0).type == ValueType::Real);
	CHECK(e->attribute(0).asReal() > 0.0014 && e->attribute(0).asReal() < 0.0016);
	CHECK(e->attribute(1).asReal() > 1999.0 && e->attribute(1).asReal() < 2001.0);
	CHECK(e->attribute(2).asReal() < -399.0 && e->attribute(2).asReal() > -401.0);
}

TEST(reads_bare_keyword_as_enum)
{
	// '(' を伴わない裸の識別子は列挙記号として保持する（想定外入力への寛容）。
	Model const model = parseStep("#1=IFCX(UNKNOWNKW);");
	const Value& v = model.entity(1)->attribute(0);
	CHECK(v.type == ValueType::Enum);
	CHECK_EQ(v.text, std::string("UNKNOWNKW"));
}

TEST(skips_unparseable_value_character)
{
	// 値位置の解釈できない文字は 1 つ捨てて Null にする。
	Model const model = parseStep("#1=IFCX(@);");
	const Entity* e = model.entity(1);
	CHECK(e != nullptr);
	CHECK_EQ(e->attributes.size(), static_cast<std::size_t>(1));
	CHECK(e->attribute(0).isNull());
}

// ---------------------------------------------------------------------------
// 複合エンティティ #id=(A(...)B(...))
// ---------------------------------------------------------------------------

TEST(reads_complex_entity_record)
{
	Model const model = parseStep("#1=(IFCX(1)IFCY('a',2));");
	const Entity* e = model.entity(1);
	CHECK(e != nullptr);
	// 型名は連結、属性は各レコードの引数を連結（1, 'a', 2）。
	CHECK_EQ(e->type, std::string("IFCX.IFCY"));
	CHECK_EQ(e->attributes.size(), static_cast<std::size_t>(3));
	CHECK_EQ(e->attribute(0).integer, static_cast<long long>(1));
	CHECK_EQ(e->attribute(1).text, std::string("a"));
	CHECK_EQ(e->attribute(2).integer, static_cast<long long>(2));
}

// ---------------------------------------------------------------------------
// 桁溢れ・数字なし参照は 0 に落とす（寛容な握りつぶし）
// ---------------------------------------------------------------------------

TEST(overflowing_integer_becomes_zero)
{
	Model const model = parseStep("#1=IFCX(999999999999999999999999);");
	const Value& v = model.entity(1)->attribute(0);
	CHECK(v.type == ValueType::Integer);
	CHECK_EQ(v.integer, static_cast<long long>(0));
}

TEST(overflowing_reference_becomes_zero)
{
	Model const model = parseStep("#1=IFCX(#99999999999999);");
	const Value& v = model.entity(1)->attribute(0);
	CHECK(v.isReference());
	CHECK_EQ(v.reference, 0);
}

TEST(reference_without_digits_is_zero)
{
	Model const model = parseStep("#1=IFCX(#);");
	const Value& v = model.entity(1)->attribute(0);
	CHECK(v.isReference());
	CHECK_EQ(v.reference, 0);
}

// ---------------------------------------------------------------------------
// 壊れた・途切れた入力への寛容さ（破棄して続行、クラッシュしない）
// ---------------------------------------------------------------------------

TEST(tolerates_malformed_terminated_instances)
{
	// 各種の壊れた宣言（';' で終端）はすべて読み飛ばし、正常な #100 だけが残る。
	Model const model = parseStep("#0=IFCX($);"		// id<=0 → 破棄
								  "#=IFCX($);"		// 数字なし id → 破棄
								  "#3 IFCX($);"		// '=' 欠落 → 破棄
								  "#5='x';"			// 型名が空 → 破棄
								  "#6=IFCY;"		// 型名の後に '(' 無し → 破棄
								  "#100=IFCZ($);"); // 正常
	CHECK(model.entity(0) == nullptr);
	CHECK(model.entity(3) == nullptr);
	CHECK(model.entity(5) == nullptr);
	CHECK(model.entity(6) == nullptr);
	CHECK(model.entity(100) != nullptr);
	CHECK_EQ(model.entity(100)->type, std::string("IFCZ"));
}

TEST(tolerates_input_truncated_mid_instance)
{
	// '=' 直後 EOF / 引数リスト未閉 / ',' 後 EOF / 複合レコード未閉。いずれも
	// 例外を漏らさず空の Model になる（1 要素の欠損で全体を止めない）。
	CHECK_EQ(parseStep("#4=").size(), static_cast<std::size_t>(0));
	CHECK_EQ(parseStep("#7=IFCX(#8").size(), static_cast<std::size_t>(0));
	CHECK_EQ(parseStep("#9=IFCX(#8,").size(), static_cast<std::size_t>(0));
	CHECK_EQ(parseStep("#1=(IFCX(1)").size(), static_cast<std::size_t>(0));
	// 複合レコードの 2 つ目のサブレコードが壊れている場合も破棄する。
	CHECK_EQ(parseStep("#1=(IFCX(1)@);").size(), static_cast<std::size_t>(0));
}

TEST(unterminated_string_is_tolerated)
{
	// 閉じられない文字列を含む文は捨てられる。クラッシュ・無限ループしないこと。
	Model const model = parseStep("#1=IFCX('abc");
	CHECK_EQ(model.size(), static_cast<std::size_t>(0));
}

TEST(skips_header_statement_with_escaped_quote)
{
	// '#' 以外で始まる文（ヘッダ等）は読み飛ばす。文字列内の '' と ';' で誤らず、
	// 後続の #1 を正しく読める。
	Model const model = parseStep("FILE_NAME('a''b;c');\n#1=IFCX($);\n");
	CHECK_EQ(model.size(), static_cast<std::size_t>(1));
	CHECK(model.entity(1) != nullptr);
}

// ---------------------------------------------------------------------------
// ISO 10303-21 拡張文字エスケープのデコード（\X2\…\X0\ / \X\HH / \S\c / \P?\）
// ---------------------------------------------------------------------------

TEST(decodes_utf16_escape_in_string)
{
	// ホームズ君 IFC の日本語 Name は \X2\<UTF-16>\X0\ で出力される（"床版"）。
	Model const model = parseStep("#1=IFCSLAB('id',$,'\\X2\\5E8A7248\\X0\\',$);\n");
	const Entity* slab = model.entity(1);
	CHECK(slab != nullptr);
	if (slab != nullptr)
		CHECK_EQ(slab->attribute(2).text, std::string("床版"));
}

TEST(decodes_utf16_escape_mixed_with_ascii)
{
	// エスケープ区間の前後に ASCII が混じる形（"木梁:1"。ホームズ君の横架材名の形）。
	CHECK_EQ(decodeStepString("\\X2\\67286881\\X0\\:1"), std::string("木梁:1"));
	// 連続する 2 区間。
	CHECK_EQ(decodeStepString("\\X2\\5E8A\\X0\\\\X2\\7248\\X0\\"), std::string("床版"));
}

TEST(decodes_ascii_and_lowercase_hex_in_utf16_escape)
{
	// エスケープ区間に ASCII 範囲のコード単位が入ることもある（1 バイトで出力する）。
	CHECK_EQ(decodeStepString("\\X2\\00410042\\X0\\"), std::string("AB"));
	// 16 進は小文字でも読む（出力側の実装差を吸収する）。
	CHECK_EQ(decodeStepString("\\X2\\5e8a7248\\X0\\"), std::string("床版"));
}

TEST(decodes_surrogate_pair)
{
	// U+20BB7（サロゲートペア D842 DFB7）。UTF-8 は 4 バイト。
	const std::string decoded = decodeStepString("\\X2\\D842DFB7\\X0\\");
	CHECK_EQ(decoded.size(), static_cast<std::size_t>(4));
	CHECK_EQ(decoded, std::string("\xF0\xA0\xAE\xB7"));
}

TEST(decodes_single_byte_and_shifted_escapes)
{
	// \X\HH は 1 バイト（ここでは U+00C4）、\S\c は c のコードポイント + 128。
	CHECK_EQ(decodeStepString("\\X\\C4"), std::string("\xC3\x84"));
	CHECK_EQ(decodeStepString("\\S\\D"), std::string("\xC3\x84"));
	// \P?\（コードページ指示）は読み飛ばす。
	CHECK_EQ(decodeStepString("\\PA\\ab"), std::string("ab"));
}

TEST(leaves_plain_and_broken_escapes_alone)
{
	// エスケープを含まない文字列はそのまま。
	CHECK_EQ(decodeStepString("X1"), std::string("X1"));
	// 壊れた（16 進でない・閉じられない）エスケープでも文字列全体を失わない。16 進として
	// 読めなくなった時点で打ち切り、残りはそのまま文字として通す（内容を捨てない）。
	CHECK_EQ(decodeStepString("a\\X2\\ZZZZ\\X0\\b"), std::string("aZZZZ\\X0\\b"));
	CHECK_EQ(decodeStepString("a\\X2\\"), std::string("a"));
	CHECK_EQ(decodeStepString("a\\qb"), std::string("a\\qb"));
}

TEST_MAIN();
