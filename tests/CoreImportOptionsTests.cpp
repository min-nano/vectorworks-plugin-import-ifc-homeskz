//
//	CoreImportOptionsTests.cpp
//
//	取り込み設定（src/core/ImportOptions）の単体テスト。VectorWorks SDK を一切 include
//	せず、無 SDK のテストハーネス（TestFramework.h）で実行する（CLAUDE.md「テスト方針」）。
//
//	検証項目（docs/DEV-NOTES.md M20 / M28）: 役割の表が全役割分ある・添字と enum が
//	一致している・既定は従来の固定名・差し替えと空文字の扱い・図面枠スタイルは既定で空
//	（＝置かない）で、空文字は既定名へ戻らない。ここだけは名前を手書きで持つ（表が
//	書き換わったら気付けるようにするための固定値）。**既定名は「この設定を導入する前に
//	解析側が書いていた名前」そのもの**であるため。
//

#include "TestFramework.h"

#include "core/ImportOptions.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using HomeskzIfcImport::core::defaultDimensionStandardIndex;
using HomeskzIfcImport::core::defaultSymbolName;
using HomeskzIfcImport::core::formatRafterSize;
using HomeskzIfcImport::core::ImportOptions;
using HomeskzIfcImport::core::isValidRafterSize;
using HomeskzIfcImport::core::kDefaultRafterHeight;
using HomeskzIfcImport::core::kDefaultRafterWidth;
using HomeskzIfcImport::core::kMaxRafterSize;
using HomeskzIfcImport::core::kSymbolRoleCount;
using HomeskzIfcImport::core::parseRafterSize;
using HomeskzIfcImport::core::presetImportOptions;
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
	// 設定を変更しなければ従来と同じ名前で置かれる（既定の取り込み結果を変えない）。
	const ImportOptions options;
	CHECK_EQ(options.symbol(SymbolRole::AnchorBoltM12), std::string("アンカーボルト_M12"));
	CHECK_EQ(options.symbol(SymbolRole::AnchorBoltM16), std::string("アンカーボルト_M16"));
	CHECK_EQ(options.symbol(SymbolRole::FloorPost), std::string("床束"));
	CHECK_EQ(options.symbol(SymbolRole::FireBrace), std::string("鋼製火打"));
	CHECK_EQ(options.symbol(SymbolRole::Joint), std::string("仕口"));
	CHECK_EQ(options.symbol(SymbolRole::Splice), std::string("継手"));
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

TEST(import_options_defaults_place_every_role)
{
	// 既定はどの役割も「取り込む」（設定を入れる前と同じ振る舞い）。
	const ImportOptions options;
	for (const auto& info : symbolRoles())
		CHECK(options.isEnabled(info.role));
}

TEST(import_options_disabling_a_role_keeps_the_others)
{
	ImportOptions options;
	options.setEnabled(SymbolRole::FireBrace, false);
	CHECK(!options.isEnabled(SymbolRole::FireBrace));
	CHECK(options.isEnabled(SymbolRole::Joint));
	// 名前は変更しない（取り込まない役割の名前は使われないだけで、削除はされない）。
	CHECK_EQ(options.symbol(SymbolRole::FireBrace), std::string("鋼製火打"));
}

TEST(import_options_set_symbol_does_not_re_enable_a_role)
{
	// 名前の差し替えと「取り込むか」は独立（片方を変更してもう片方が戻ると、
	// ダイアログの操作順で結果が変わってしまう）。
	ImportOptions options;
	options.setEnabled(SymbolRole::FloorPost, false);
	options.setSymbol(SymbolRole::FloorPost, "床束_大");
	CHECK(!options.isEnabled(SymbolRole::FloorPost));
	CHECK_EQ(options.symbol(SymbolRole::FloorPost), std::string("床束_大"));
}

TEST(import_options_empty_name_falls_back_to_default)
{
	// 空の名前は「名前の無いシンボルを置け」という命令になり、描画側で必ず失敗する。
	ImportOptions options;
	options.setSymbol(SymbolRole::Joint, "仕口_特");
	options.setSymbol(SymbolRole::Joint, "");
	CHECK_EQ(options.symbol(SymbolRole::Joint), std::string("仕口"));
}

TEST(import_options_title_block_is_off_by_default)
{
	// M28 既定は「図面枠を置かない」（設定ダイアログを出さなければ従来と同じ振る舞い）。
	const ImportOptions options;
	CHECK(!options.hasTitleBlock());
	CHECK(options.titleBlockStyle().empty());
}

