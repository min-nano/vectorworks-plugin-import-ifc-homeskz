//
//	CoreFloorScriptTests.cpp
//
//	床の VectorScript 組み立て（src/core/FloorScript）の単体テスト。VectorWorks SDK を
//	一切 include せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md
//	「テスト方針」: SDK から切り離せる部分は core へ寄せて無 SDK テストする）。
//
//	床ツール（Floor オブジェクト）を生成する API が ISDK に無いため、draw/Floor は
//	SDK のスクリプトエンジンで VectorScript を実行して床を描く（理由は
//	src/core/FloorScript.h 参照）。実行そのものは実機でしか確かめられないが、
//	**スクリプト本文**（命令の並び・座標の書式・エスケープ）はここで検証できる。
//	Python 版 vw/floor.py の draw_floor と 1 対 1 で対応することを確認する。
//

#include "TestFramework.h"

#include "core/Document.h"
#include "core/FloorScript.h"

#include <string>

using namespace HomeskzIfcImport;
using core::buildFloorScript;
using core::FloorCommand;
using core::formatScriptNumber;
using core::quoteScriptString;

namespace
{
	// 検証用の床命令（4 点の外形・厚み 24・床下端 -120・段差 -832）。
	FloorCommand sampleFloor()
	{
		FloorCommand floor;
		floor.layer = "2-FL";
		floor.drawClass = "04構造-02木造-06耐力面材-02床";
		floor.boundary = {core::Vec2{0.0, 0.0}, core::Vec2{1000.0, 0.0}, core::Vec2{1000.0, 2000.0},
						  core::Vec2{0.0, 2000.0}};
		floor.thickness = 24.0;
		floor.elevation = -120.0;
		floor.bound = core::StoryBoundCommand{0, "横架材天端", -832.0};
		return floor;
	}

	// haystack に needle が含まれるか。
	bool contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	// a が b より前に現れるか（命令の順序検証。どちらも存在することを前提にする）。
	bool before(const std::string& text, const std::string& a, const std::string& b)
	{
		const std::size_t posA = text.find(a);
		const std::size_t posB = text.find(b);
		return posA != std::string::npos && posB != std::string::npos && posA < posB;
	}
} // namespace

// ---------------------------------------------------------------------------
// 数値・文字列の整形
// ---------------------------------------------------------------------------

TEST(format_script_number_is_fixed_point_and_locale_independent)
{
	// 常に '.' 小数点・指数表記なし（ロケールに依存しない）。
	CHECK_EQ(formatScriptNumber(0.0), std::string("0.000000"));
	CHECK_EQ(formatScriptNumber(-832.0), std::string("-832.000000"));
	CHECK_EQ(formatScriptNumber(1234567.5), std::string("1234567.500000"));
	// 極小値・巨大値でも指数表記（1e-07 等）にならない。VectorScript のリテラルとして
	// そのまま埋め込めることが要件。
	CHECK_EQ(formatScriptNumber(0.0000001), std::string("0.000000"));
	CHECK(!contains(formatScriptNumber(0.0000001), "e"));
	CHECK(!contains(formatScriptNumber(1e12), "e"));
}

TEST(quote_script_string_escapes_single_quotes)
{
	CHECK_EQ(quoteScriptString("横架材天端"), std::string("'横架材天端'"));
	// VectorScript の文字列リテラルでは ' を '' へ重ねてエスケープする。
	CHECK_EQ(quoteScriptString("a'b"), std::string("'a''b'"));
}

// ---------------------------------------------------------------------------
// スクリプト本文（Python 版 draw_floor との 1 対 1 対応）
// ---------------------------------------------------------------------------

TEST(floor_script_is_a_complete_runnable_procedure)
{
	const std::string script = buildFloorScript(sampleFloor());
	// 完結した手続き（PROCEDURE … END; RUN(…);）であること。
	CHECK(contains(script, "PROCEDURE HomeskzImportDrawFloor;"));
	CHECK(contains(script, "RUN(HomeskzImportDrawFloor);"));
	CHECK(contains(script, "h : HANDLE;"));
}

