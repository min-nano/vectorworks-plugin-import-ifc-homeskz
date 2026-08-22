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
// インポート完了ダイアログの本文（M15「完了文言の集約」）
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

TEST(format_import_result_lists_only_elements_with_commands)
{
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 2;

	std::string const text = formatImportResult(sampleDocument(), counts);

	// 命令のある要素は「ラベル: 件数 助数詞」で並ぶ。
	CHECK(text.find("横架材: 3 本") != std::string::npos);
	CHECK(text.find("柱: 2 本") != std::string::npos);
	// 命令の無い要素（床・伏図…）は行ごと出さない。
	CHECK(text.find("床") == std::string::npos);
	CHECK(text.find("伏図") == std::string::npos);
	// 中止も診断も無いので、その断り書きは出ない。
	CHECK(text.find("キャンセル") == std::string::npos);
}

TEST(format_import_result_shows_shortfall_as_ratio)
{
	// 命令はあるのに描けなかった要素は「描けた数/命令数」の形にして、描画側の問題だと
	// 分かるようにする（配置先レイヤが無い・PIO を作れない等）。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 0;

	std::string const text = formatImportResult(sampleDocument(), counts);

	CHECK(text.find("横架材: 3 本") != std::string::npos);
	CHECK(text.find("柱: 0/2 本") != std::string::npos);
}

TEST(format_import_result_reports_cancel_and_diagnostics)
{
	DrawCounts counts;
	counts.valid = true;
	counts.members = 1;
	counts.columns = 0;
	counts.cancelled = true;
	counts.diagnostics = "横架材: 断面が入りませんでした";

	std::string const text = formatImportResult(sampleDocument(), counts);

	CHECK(text.find("横架材: 1/3 本") != std::string::npos);
	CHECK(text.find("キャンセルされたため、途中で中断しました。") != std::string::npos);
	CHECK(text.find("断面が入りませんでした") != std::string::npos);
}

TEST(format_import_result_points_at_the_trace_log)
{
	// 診断ログが有効なとき（dev ビルド / HOMESKZ_IFC_TRACE 指定時）は場所を必ず案内する。
	// 一時ディレクトリは macOS では /var/folders/… という当てられない場所なので、
	// 「ログはどこ？」を毎回聞かれないようにするため。
	DrawCounts counts;
	counts.valid = true;
	counts.members = 3;
	counts.columns = 2;

	std::string const text =
		formatImportResult(sampleDocument(), counts, "/tmp/HomeskzIfcImport.log");
	CHECK(text.find("診断ログ: /tmp/HomeskzIfcImport.log") != std::string::npos);

	// 無効なら案内も出ない（存在しない場所を指さない）。
	CHECK(formatImportResult(sampleDocument(), counts).find("診断ログ") == std::string::npos);
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
	CHECK(text.find("横架材") == std::string::npos);
}

TEST(format_import_result_reports_empty_document)
{
	// 命令が 1 件も無ければ「見つかりませんでした」。検証自体は空の Document でも通る。
	DrawCounts counts;
	counts.valid = true;
	std::string const text = formatImportResult(Document{}, counts);

	CHECK(text.find("見つかりませんでした") != std::string::npos);
}

TEST(document_command_count_sums_every_element_list)
{
	// **要素を足したときに数え漏らさない**ための番人。Document の各リストに 1 件ずつ
	// 入れたら、総数はリストの数と一致しなければならない（kElements の網羅性を固定する）。
	CHECK_EQ(documentCommandCount(fullDocument()), static_cast<std::size_t>(17));
	CHECK_EQ(documentCommandCount(Document{}), static_cast<std::size_t>(0));
}

TEST(format_import_result_covers_every_element)
{
	// 表（kElements）の**全行**を通し、表示名と助数詞をここで固定する。要素を足したときに
	// 表へ書き忘れれば上の総数のケースが、ラベルや助数詞を取り違えればこのケースが落ちる。
	std::string const text = formatImportResult(fullDocument(), fullCounts());

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

TEST(format_import_result_shortfall_for_every_element)
{
	// どの要素でも「描けた数/命令数」で出せること（描画側の取りこぼしを要素によらず
	// 同じ形で見せる）。全要素の命令はあるが 1 つも描けなかった、という極端な形で通す。
	DrawCounts counts; // 件数はすべて 0 のまま
	counts.valid = true;
	std::string const text = formatImportResult(fullDocument(), counts);

	CHECK(text.find("ストーリ: 0/1 層") != std::string::npos);
	CHECK(text.find("軸組図: 0/1 枚") != std::string::npos);
	CHECK(text.find("アンカーボルト: 0/1 本") != std::string::npos);
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
	// 診断ログが無効なら案内も出ない（存在しない場所を指さない）。
	CHECK(text.find("診断ログ") == std::string::npos);
}

TEST(format_import_error_points_at_the_trace_log)
{
	// ログが有効なら場所を案内する（最終行の直後が原因箇所。core/Trace.h）。
	std::string const text = formatImportError("bad_alloc", "/tmp/HomeskzIfcImport.log");
	CHECK(text.find("診断ログ: /tmp/HomeskzIfcImport.log") != std::string::npos);
}

TEST_MAIN();
