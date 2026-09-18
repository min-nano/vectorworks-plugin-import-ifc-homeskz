//
//	parse/Regression.h
//
//	**回帰テストの結果と、その基準**（M28）。dev だけのメニュー「回帰テストを実行…」
//	（src/draw/Regression.h）が、指定フォルダの IFC を 1 件ずつ取り込んだ結果をここへ渡し、
//	**前回の結果（基準）と引き比べて**何が動いたかを出す。
//
//	【なぜ要るか】このリポジトリは公開されているので、過去の物件の IFC をフィクスチャとして
//	置けない。一方で退行を一番よく捕まえるのは「実際にやった物件を通し直すこと」である。
//	リポジトリの外に置いたままの物件を**利用者の実機で**回し、前回との差を見る——それが
//	この仕組みで、無 SDK のテスト（tests/）では届かない**描画まで込みの退行**を数字で拾う。
//
//	【何を「退行」と見るか】2 つある。**どちらか片方でも出たら読む側に知らせる。**
//	  1. **基準との差**……前回と件数が違う（`RegressionVerdict`）。直した結果どう動いたかを
//	     見るのはこちらで、判定は内訳の 1 行表現（parse/Feedback の `formatTally`）の
//	     突き合わせ——**実機フィードバックの往復と同じ道具を使う**（同じ比較を 2 つ書かない）。
//	  2. **基準に依らない異常**……例外で中断した・検証に落ちた・命令の数だけ描けなかった・
//	     描画側が注意を持ち帰った（`RegressionEntry::status` が「成功」以外）。**基準が
//	     その異常ごと記録されていても見逃さない**ために、1 とは別に数える。
//
//	【基準はフォルダの中に置く】置き場所は対象フォルダの直下（`kBaselineFileName`）。
//	物件と一緒に動くので、フォルダを別の機械へ持っていっても基準が付いてくる。形は
//	`key=value` の行で、`file=` が 1 件の始まり——**壊れた行と知らない鍵は飛ばして読み
//	続ける**（core::FeedbackSession の記憶と同じ作法。古い版が書いたものも読める）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を include しない（CLAUDE.md「Phase 1」）。
//	ここは Document と DrawCounts を読んで文字列を組むだけなので、無 SDK で単体テスト
//	できる（tests/ParseRegressionTests.cpp）。
//

#pragma once