TEST(floor_script_draws_floor_tool_with_thickness_and_boundary)
{
	const std::string script = buildFloorScript(sampleFloor());

	// 床ツール: BeginFloor(厚み) → 閉じたポリゴン → EndGroup（Python 版と同じ手順）。
	CHECK(contains(script, "BeginFloor(24.000000);"));
	CHECK(contains(script, "ClosePoly;"));
	CHECK(contains(script, "BeginPoly;"));
	CHECK(contains(script, "MoveTo(0.000000, 0.000000);"));
	CHECK(contains(script, "LineTo(1000.000000, 0.000000);"));
	CHECK(contains(script, "LineTo(1000.000000, 2000.000000);"));
	CHECK(contains(script, "LineTo(0.000000, 2000.000000);"));
	CHECK(contains(script, "EndPoly;"));
	CHECK(contains(script, "EndGroup;"));

	// 順序: BeginFloor → 外形 → EndGroup → LNewObj。
	CHECK(before(script, "BeginFloor(", "BeginPoly;"));
	CHECK(before(script, "BeginPoly;", "EndGroup;"));
	CHECK(before(script, "EndGroup;", "h := LNewObj;"));
}

TEST(floor_script_moves_to_absolute_elevation_and_binds_story_level)
{
	const std::string script = buildFloorScript(sampleFloor());

	// 床下端を IFC の床位置（絶対 Z）へ移動する。
	CHECK(contains(script, "Move3D(0, 0, -120.000000);"));
	// クラス分けと by-class 属性。
	CHECK(contains(script, "SetClass(h, '04構造-02木造-06耐力面材-02床');"));
	CHECK(contains(script, "SetPenColorByClass(h);"));
	CHECK(contains(script, "SetFillColorByClass(h);"));
	CHECK(contains(script, "SetLWByClass(h);"));
	CHECK(contains(script, "SetLSByClass(h);"));
	CHECK(contains(script, "SetFPatByClass(h);"));
	CHECK(contains(script, "SetMarkerByClass(h);"));
	CHECK(contains(script, "SetOpacityByClass(h);"));
	// 高さ基準（下端=0・ストーリレベル基準=2・自階=0・横架材天端・段差 offset）。
	CHECK(contains(script, "SetObjectStoryBound(h, 0, 2, 0, '横架材天端', -832.000000);"));
	CHECK(contains(script, "ResetObject(h);"));

	// 順序: Move3D → SetClass → バインド → ResetObject。
	CHECK(before(script, "Move3D(", "SetClass(h,"));
	CHECK(before(script, "SetClass(h,", "SetObjectStoryBound(h,"));
	CHECK(before(script, "SetObjectStoryBound(h,", "ResetObject(h);"));
}

TEST(floor_script_falls_back_to_outline_polygon)
{
	// 床が作れない（LNewObj が NIL）ときは外形ポリゴンにフォールバックする。
	// そのため外形の描画命令は 2 度現れ、ELSE 節でもクラス分けする。
	const std::string script = buildFloorScript(sampleFloor());
	CHECK(contains(script, "IF h <> NIL THEN"));
	CHECK(contains(script, "ELSE"));

	std::size_t beginPolyCount = 0;
	for (std::size_t pos = script.find("BeginPoly;"); pos != std::string::npos;
		 pos = script.find("BeginPoly;", pos + 1))
		++beginPolyCount;
	CHECK_EQ(beginPolyCount, static_cast<std::size_t>(2));

	// フォールバック側は Move3D も高さバインドもしない（床でないため）。
	std::size_t move3dCount = 0;
	for (std::size_t pos = script.find("Move3D("); pos != std::string::npos;
		 pos = script.find("Move3D(", pos + 1))
		++move3dCount;
	CHECK_EQ(move3dCount, static_cast<std::size_t>(1));
}

TEST(floor_script_quotes_class_and_level_names)
{
	// クラス名・レベル名に ' が含まれていてもスクリプトが壊れない。
	FloorCommand floor = sampleFloor();
	floor.drawClass = "a'b";
	floor.bound.level = "c'd";
	const std::string script = buildFloorScript(floor);
	CHECK(contains(script, "SetClass(h, 'a''b');"));
	CHECK(contains(script, "'c''d'"));
}

TEST_MAIN();