TEST(import_options_title_block_keeps_an_empty_name_as_off)
{
	// **図面枠には既定名が無い**ので、空文字は「置かない」という意味をそのまま持つ
	// （シンボルの setSymbol が空を既定名へ戻すのとは逆。core/ImportOptions.h）。
	ImportOptions options;
	options.setTitleBlockStyle("名無し建築士事務所");
	CHECK(options.hasTitleBlock());
	CHECK_EQ(options.titleBlockStyle(), std::string("名無し建築士事務所"));

	options.setTitleBlockStyle("");
	CHECK(!options.hasTitleBlock());
	CHECK(options.titleBlockStyle().empty());
}

TEST(import_options_title_block_does_not_touch_the_symbol_roles)
{
	// 図面枠は役割の表に載らない別の設定——変更しても既定のシンボル対応は変わらない。
	ImportOptions options;
	options.setTitleBlockStyle("図面枠A");
	for (const auto& info : symbolRoles())
	{
		CHECK(options.isEnabled(info.role));
		CHECK_EQ(options.symbol(info.role), std::string(info.defaultSymbol));
	}
}

TEST(import_options_dimensions_are_off_by_default_and_keep_an_empty_name_as_off)
{
	// M31 既定は「寸法を入れない」。図面枠と同じく既定名が無いので、空文字は「入れない」
	// という意味をそのまま持つ。
	ImportOptions options;
	CHECK(!options.hasDimensions());
	CHECK(options.dimensionStandard().empty());

	options.setDimensionStandard("構造図 寸法");
	CHECK(options.hasDimensions());
	CHECK_EQ(options.dimensionStandard(), std::string("構造図 寸法"));
	// 図面枠とは別の設定。
	CHECK(!options.hasTitleBlock());

	options.setDimensionStandard("");
	CHECK(!options.hasDimensions());
	CHECK(options.dimensionStandard().empty());
}

TEST(import_options_merge_nothing_by_default)
{
	// 既定は「まとめない」＝横架材の高さごとに 1 枚ずつ伏図を作る（ご要望）。
	const ImportOptions options;
	CHECK(options.mergedPlanLevels.empty());
	CHECK(!options.mergesWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}));
}

TEST(import_options_merge_is_keyed_by_story_and_height)
{
	// 鍵は（階・高さ）の組。同じ高さでも階が違えば別のレベル。
	ImportOptions options;
	options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}, true);
	CHECK(options.mergesWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}));
	CHECK(!options.mergesWithPrevious(HomeskzIfcImport::core::PlanLevelKey{2, 3531}));
	CHECK(!options.mergesWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3530}));
	options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}, false);
	CHECK(options.mergedPlanLevels.empty());
}

TEST(import_options_merge_list_stays_sorted_without_duplicates)
{
	// 二分探索で検索するので、どの順に追加しても昇順・重複なしに保つ。
	ImportOptions options;
	options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{2, 6374}, true);
	options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{1, 3531}, true);
	options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{2, 6374}, true);
	options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{2, 6010}, true);
	CHECK_EQ(options.mergedPlanLevels.size(), std::size_t(3));
	CHECK((options.mergedPlanLevels[0] == HomeskzIfcImport::core::PlanLevelKey{1, 3531}));
	CHECK((options.mergedPlanLevels[1] == HomeskzIfcImport::core::PlanLevelKey{2, 6010}));
	CHECK((options.mergedPlanLevels[2] == HomeskzIfcImport::core::PlanLevelKey{2, 6374}));
	CHECK(options.mergesWithPrevious(HomeskzIfcImport::core::PlanLevelKey{2, 6010}));
	// 無いものを除外しても何も起きない。
	options.setMergeWithPrevious(HomeskzIfcImport::core::PlanLevelKey{3, 1}, false);
	CHECK_EQ(options.mergedPlanLevels.size(), std::size_t(3));
}

TEST(import_options_skip_no_sections_by_default)
{
	// M34 既定は「除外する通りなし」＝従来どおり全部描画する（設定ダイアログを出さない経路でも
	// 軸組図が消えない。core/ImportOptions.h の skippedSections）。
	const ImportOptions options;
	CHECK(options.skippedSections.empty());
	CHECK(!options.isSectionSkipped("X1"));
	CHECK(!options.isSectionSkipped(""));
}

