//
//	ParseSummaryTests.cpp
//
//	IFC 読み取りサマリ（src/parse/Summary）の単体テスト。無 SDK のテストハーネスで走る
//	（CLAUDE.md「テスト方針」）。ダイアログ表示（SDK 依存）を切り離し、件数集計と文言
//	整形だけを検証する。フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "parse/Loader.h"
#include "parse/Summary.h"

#include "core/Document.h"

#include <string>

using namespace HomeskzIfcImport::parse;
using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::DrawCounts;

namespace
{
	// counts の中から表示用 IFC 型名で件数を引く（見つからなければ最大値を返して
	// テストを分かりやすく落とす）。
	std::size_t countOf(const IfcSummary& summary, const std::string& ifcType)
	{
		for (const IfcTypeCount& c : summary.counts)
			if (c.ifcType == ifcType)
				return c.count;
		return static_cast<std::size_t>(-1);
	}
} // namespace

// ---------------------------------------------------------------------------
// 合成モデルでの集計
// ---------------------------------------------------------------------------

TEST(summarize_model_counts_known_types)
{
	// 通り芯 2・柱 1・金物 1。ほかの主要型は 0 件でも counts に並ぶ（固定順）。
	std::string const text = "DATA;\n"
							 "#1=IFCGRIDAXIS('X1',#10,.T.);\n"
							 "#2=IFCGRIDAXIS('X2',#10,.T.);\n"
							 "#3=IFCCOLUMN('c',$,'STANDCOLUMN',$,$,$,$,$,$);\n"
							 "#4=IFCMECHANICALFASTENER('m',$,$,$,$,$,$,$,$);\n"
							 "ENDSEC;\n";
	Model const model = loadIfcFromText(text);
	IfcSummary const summary = summarizeModel(model);

	CHECK(summary.loaded);
	// 主要型は固定の 7 種すべてが並ぶ。
	CHECK_EQ(summary.counts.size(), static_cast<std::size_t>(7));
	CHECK_EQ(countOf(summary, "IfcGridAxis"), static_cast<std::size_t>(2));
	CHECK_EQ(countOf(summary, "IfcColumn"), static_cast<std::size_t>(1));
	CHECK_EQ(countOf(summary, "IfcMechanicalFastener"), static_cast<std::size_t>(1));
	// 出現しない型も 0 件で含まれる（表示が入力でブレない）。
	CHECK_EQ(countOf(summary, "IfcBeam"), static_cast<std::size_t>(0));
	CHECK_EQ(countOf(summary, "IfcFooting"), static_cast<std::size_t>(0));
	// エンティティ総数は 4。
	CHECK_EQ(summary.entityCount, static_cast<std::size_t>(4));
}

TEST(summarize_model_order_is_grid_first)
{
	// 表示順の先頭は通り芯（M1 が最初の縦切り）。決定的な並びであることを固定する。
	Model const model = loadIfcFromText("#1=IFCGRIDAXIS('X1',$,.T.);\n");
	IfcSummary const summary = summarizeModel(model);
	CHECK(!summary.counts.empty());
	CHECK_EQ(summary.counts.front().ifcType, std::string("IfcGridAxis"));
	CHECK_EQ(summary.counts.front().label, std::string("通り芯"));
}

// ---------------------------------------------------------------------------
// 文言整形
// ---------------------------------------------------------------------------

TEST(format_summary_lists_counts)
{
	Model const model = loadIfcFromText("#1=IFCGRIDAXIS('X1',$,.T.);\n"
										"#2=IFCGRIDAXIS('X2',$,.T.);\n"
										"#3=IFCGRIDAXIS('X3',$,.T.);\n");
	std::string const text = formatSummary(summarizeModel(model));

	// 日本語ラベル・IFC 型名・件数が本文に出る（「通り芯 (IfcGridAxis): 3」）。
	CHECK(text.find("通り芯") != std::string::npos);
	CHECK(text.find("IfcGridAxis") != std::string::npos);
	CHECK(text.find("(IfcGridAxis): 3") != std::string::npos);
	CHECK(text.find("エンティティ総数: 3") != std::string::npos);
}

