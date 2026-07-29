//
//	ParseStructuralClassTests.cpp
//
//	構造クラス判定（src/parse/StructuralClass）の単体テスト。VectorWorks SDK を一切
//	include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md
//	「テスト方針」: core/ parse/ は無 SDK で単体テスト）。Python 版
//	test_ifc_structural_class.py の意図を 1 対 1 で写す（ROADMAP.md M4）。
//
//	検証項目: 種別トークン抽出（"木梁:{種別}:{連番}" は中央・2 要素名は接頭辞・空/未設定）・
//	種別→クラスの直接対応（床小梁/床大梁/甲乙梁→床梁、登り梁、未知は無し）・横架材クラス
//	の状況推定（名前優先・最下階=土台・中間階=床梁・最上階の軒高=小屋梁・軒高超=母屋）・
//	柱クラス（STANDCOLUMN/名前/最上階で小屋束、一般階は貫通で通し柱/管柱）。純ロジック。
//

#include "TestFramework.h"

#include "parse/StructuralClass.h"

#include <string>

using HomeskzIfcImport::parse::CLASS_DODAI;
using HomeskzIfcImport::parse::CLASS_DOUSASHI;
using HomeskzIfcImport::parse::CLASS_KOYABARI;
using HomeskzIfcImport::parse::CLASS_KOYAZUKA;
using HomeskzIfcImport::parse::CLASS_KUDABASHIRA;
using HomeskzIfcImport::parse::CLASS_MOYA;
using HomeskzIfcImport::parse::CLASS_MUNAGI;
using HomeskzIfcImport::parse::CLASS_NEDA;
using HomeskzIfcImport::parse::CLASS_NOBORIBARI;
using HomeskzIfcImport::parse::CLASS_NOKIGETA;
using HomeskzIfcImport::parse::CLASS_OOBIKI;
using HomeskzIfcImport::parse::CLASS_TOSHIBASHIRA;
using HomeskzIfcImport::parse::CLASS_YUKABARI;
using HomeskzIfcImport::parse::memberClassFromName;
using HomeskzIfcImport::parse::memberTypeOfName;
using HomeskzIfcImport::parse::resolveColumnClass;
using HomeskzIfcImport::parse::resolveMemberClass;

// --- memberTypeOfName（Python 版 TestMemberTypeOfName） --------------------------

// "木梁:{種別}:{連番}" は中央の種別トークンを使う。
TEST(member_type_wood_beam_uses_middle_token)
{
	CHECK_EQ(memberTypeOfName("木梁:土台:1"), std::string("土台"));
	CHECK_EQ(memberTypeOfName("木梁:軒桁:1_1_0"), std::string("軒桁"));
	CHECK_EQ(memberTypeOfName("木梁:床大梁:1_5"), std::string("床大梁"));
}

// 2 要素名（火打・筋かい）は接頭辞を使う。
TEST(member_type_two_part_name_uses_prefix)
{
	CHECK_EQ(memberTypeOfName("火打:0_1"), std::string("火打"));
	CHECK_EQ(memberTypeOfName("筋かい:1FL_1"), std::string("筋かい"));
}

// 空文字（未設定）は空文字を返す（Python の None/'' 相当。C++ は空文字で表す）。
TEST(member_type_handles_empty)
{
	CHECK_EQ(memberTypeOfName(""), std::string(""));
}

// --- memberClassFromName（Python 版 TestMemberClassFromName） ----------------------

// 既知種別は直接クラスへ対応する（_MEMBER_CLASS_BY_TYPE の全対応を網羅する。床小梁・
// 床大梁・甲乙梁はまとめて床梁、登り梁は小屋組の登り梁クラス）。
TEST(member_class_known_types_map_directly)
{
	CHECK(memberClassFromName("木梁:土台:1").has_value());
	CHECK_EQ(memberClassFromName("木梁:土台:1").value(), std::string(CLASS_DODAI));
	CHECK_EQ(memberClassFromName("木梁:大引:1").value(), std::string(CLASS_OOBIKI));
	CHECK_EQ(memberClassFromName("木梁:根太:1").value(), std::string(CLASS_NEDA));
	CHECK_EQ(memberClassFromName("木梁:軒桁:1").value(), std::string(CLASS_NOKIGETA));
	CHECK_EQ(memberClassFromName("木梁:胴差:1").value(), std::string(CLASS_DOUSASHI));
	CHECK_EQ(memberClassFromName("木梁:床小梁:1").value(), std::string(CLASS_YUKABARI));
	CHECK_EQ(memberClassFromName("木梁:床大梁:1").value(), std::string(CLASS_YUKABARI));
	CHECK_EQ(memberClassFromName("木梁:甲乙梁:1").value(), std::string(CLASS_YUKABARI));
	CHECK_EQ(memberClassFromName("木梁:小屋梁:1").value(), std::string(CLASS_KOYABARI));
	CHECK_EQ(memberClassFromName("木梁:母屋:1").value(), std::string(CLASS_MOYA));
	CHECK_EQ(memberClassFromName("木梁:棟木:1").value(), std::string(CLASS_MUNAGI));
	CHECK_EQ(memberClassFromName("木梁:登り梁:1").value(), std::string(CLASS_NOBORIBARI));
}