TEST(import_options_skipped_sections_are_sorted_unique_and_non_empty)
{
	// 空文字・重複は除去し、名前順に並べ直す（ログの並びを選んだ順に依らせない）。
	ImportOptions options;
	options.setSkippedSections({"Y2", "", "X1", "Y2", "又い"});
	CHECK_EQ(options.skippedSections.size(), std::size_t(3));
	CHECK_EQ(options.skippedSections[0], std::string("X1"));
	CHECK_EQ(options.skippedSections[1], std::string("Y2"));
	CHECK(options.isSectionSkipped("X1"));
	CHECK(options.isSectionSkipped("又い"));
	CHECK(!options.isSectionSkipped("X2"));
	CHECK(!options.isSectionSkipped(""));

	// 差し替えは前の選択を残さない。
	options.setSkippedSections(std::vector<std::string>{});
	CHECK(options.skippedSections.empty());
	CHECK(!options.isSectionSkipped("X1"));
}

TEST(import_options_rafter_size_defaults_to_45_by_45)
{
	// 設定を変更しなければ従来の固定値 45×45 のまま（既定の取り込み結果を変えない）。
	const ImportOptions options;
	CHECK(std::abs(options.rafterWidth - 45.0) < 1e-9);
	CHECK(std::abs(options.rafterHeight - 45.0) < 1e-9);
	CHECK(std::abs(kDefaultRafterWidth - 45.0) < 1e-9);
	CHECK(std::abs(kDefaultRafterHeight - 45.0) < 1e-9);
}

TEST(import_options_set_rafter_size_rejects_unusable_values_per_dimension)
{
	ImportOptions options;
	options.setRafterSize(60.0, 90.0);
	CHECK(std::abs(options.rafterWidth - 60.0) < 1e-9);
	CHECK(std::abs(options.rafterHeight - 90.0) < 1e-9);

	// 受け付けない値は**その寸法だけ**既定へ戻す（もう一方は残す）。
	options.setRafterSize(0.0, 105.0);
	CHECK(std::abs(options.rafterWidth - kDefaultRafterWidth) < 1e-9);
	CHECK(std::abs(options.rafterHeight - 105.0) < 1e-9);
	options.setRafterSize(75.0, -1.0);
	CHECK(std::abs(options.rafterWidth - 75.0) < 1e-9);
	CHECK(std::abs(options.rafterHeight - kDefaultRafterHeight) < 1e-9);
	options.setRafterSize(std::numeric_limits<double>::quiet_NaN(), kMaxRafterSize + 1.0);
	CHECK(std::abs(options.rafterWidth - kDefaultRafterWidth) < 1e-9);
	CHECK(std::abs(options.rafterHeight - kDefaultRafterHeight) < 1e-9);
}

TEST(rafter_size_validity_range)
{
	CHECK(isValidRafterSize(0.1));
	CHECK(isValidRafterSize(45.0));
	CHECK(isValidRafterSize(kMaxRafterSize));
	CHECK(!isValidRafterSize(0.0));
	CHECK(!isValidRafterSize(-45.0));
	CHECK(!isValidRafterSize(kMaxRafterSize + 0.1));
	CHECK(!isValidRafterSize(std::numeric_limits<double>::infinity()));
	CHECK(!isValidRafterSize(std::numeric_limits<double>::quiet_NaN()));
}

TEST(parse_rafter_size_reads_plain_numbers)
{
	const std::optional<double> integer = parseRafterSize("60");
	CHECK(integer.has_value() && std::abs(*integer - 60.0) < 1e-9);
	const std::optional<double> decimal = parseRafterSize("45.5");
	CHECK(decimal.has_value() && std::abs(*decimal - 45.5) < 1e-9);
	const std::optional<double> leadingDot = parseRafterSize(".5");
	CHECK(leadingDot.has_value() && std::abs(*leadingDot - 0.5) < 1e-9);
	const std::optional<double> trailingDot = parseRafterSize("90.");
	CHECK(trailingDot.has_value() && std::abs(*trailingDot - 90.0) < 1e-9);
	// 前後の空白は読み飛ばす。
	const std::optional<double> spaced = parseRafterSize("  105\t");
	CHECK(spaced.has_value() && std::abs(*spaced - 105.0) < 1e-9);
}

TEST(parse_rafter_size_reads_full_width_digits)
{
	// 日本語入力のまま打たれた全角の数字・小数点も読む。
	const std::optional<double> fullWidth = parseRafterSize("４５");
	CHECK(fullWidth.has_value() && std::abs(*fullWidth - 45.0) < 1e-9);
	const std::optional<double> fullWidthDecimal = parseRafterSize("６０．５");
	CHECK(fullWidthDecimal.has_value() && std::abs(*fullWidthDecimal - 60.5) < 1e-9);
}

