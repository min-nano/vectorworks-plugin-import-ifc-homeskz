//
//	CoreDocumentTests.cpp
//
//	SDK 非依存の骨組み（core/ + parse/）が、無 SDK のテストハーネスから実際に
//	リンク・実行できることを確かめるスモークテスト。フォルダ骨組みと CMake の
//	ターゲット分割（SDK 非依存ライブラリ HomeskzIfcCore）が正しく通ることを担保する。
//
//	この翻訳単位は VectorWorks SDK を一切 include せず、core/Document.h・
//	core/Geometry.h・parse/BuildDocument.h だけに依存する。要素の移植が進むにつれ、
//	各 parse モジュールの本格的なテスト（ParseGridTests 等）を隣に足していく。
//

#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/BuildDocument.h"

using namespace HomeskzIfcImport;

// ---------------------------------------------------------------------------
// core::Document / validateDocument
// ---------------------------------------------------------------------------

TEST(empty_document_has_current_version)
{
	core::Document const document;
	CHECK_EQ(document.version, core::kDocumentVersion);
}

TEST(validate_accepts_empty_document)
{
	core::Document const document;
	CHECK(core::validateDocument(document));
}

TEST(validate_rejects_unknown_version)
{
	core::Document document;
	document.version = core::kDocumentVersion + 1;
	CHECK(!core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// parse::buildDocument（骨組み: いまは空の Document を返すだけ）
// ---------------------------------------------------------------------------

TEST(build_document_skeleton_returns_valid_empty_document)
{
	core::Document const document = parse::buildDocument("dummy.ifc");
	CHECK_EQ(document.version, core::kDocumentVersion);
	CHECK(core::validateDocument(document));
}

// ---------------------------------------------------------------------------
// core::Geometry（骨組みの最小型が使えることの確認）
// ---------------------------------------------------------------------------

TEST(geometry_vectors_default_to_origin)
{
	core::Vec2 const p2;
	core::Vec3 const p3;
	CHECK_EQ(p2.x, 0.0);
	CHECK_EQ(p2.y, 0.0);
	CHECK_EQ(p3.x, 0.0);
	CHECK_EQ(p3.y, 0.0);
	CHECK_EQ(p3.z, 0.0);
}

TEST_MAIN();