// 未知種別（隅木・谷木、火打）と空文字は対応無し（std::nullopt）。
TEST(member_class_unknown_types_return_nullopt)
{
	CHECK(!memberClassFromName("木梁:隅木・谷木:1").has_value());
	CHECK(!memberClassFromName("火打:0_1").has_value());
	CHECK(!memberClassFromName("").has_value());
}

// --- resolveMemberClass（Python 版 TestResolveMemberClass） ------------------------

// 名前で判別できれば階・高さに依らずその種別クラスにする。
TEST(resolve_member_name_is_trusted_over_position)
{
	CHECK_EQ(resolveMemberClass("木梁:小屋梁:1", 0, 2, /*aboveEaves=*/false),
			 std::string(CLASS_KOYABARI));
}

// 最下階の判別不能な横架材は土台。
TEST(resolve_member_fallback_lowest_story_is_dodai)
{
	CHECK_EQ(resolveMemberClass("", 0, 2, /*aboveEaves=*/false), std::string(CLASS_DODAI));
}

// 中間階の判別不能な横架材（火打等）は床梁。
TEST(resolve_member_fallback_middle_story_is_yukabari)
{
	CHECK_EQ(resolveMemberClass("火打:1_1", 1, 2, /*aboveEaves=*/false),
			 std::string(CLASS_YUKABARI));
}

// 最上階の軒高付近（aboveEaves=false）の判別不能な横架材は小屋梁。
TEST(resolve_member_fallback_top_story_at_eaves_is_koyabari)
{
	CHECK_EQ(resolveMemberClass("木梁:隅木・谷木:1", 2, 2, /*aboveEaves=*/false),
			 std::string(CLASS_KOYABARI));
}

// 最上階の軒高を超える（aboveEaves=true）判別不能な横架材は母屋。
TEST(resolve_member_fallback_top_story_above_eaves_is_moya)
{
	CHECK_EQ(resolveMemberClass("木梁:隅木・谷木:1", 2, 2, /*aboveEaves=*/true),
			 std::string(CLASS_MOYA));
}

// --- resolveColumnClass（Python 版 TestResolveColumnClass） ------------------------

// ObjectType=STANDCOLUMN は小屋束。
TEST(resolve_column_standcolumn_object_type_is_koyazuka)
{
	CHECK_EQ(resolveColumnClass("STANDCOLUMN", "小屋束:1_1", 1, 2, /*isThrough=*/false),
			 std::string(CLASS_KOYAZUKA));
}

// ObjectType 無しでも Name が "小屋束" 始まりなら小屋束。
TEST(resolve_column_koyazuka_name_without_object_type)
{
	CHECK_EQ(resolveColumnClass("", "小屋束:2_1", 1, 2, /*isThrough=*/false),
			 std::string(CLASS_KOYAZUKA));
}

// 記録が無くても最上階（index >= topIndex）の柱は小屋束にフォールバックする。
TEST(resolve_column_top_story_column_falls_back_to_koyazuka)
{
	CHECK_EQ(resolveColumnClass("", "柱:3_1", 2, 2, /*isThrough=*/false),
			 std::string(CLASS_KOYAZUKA));
}

// 一般階で 1 階分しか伸びない柱は管柱。
TEST(resolve_column_general_story_short_column_is_kudabashira)
{
	CHECK_EQ(resolveColumnClass("", "柱:1_1", 0, 2, /*isThrough=*/false),
			 std::string(CLASS_KUDABASHIRA));
}

// 一般階で上階を貫く柱は通し柱。
TEST(resolve_column_general_story_through_column_is_toshibashira)
{
	CHECK_EQ(resolveColumnClass("", "柱:1_1", 0, 2, /*isThrough=*/true),
			 std::string(CLASS_TOSHIBASHIRA));
}

TEST_MAIN();
