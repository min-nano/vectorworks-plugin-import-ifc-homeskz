//
//	CoreImportOptionsTests.cpp
//
//	取り込み設定（src/core/ImportOptions）の単体テスト。VectorWorks SDK を一切 include
//	せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//
//	検証項目（docs/DEV-NOTES.md M20）: 役割の表が全役割ぶん・添字と enum がずれていない・
//	既定は従来の固定名・差し替えと空文字の扱い。**既定名は「この設定を入れる前に解析側が
//	書いていた名前」そのもの**なので、ここだけは名前を手書きで持つ（表が書き換わったら
//	気付けるようにするための固定値）。
//

#include "TestFramework.h"

#include "core/ImportOptions.h"

#include <cstddef>
#include <string>

using HomeskzIfcImport::core::defaultSymbolName;
using HomeskzIfcImport::core::ImportOptions;
using HomeskzIfcImport::core::kSymbolRoleCount;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::symbolRoleLabel;
using HomeskzIfcImport::core::symbolRoles;

TEST(import_options_role_table_matches_enum)
{
	// 表の並びは enum の値順（＝設定ダイアログの行の順）で、添字と一致している。
	CHECK_EQ(symbolRoles().size(), kSymbolRoleCount);
	for (std::size_t i = 0; i < kSymbolRoleCount; ++i)
		CHECK_EQ(static_cast<std::size_t>(symbolRoles()[i].role), i);
}

TEST(import_options_defaults_are_the_previous_fixed_names)
{
	// 設定を触らなければ従来と同じ名前で置かれる（既定の取り込み結果を変えない）。
	const ImportOptions options;
	CHECK_EQ(options.symbol(SymbolRole::AnchorBoltM12), std::string("アンカーボルト_M12"));
	CHECK_EQ(options.symbol(SymbolRole::AnchorBoltM16), std::string("アンカーボルト_M16"));
	CHECK_EQ(options.symbol(SymbolRole::FloorPost), std::string("床束"));
	CHECK_EQ(options.symbol(SymbolRole::FireBrace), std::string("鋼製火打"));
	CHECK_EQ(options.symbol(SymbolRole::Joint), std::string("仕口"));
	CHECK_EQ(options.symbol(SymbolRole::PlanMarkColumn), std::string("柱伏図記号"));
	CHECK_EQ(options.symbol(SymbolRole::PlanMarkKoyazuka), std::string("束伏図記号"));
}

TEST(import_options_labels_and_defaults_are_not_empty)
{
	// 表示名が空だと設定ダイアログの行が無名になる（＝選べない）。
	for (const auto& info : symbolRoles())
	{
		CHECK(std::string(info.label) != std::string());
		CHECK(std::string(info.defaultSymbol) != std::string());
		CHECK_EQ(std::string(symbolRoleLabel(info.role)), std::string(info.label));
		CHECK_EQ(std::string(defaultSymbolName(info.role)), std::string(info.defaultSymbol));
	}
}

TEST(import_options_set_symbol_replaces_only_that_role)
{
	ImportOptions options;
	options.setSymbol(SymbolRole::FloorPost, "床束_大");
	CHECK_EQ(options.symbol(SymbolRole::FloorPost), std::string("床束_大"));
	// 他の役割は既定のまま。
	CHECK_EQ(options.symbol(SymbolRole::FireBrace), std::string("鋼製火打"));
}

TEST(import_options_empty_name_falls_back_to_default)
{
	// 空の名前は「名前の無いシンボルを置け」という命令になり、描画側で必ず失敗する。
	ImportOptions options;
	options.setSymbol(SymbolRole::Joint, "仕口_特");
	options.setSymbol(SymbolRole::Joint, "");
	CHECK_EQ(options.symbol(SymbolRole::Joint), std::string("仕口"));
}

TEST_MAIN();