TEST(parse_rafter_size_rejects_anything_else)
{
	CHECK(!parseRafterSize("").has_value());
	CHECK(!parseRafterSize("   ").has_value());
	CHECK(!parseRafterSize(".").has_value());
	CHECK(!parseRafterSize("0").has_value());
	CHECK(!parseRafterSize("-45").has_value());
	CHECK(!parseRafterSize("+45").has_value());
	CHECK(!parseRafterSize("45mm").has_value());
	CHECK(!parseRafterSize("4 5").has_value());
	CHECK(!parseRafterSize("4.5.1").has_value());
	CHECK(!parseRafterSize("1e2").has_value());
	CHECK(!parseRafterSize("45,5").has_value());
	CHECK(!parseRafterSize("1000.1").has_value()); // 上限（kMaxRafterSize）超え
	CHECK(parseRafterSize("1000").has_value());
}

TEST(format_rafter_size_drops_trailing_zeros_and_round_trips)
{
	CHECK_EQ(formatRafterSize(45.0), std::string("45"));
	CHECK_EQ(formatRafterSize(45.5), std::string("45.5"));
	CHECK_EQ(formatRafterSize(105.0), std::string("105"));
	CHECK_EQ(formatRafterSize(0.5), std::string("0.5"));
	// 0.1mm 単位に丸める。
	CHECK_EQ(formatRafterSize(45.04), std::string("45"));
	CHECK_EQ(formatRafterSize(45.06), std::string("45.1"));
	for (const double mm : {45.0, 60.5, 105.0, 0.5, 999.9})
	{
		const std::optional<double> back = parseRafterSize(formatRafterSize(mm));
		CHECK(back.has_value() && std::abs(*back - mm) < 1e-9);
	}
}

// ---------------------------------------------------------------------------
// **図面にあるものから組む既定の設定**（M40。MCP から起こす実機テストの周）。設定ダイアログを
// まだ一度も決めていないときに開く初期値と同じになること。

TEST(default_dimension_standard_prefers_jis)
{
	CHECK_EQ(defaultDimensionStandardIndex({"ANSI", "JIS", "DIN"}), std::size_t{1});
	// JIS が無ければ最初。空でも 0（呼び出し側が範囲を確認する）。
	CHECK_EQ(defaultDimensionStandardIndex({"ANSI", "DIN"}), std::size_t{0});
	CHECK_EQ(defaultDimensionStandardIndex({}), std::size_t{0});
}

TEST(preset_import_options_enables_only_symbols_in_the_drawing)
{
	// 図面に既定名のシンボルがある役割だけ取り込む（無い名前は置きようがない）。
	const ImportOptions options =
		presetImportOptions({"床束", "鋼製火打", "柱伏図記号", "関係ないシンボル"}, {}, {});
	CHECK(options.isEnabled(SymbolRole::FloorPost));
	CHECK(options.isEnabled(SymbolRole::FireBrace));
	CHECK(options.isEnabled(SymbolRole::PlanMarkColumn));
	CHECK(!options.isEnabled(SymbolRole::AnchorBoltM12));
	CHECK(!options.isEnabled(SymbolRole::AnchorBoltM16));
	CHECK(!options.isEnabled(SymbolRole::Joint));
	CHECK(!options.isEnabled(SymbolRole::PlanMarkKoyazuka));
	CHECK(!options.isEnabled(SymbolRole::Splice));
	// 名前は既定名のまま（差し替えない）。
	CHECK_EQ(options.symbol(SymbolRole::FloorPost), std::string("床束"));
	// 図面枠・寸法規格の候補が無ければ置かない。
	CHECK(!options.hasTitleBlock());
	CHECK(!options.hasDimensions());
}

TEST(preset_import_options_places_the_first_title_block_and_jis)
{
	const ImportOptions options =
		presetImportOptions({}, {"図面枠A3", "図面枠A2"}, {"ANSI", "JIS"});
	CHECK_EQ(options.titleBlockStyle(), std::string("図面枠A3"));
	CHECK_EQ(options.dimensionStandard(), std::string("JIS"));
	// 残りは既定のまま（まとめない・全部描画する・45×45）。
	CHECK(options.mergedPlanLevels.empty());
	CHECK(options.skippedSections.empty());
	CHECK(std::fabs(options.rafterWidth - kDefaultRafterWidth) < 1e-9);
	CHECK(std::fabs(options.rafterHeight - kDefaultRafterHeight) < 1e-9);
	// JIS が無ければ最初の規格。
	CHECK_EQ(presetImportOptions({}, {}, {"ANSI", "DIN"}).dimensionStandard(), std::string("ANSI"));
}

TEST_MAIN();