#include "core/Document.h"
#include "parse/Summary.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// **基準ファイルの名前**（対象フォルダの直下に置く）。**この綴りが唯一**——書く側と
	// 読む側が別々に持つと、片方を直したときに基準が行方不明になる。
	inline constexpr const char* kBaselineFileName = "min-nano_structure-regression.txt";

	// 1 件ぶんの結果。**基準ファイルへ書くのはこの形そのもの**で、次回はこれを読み戻して
	// 突き合わせる。
	struct RegressionEntry
	{
		// 見出し（フォルダに並べたファイル名）。**基準と今回を結ぶ鍵**なので、実体のパス
		// ではなく並べた名前を使う（実体を移しても、同じ名前で並べ直せば繋がったまま）。
		std::string name;
		// 結末の語（`importStatusWord`。取り込みが例外で中断したときは "エラー"）。
		std::string status;
		// 内訳の 1 行表現（`formatTally`）。比較はこれ 1 本で行う。
		std::string tally;
		// 所要（秒）。**比較には使わない**——機械や状況で動くので、退行の根拠にならない。
		// 記録に出して「急に遅くなった」に気付ける材料にするだけ。
		double seconds = 0.0;
		// エラーの説明（status が "エラー" のときだけ。空でもよい）。
		std::string detail;
	};

	// 取り込みが例外で中断した件の結末の語。**"成功" 以外なら異常**という判定に乗せる
	// ため、他の結末（`importStatusWord`）と同じ並びの語にしてある。
	inline constexpr const char* kRegressionErrorWord = "エラー";

	// 基準との突き合わせの結果。
	enum class RegressionVerdict
	{
		Same,	 // 基準と内訳も結末も同じ
		Changed, // 内訳か結末が動いた
		Added,	 // 基準に無かった（新しく並べた物件）
		Missing, // 基準にあるのに今回は走らなかった
	};

	// 1 件ぶんの突き合わせ。
	struct RegressionCompare
	{
		std::string name;
		RegressionVerdict verdict = RegressionVerdict::Same;
		// **基準に依らない異常**（今回の結末が "成功" ではない）。Missing のときは常に false
		// ——走っていないものに異常も正常も無い。
		bool abnormal = false;
		std::string status;		  // 今回の結末（Missing のときは基準の結末）
		std::string statusBefore; // 基準の結末（変わったときだけ出す）
		std::string detail;		  // エラーの説明（あれば）
		std::string diff;	  // 内訳の差分（`formatTallyDiff` の本文。無ければ空）
		double seconds = 0.0; // 今回の所要
	};

	// 全体の勘定。
	struct RegressionSummary
	{
		std::size_t total = 0;	 // 今回走らせた件数
		std::size_t same = 0;	 //
		std::size_t changed = 0; //
		std::size_t added = 0;	 //
		std::size_t missing = 0; //
		std::size_t abnormal = 0; // 結末が "成功" でなかった件数（上の分類とは独立）
		bool baselineKnown = false; // 基準があったか（無ければ差分は出せない）
		bool cancelled = false;		// 途中で中止した
	};

	// ------------------------------------------------------------------------
	// 1 件ぶんの結果を作る
	// ------------------------------------------------------------------------

	// 取り込みが通った件。結末は `importOutcome` / `importStatusWord`、内訳は
	// `elementRows` → `formatTally`——**どれも取り込みの報告が使っているものと同じ**で、
	// ここで新しい数え方を作らない。
	RegressionEntry regressionEntry(const std::string& name, const core::Document& document,
									const core::DrawCounts& counts, double seconds);

	// 取り込みが例外で中断した件（`ImportRound::failed`）。
	RegressionEntry regressionErrorEntry(const std::string& name, const std::string& detail,
										 double seconds);

	// ------------------------------------------------------------------------
	// 基準ファイル
	// ------------------------------------------------------------------------

	// **基準ファイルの置き場所**（対象フォルダの直下）。物件と一緒に動くので、フォルダごと
	// 別の機械へ持っていっても基準が付いてくる。
	std::string regressionBaselinePath(const std::string& folder);

	// 基準ファイルを読む／書く。**無くても異常ではない**（初めて走らせたとき）ので、
	// 読めなければ false を返すだけで、呼び出し側は「基準が無い」として続ける。
	bool readRegressionBaseline(const std::string& path, std::string& text);
	bool writeRegressionBaseline(const std::string& path, const std::string& text);

	// 基準ファイルの本文を組み立てる。build は走らせたビルドの素性、recordedAt は壁時計
	// （`core::trace::localTimestamp`）。
	std::string formatRegressionBaseline(const std::vector<RegressionEntry>& entries,
										 const BuildInfo& build, const std::string& recordedAt);

	// 基準ファイルを読む。**壊れた行と知らない鍵は飛ばす**（読めた分だけ返す）。
	std::vector<RegressionEntry> parseRegressionBaseline(const std::string& text);

	// 基準の素性を 1 行で（"2026-09-18 12:34:56 / abc1234 (main)"）。読めなければ空。
	// **どの版で採った基準と引き比べているか**は、差分を読む前に知りたい 1 行である。
	std::string regressionBaselineOrigin(const std::string& text);

	// ------------------------------------------------------------------------
	// 突き合わせと文面
	// ------------------------------------------------------------------------

	// 基準と今回を突き合わせる。**今回走らせた順**（＝名前の昇順）に並べ、基準にしか
	// 無かったものを最後へ足す。
	std::vector<RegressionCompare> compareRegression(const std::vector<RegressionEntry>& baseline,
													 const std::vector<RegressionEntry>& current);

	// 勘定を取る。baselineKnown が false なら差分の分類は意味を持たないので、すべて Added
	// として数えたうえで「基準を作った」と読ませる。
	RegressionSummary summarizeRegression(const std::vector<RegressionCompare>& compares,
										  bool baselineKnown, bool cancelled);

	// **走らせる前に出す問いの本文**（draw/Regression が `AlertQuestion` に渡す）。
	// **尋ねるのは始まる前だけ**——何十分も無人で回るものの終わりに確認が待っていると、
	// その人は席を離れられない（CLAUDE.md「取り込みのあとに人の操作を残さない」と同じ
	// 理屈）。found は見つかった件数、origin は `regressionBaselineOrigin`（基準が無ければ空）。
	std::string formatRegressionPrompt(std::size_t found, std::size_t baselineCount,
									   const std::string& origin);

	// **結果ダイアログの短い本文。** 読むのは「何件走ったか・基準から動いたか・異常は
	// あったか」の 3 つだけで済むよう短く保つ（完了ダイアログと同じ方針。parse/Summary.h）
	// ——件ごとの中身はログにある。
	std::string formatRegressionResult(const RegressionSummary& summary, bool baselineWritten);

	// **診断ログの本文**（件ごとの結末・内訳の差分・走査で気付いたこと）。notes は
	// `core::FixtureScan::notes`（辿れなかったもの等。空でよい）。
	std::string formatRegressionLog(const std::vector<RegressionCompare>& compares,
									const RegressionSummary& summary,
									const std::vector<std::string>& notes);
} // namespace HomeskzIfcImport::parse