TEST(format_summary_reports_load_failure)
{
	// 読み込めなかったサマリ（loaded=false）は失敗メッセージになる。
	IfcSummary const failed; // 既定は loaded=false
	std::string const text = formatSummary(failed);
	CHECK(text.find("読み込めませんでした") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(summarize_ifc_reads_fixture)
{
	std::string const path = HomeskzIfcTests::fixturePath("minimal_grid.ifc");
	IfcSummary const summary = summarizeIfc(path);

	CHECK(summary.loaded);
	// minimal_grid.ifc は通り芯 3・階 1（LoaderTests と同じ既知の内訳）。
	CHECK_EQ(countOf(summary, "IfcGridAxis"), static_cast<std::size_t>(3));
	CHECK_EQ(countOf(summary, "IfcBuildingStorey"), static_cast<std::size_t>(1));
	CHECK(summary.entityCount > 0);
}

TEST(summarize_ifc_missing_file_reports_not_loaded)
{
	IfcSummary const summary = summarizeIfc(HomeskzIfcTests::fixturePath("does_not_exist.ifc"));
	CHECK(!summary.loaded);
	CHECK(summary.counts.empty());
	CHECK(formatSummary(summary).find("読み込めませんでした") != std::string::npos);
}

// ---------------------------------------------------------------------------
// インポート完了ダイアログの本文（M15「完了文言の集約」／M19「短い完了・厚いログ」）
// ---------------------------------------------------------------------------

namespace
{
	// 横架材 3 本・柱 2 本だけを持つ最小の命令セット。中身は見ないので既定値のままでよい
	// （formatImportResult が読むのは**件数**だけ）。
	Document sampleDocument()
	{
		Document document;
		document.members.resize(3);
		document.columns.resize(2);
		return document;
	}

	// 全要素に 1 件ずつ入れた命令セット。**要素表（Summary.cpp の kElements）を端から端まで
	// 通す**ために使う——要素を足したときに表へ書き忘れると、下の 2 つのケースが落ちる。
	Document fullDocument()
	{
		Document document;
		document.stories.resize(1);
		document.grids.resize(1);
		document.walls.resize(1);
		document.wallJoins.resize(1);
		document.slabs.resize(1);
		document.floors.resize(1);
		document.members.resize(1);
		document.columns.resize(1);
		document.rafters.resize(1);
		document.roofs.resize(1);
		document.anchorBolts.resize(1);
		document.floorPosts.resize(1);
		document.fireBraces.resize(1);
		document.joints.resize(1);
		document.columnMarks.resize(1);
		document.sheets.resize(1);
		document.sections.resize(1);
		return document;
	}

	// fullDocument をすべて描けた場合の件数。
	DrawCounts fullCounts()
	{
		DrawCounts counts;
		counts.valid = true;
		counts.stories = 1;
		counts.grids = 1;
		counts.walls = 1;
		counts.wallJoins = 1;
		counts.slabs = 1;
		counts.floors = 1;
		counts.members = 1;
		counts.columns = 1;
		counts.rafters = 1;
		counts.roofs = 1;
		counts.anchorBolts = 1;
		counts.floorPosts = 1;
		counts.fireBraces = 1;
		counts.joints = 1;
		counts.columnMarks = 1;
		counts.sheets = 1;
		counts.sections = 1;
		return counts;
	}
} // namespace

TEST(format_import_result_is_short_and_names_the_file)
{
	// **完了ダイアログは短く保つ**（M19）。読むのは「どのファイルを・成功したのか・
	// 問題はあったのか」の 3 つで、要素ごとの内訳はログにある。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 2;

	ImportInfo info;
	info.fileName = "安藤邸.ifc";
	info.seconds = 71.4;

	std::string const text = formatImportResult(sampleDocument(), counts, info);

	CHECK(text.find("取り込みが完了しました。") != std::string::npos);
	CHECK(text.find("ファイル: 安藤邸.ifc") != std::string::npos);
	// 描いた数は**総数 1 行だけ**。要素ごとの行は出さない。
	CHECK(text.find("描いたもの: 5 件") != std::string::npos);
	CHECK(text.find("所要 1 分 11 秒") != std::string::npos);
	CHECK(text.find("横架材") == std::string::npos);
	CHECK(text.find("柱:") == std::string::npos);
	// 問題が無いのだから、ログを見に行かせる案内も出さない。
	CHECK(text.find("ログを表示") == std::string::npos);
}

TEST(format_import_result_flags_a_shortfall_as_a_problem)
{
	// 命令はあるのに描けなかったなら「うまくいかなかったところがある」と言い、
	// 内訳（どの要素が何本足りないか）はログへ送る。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 0;

	std::string const text = formatImportResult(sampleDocument(), counts);

	CHECK(text.find("うまくいかなかったところがあります") != std::string::npos);
	CHECK(text.find("描いたもの: 3/5 件") != std::string::npos);
	CHECK(text.find("内訳と原因はログにあります") != std::string::npos);
}

TEST(format_import_result_flags_draw_diagnostics_as_a_problem)
{
	// 件数が揃っていても描画側が異常を持ち帰ったなら「問題あり」。**平常の記録（notes）は
	// 問題にしない**——毎回出るものを問題にすると、ダイアログが常に「問題あり」になる。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 2;
	counts.diagnostics = "横架材: 断面が入りませんでした";
	CHECK(formatImportResult(sampleDocument(), counts).find("うまくいかなかった") !=
		  std::string::npos);

	DrawCounts quiet;
	quiet.valid = true;
	quiet.members = 3;
	quiet.columns = 2;
	quiet.notes = "伏図の割り付け（mm）: 用紙 594×420";
	CHECK(formatImportResult(sampleDocument(), quiet).find("取り込みが完了しました") !=
		  std::string::npos);
	// 記録はダイアログには出さない（ログにだけ出る）。
	CHECK(formatImportResult(sampleDocument(), quiet).find("割り付け") == std::string::npos);
}

TEST(format_import_result_tells_to_update_viewports_only_when_drawings_were_made)
{
	// **取り込み直後の伏図・軸組図は 1 回の「更新」が要る**（VW はレイヤを高さの降順で描くので、
	// 床仕上げ天端が構造天端より上にある以上、そのままでは床が柱・梁を覆う。並べた重ね順は
	// 図面には入っていて、更新すればそちらで描き直される。docs/DEV-NOTES.md）。
	// 黙って誤った絵を見せないよう、図を 1 枚でも作ったなら必ず伝える。
	DrawCounts drawn;
	drawn.valid = true;
	drawn.sheets = 1;
	CHECK(formatImportResult(sampleDocument(), drawn).find("1 回「更新」") != std::string::npos);

	DrawCounts sections;
	sections.valid = true;
	sections.sections = 1;
	CHECK(formatImportResult(sampleDocument(), sections).find("1 回「更新」") != std::string::npos);

	// 図を 1 枚も作っていない取り込みでは出さない（関係のない案内で埋めない）。
	DrawCounts none;
	none.valid = true;
	none.members = 3;
	none.columns = 2;
	CHECK(formatImportResult(sampleDocument(), none).find("1 回「更新」") == std::string::npos);
}

TEST(format_import_result_reports_cancel)
{
	// 中止は「描き切れなくて当然」なので、問題あり扱いにしない（原因を探しに行かせない）。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 1;
	counts.cancelled = true;

	std::string const text = formatImportResult(sampleDocument(), counts);

	CHECK(text.find("キャンセルされたため、途中で中断しました") != std::string::npos);
	CHECK(text.find("うまくいかなかった") == std::string::npos);
}

TEST(format_import_result_points_at_the_log)
{
	// **ログの場所は必ず出す**（不具合の報告でファイルごと添えるときの唯一の手掛かり。
	// 一時ディレクトリは macOS では /var/folders/… という当てられない場所にある）。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 2;

	ImportInfo info;
	info.logPath = "/tmp/HomeskzIfcImport.log";
	CHECK(formatImportResult(sampleDocument(), counts, info)
			  .find("ログ: /tmp/HomeskzIfcImport.log") != std::string::npos);

	// 開けなかったときは案内も出ない（存在しない場所を指さない）。
	CHECK(formatImportResult(sampleDocument(), counts).find("ログ: ") == std::string::npos);
}

TEST(format_import_result_tells_how_far_undo_reaches)
{
	// 図形を描いたなら**取り消しがどこまで効くか**を必ず 1 行で伝える（ユーザーは
	// 「間違えたら取り消せばいい」と考えるのが自然なので、戻せる／一部だけ／戻せないの
	// 区別を黙らない。判断材料は描画側が置く。core::DrawCounts）。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 2;

	// (1) undo イベントを張れた＝1 回で戻せる。
	counts.undoArmed = true;
	CHECK(formatImportResult(sampleDocument(), counts).find("「取り消し」1 回で元に戻せます") !=
		  std::string::npos);

	// (2) 取り込み前から在ったレイヤへも描いた＝その分は戻らない。
	counts.undoPartial = true;
	CHECK(formatImportResult(sampleDocument(), counts).find("新しく作ったレイヤの分だけ") !=
		  std::string::npos);

	// (3) そもそも張れなかった＝戻せない。保存せずに閉じるしかない。
	counts.undoArmed = false;
	counts.undoPartial = false;
	CHECK(formatImportResult(sampleDocument(), counts).find("「取り消し」では戻せません") !=
		  std::string::npos);

	// 何も描いていないとき（命令が 0 件）は取り消しの話をしない。
	DrawCounts empty;
	empty.valid = true;
	CHECK(formatImportResult(Document{}, empty).find("取り消し") == std::string::npos);
}

TEST(format_import_result_reports_invalid_document)
{
	// 検証に落ちたときは何も描いていないので、件数は並べず理由だけを返す。
	DrawCounts const counts; // 既定は valid=false
	std::string const text = formatImportResult(sampleDocument(), counts);

	CHECK(text.find("検証に通らなかった") != std::string::npos);
	CHECK(text.find("描いたもの") == std::string::npos);
}

TEST(format_import_result_reports_empty_document)
{
	// 命令が 1 件も無ければ「見つかりませんでした」。検証自体は空の Document でも通る。
	DrawCounts counts;
	counts.valid = true;
	std::string const text = formatImportResult(Document{}, counts);

	CHECK(text.find("見つかりませんでした") != std::string::npos);
}

TEST(import_outcome_classifies_the_run)
{
	// **ダイアログもログも同じ判断を使う**ので、「成功と言いながらログには問題が並ぶ」
	// という食い違いが起きない。ここでその判断そのものを固定する。
	DrawCounts success;
	success.valid = true;
	success.members = 3;
	success.columns = 2;
	CHECK(importOutcome(sampleDocument(), success).status == ImportStatus::Success);
	CHECK_EQ(importOutcome(sampleDocument(), success).placed, static_cast<std::size_t>(5));
	CHECK_EQ(importOutcome(sampleDocument(), success).commands, static_cast<std::size_t>(5));

	DrawCounts warning = success;
	warning.columns = 1;
	CHECK(importOutcome(sampleDocument(), warning).status == ImportStatus::Warning);

	// 中止は Warning より優先（描き切れないのが当たり前だから）。
	DrawCounts cancelled = warning;
	cancelled.cancelled = true;
	CHECK(importOutcome(sampleDocument(), cancelled).status == ImportStatus::Cancelled);

	DrawCounts const invalid; // valid=false
	CHECK(importOutcome(sampleDocument(), invalid).status == ImportStatus::Invalid);

	DrawCounts empty;
	empty.valid = true;
	CHECK(importOutcome(Document{}, empty).status == ImportStatus::Empty);
}

TEST(document_command_count_sums_every_element_list)
{
	// **要素を足したときに数え漏らさない**ための番人。Document の各リストに 1 件ずつ
	// 入れたら、総数はリストの数と一致しなければならない（kElements の網羅性を固定する）。
	CHECK_EQ(documentCommandCount(fullDocument()), static_cast<std::size_t>(17));
	CHECK_EQ(documentCommandCount(Document{}), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// 診断ログの本文（M19「短い完了・厚いログ」）
// ---------------------------------------------------------------------------

TEST(format_log_header_names_the_build_time_and_file)
{
	// 報告に貼られたログから最初に知りたいのは「どのリビジョンを・いつ・どのファイルに
	// 対して動かしたか」。その 3 つが必ず頭に並ぶ。
	BuildInfo build;
	build.plugin = "HomeskzIfcImportDev";
	build.channel = "dev";
	build.commit = "1a2b3c4";
	build.branch = "claude/example";
	build.platform = "macOS";

	std::string const text =
		formatLogHeader(build, "/Users/x/安藤邸.ifc", 12876543ULL, "2026-08-29 14:03:21");

	CHECK(text.find("日時: 2026-08-29 14:03:21") != std::string::npos);
	CHECK(text.find("HomeskzIfcImportDev") != std::string::npos);
	CHECK(text.find("dev") != std::string::npos);
	CHECK(text.find("commit 1a2b3c4") != std::string::npos);
	CHECK(text.find("branch claude/example") != std::string::npos);
	CHECK(text.find("macOS") != std::string::npos);
	// 対象は**フルパス**（同じ名前の IFC を版ごとに持つのが普通なので名前だけでは足りない）。
	CHECK(text.find("対象: /Users/x/安藤邸.ifc") != std::string::npos);
	CHECK(text.find("12.3 MB") != std::string::npos);
}

TEST(format_log_header_fills_unknown_fields)
{
	// 素性が分からなくても行は落とさない（「無い」ことも情報なので黙らない）。
	std::string const text = formatLogHeader(BuildInfo{}, "", 0, "");
	CHECK(text.find("日時: 不明") != std::string::npos);
	CHECK(text.find("ビルド: 不明") != std::string::npos);
	CHECK(text.find("対象: 不明") != std::string::npos);
	// 大きさが分からないときは括弧ごと出さない。
	CHECK(text.find("（0") == std::string::npos);
}

TEST(format_log_result_lists_every_element_and_the_verdict)
{
	// 表（kElements）の**全行**を通し、表示名と助数詞をここで固定する。要素を足したときに
	// 表へ書き忘れれば document_command_count のケースが、ラベルや助数詞を取り違えれば
	// このケースが落ちる。**完了ダイアログから外した内訳はここにある。**
	std::string const text = formatLogResult(fullDocument(), fullCounts(), 71.4);

	CHECK(text.find("結果: 成功") != std::string::npos);
	CHECK(text.find("所要: 1 分 11 秒") != std::string::npos);
	CHECK(text.find("描いたもの: 17 件") != std::string::npos);
	CHECK(text.find("ストーリ: 1 層") != std::string::npos);
	CHECK(text.find("通り芯: 1 本") != std::string::npos);
	CHECK(text.find("立上り: 1 本") != std::string::npos);
	CHECK(text.find("壁結合: 1 箇所") != std::string::npos);
	CHECK(text.find("底盤: 1 枚") != std::string::npos);
	CHECK(text.find("床: 1 枚") != std::string::npos);
	CHECK(text.find("横架材: 1 本") != std::string::npos);
	CHECK(text.find("柱: 1 本") != std::string::npos);
	CHECK(text.find("垂木: 1 本") != std::string::npos);
	CHECK(text.find("野地板: 1 枚") != std::string::npos);
	CHECK(text.find("アンカーボルト: 1 本") != std::string::npos);
	CHECK(text.find("床束: 1 本") != std::string::npos);
	CHECK(text.find("火打: 1 本") != std::string::npos);
	CHECK(text.find("仕口: 1 箇所") != std::string::npos);
	CHECK(text.find("柱記号: 1 個") != std::string::npos);
	CHECK(text.find("伏図: 1 枚") != std::string::npos);
	CHECK(text.find("軸組図: 1 枚") != std::string::npos);
	// 並びは draw/ExecuteDocument のディスパッチ順（ストーリが先頭・軸組図が末尾）。
	CHECK(text.find("ストーリ: 1 層") < text.find("軸組図: 1 枚"));
}

TEST(format_log_result_shows_shortfall_notes_and_records)
{
	// 描けなかったぶんは「描けた数/命令数」で、原因（注意）と平常の記録（記録）は
	// 見出しを分けて並べる——前者だけが「問題あり」の根拠になるから。
	DrawCounts counts; // 件数はすべて 0 のまま
	counts.valid = true;
	counts.diagnostics = "柱: 断面が入りませんでした";
	counts.notes = "伏図の割り付け（mm）: 用紙 594×420";

	std::string const text = formatLogResult(fullDocument(), counts, 0.0);

	CHECK(text.find("結果: 問題あり") != std::string::npos);
	CHECK(text.find("ストーリ: 0/1 層") != std::string::npos);
	CHECK(text.find("軸組図: 0/1 枚") != std::string::npos);
	CHECK(text.find("アンカーボルト: 0/1 本") != std::string::npos);
	CHECK(text.find("注意:\n  柱: 断面が入りませんでした") != std::string::npos);
	CHECK(text.find("記録:\n  伏図の割り付け") != std::string::npos);
	// 所要が分からない（0）ときは行ごと出さない。
	CHECK(text.find("所要:") == std::string::npos);
}

TEST(format_log_result_reports_cancel_and_invalid)
{
	DrawCounts cancelled;
	cancelled.valid = true;
	cancelled.members = 1;
	cancelled.cancelled = true;
	CHECK(formatLogResult(sampleDocument(), cancelled, 1.0).find("結果: 中断（キャンセル）") !=
		  std::string::npos);

	DrawCounts const invalid; // valid=false
	std::string const text = formatLogResult(sampleDocument(), invalid, 1.0);
	CHECK(text.find("結果: 失敗") != std::string::npos);
	// 何も描いていないので内訳は並べない。
	CHECK(text.find("横架材") == std::string::npos);
}

TEST(format_import_error_includes_detail)
{
	// 例外の説明はそのまま本文へ載せる（ネイティブの異常は再現条件が分からなくなりがちで、
	// ここで捨てるとユーザーからは「黙って止まった」としか見えない）。
	std::string const text = formatImportError("bad_alloc");
	CHECK(text.find("予期しないエラー") != std::string::npos);
	CHECK(text.find("詳細: bad_alloc") != std::string::npos);
}

TEST(format_import_error_without_detail_says_unknown)
{
	// std::exception ですらないものを受けたときは説明が無い（catch(...)）。
	std::string const text = formatImportError("");
	CHECK(text.find("詳細: 原因不明") != std::string::npos);
	// ログを開けなかったなら場所は案内しない（存在しない場所を指さない）。
	CHECK(text.find("ログ: ") == std::string::npos);
}

TEST(format_import_error_points_at_the_log)
{
	// ログの最終行の直後が原因箇所（core/Trace.h）。場所とファイル名を添える。
	ImportInfo info;
	info.fileName = "安藤邸.ifc";
	info.logPath = "/tmp/HomeskzIfcImport.log";
	std::string const text = formatImportError("bad_alloc", info);
	CHECK(text.find("ファイル: 安藤邸.ifc") != std::string::npos);
	CHECK(text.find("ログ: /tmp/HomeskzIfcImport.log") != std::string::npos);
}

TEST_MAIN();
