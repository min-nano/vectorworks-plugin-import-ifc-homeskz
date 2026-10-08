//
//	parse/Feedback.h
//
//	**実機テストの報告**（docs/DEV-NOTES.md M23 / M38）。開発版ビルドの MCP の
//	`vw_run_test` が取り込みを実行した結果を、Claude が読む Markdown へ
//	組み立てる。報告は手元のファイルに残り、ローカルの Claude Code が MCP の
//	`vw_test_report` で読む。
//
//	【何を載せるか】
//	  * 結末・所要時間・**要素ごとの内訳**（parse/Summary の elementRows。表は 1 つきり）
//	  * 取り込み前から在ったレイヤの枚数（描画結果の破綻を実装のせいにしないための 1 行）
//	  * 描画側が記録した注意（`DrawCounts::diagnostics`）と記録（`notes`）
//	  * 診断ログ（上限を超える分は古いほうから削除する）
//
//	【前の周と比べない（M43）】報告は 1 周ぶんで完結させる。前の周の報告は、周を起こした
//	ローカルの Claude Code のセッションが持っているので、比べるのはそちらの仕事である
//	（M42 までは前の周の内訳と 1 周目のレイヤ構成を記憶して差分を載せていた）。
//
//	【所見はここに載らない】**実機を確認した人の所見**は人が Claude とのチャットへ直接書く
//	（docs/DEV-NOTES.md M23「所見はプラグインの仕事ではなかった」）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を include しない。ここは Document と
//	DrawCounts を読むだけの純粋な文字列組み立てなので、無 SDK で単体テストできる。
//
//	【なぜ要るか】`draw/` の実描画は CI では検証できず、ローカルの VectorWorks でしか
//	確かめられない（CLAUDE.md「テスト方針」）。その確認結果を人が手で書き写して伝えている
//	限り、1 往復ごとに「ファイルを選び直す・ログを貼る」という手間がかかる。**プラグイン自身に
//	報告させれば、人がするのは「描画結果を確認して一言書く」だけになる。**
//
//	【伏せない】M37 までは報告を PR へコメントとして投稿しており、PR コメントは公開される
//	のでファイル名・ユーザー名・図面枠のスタイル名を伏せていた。報告は利用者の計算機の
//	中だけで読まれるので、伏せる理由が無い。
//

#pragma once

#include "core/Document.h"
#include "parse/Summary.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::parse
{
	// 1 周ぶんの材料。**描画側（draw/Feedback）が詰めて渡すだけ**で、ここは受け取った
	// 値を並べる（完了ダイアログの文言と同じ分担。parse/Summary.h）。
	struct FeedbackRound
	{
		BuildInfo build; // 動いていたビルド（ブランチ・コミット・プラットフォーム）
		std::string ifcPath; // 取り込んだ IFC の絶対パス（報告にはファイル名を出す）
		unsigned long long bytes = 0; // 対象ファイルの大きさ（0 なら出さない）
		double seconds = 0.0;		  // 所要（0 以下なら出さない）
		std::string startedAt;		  // 壁時計（core::trace::localTimestamp）
		std::string log;			  // 診断ログ全文（core::trace::text）

		// **取り込みの前に図面へ何をしたか**（draw/Feedback の openRoundDocument が
		// 返す 1 行。空なら出さない）。診断ログにも同じ行が入るが、**ログは上限で切り詰め
		// られるので、そこだけを頼りにしない**——図面をどう用意したか（前の周の図面を閉じて
		// テンプレートから開いたか。M39）は「図面の状態」と並べて読みたい重要な情報である
		// （実機 round 2 で、この行がログの省略部分へ落ちて読めなかった）。
		std::string preparation;

		// 案件が分かるものを伏せるか。**どこからも読まれていない**（M38 で伏せるのをやめた。
		// ヘッダ冒頭「伏せない」）。
		bool anonymize = true;
	};

	// -----------------------------------------------------------------------
	// **実機テストの周の結末**（M25）。MCP の応答
	// （`vw_run_test`）に、**このコマンド自身の言葉で**短く伝える。
	//
	// **取り込みコマンドの完了文言（`formatImportResult` / `formatImportError`）を流用しない。**
	// 流用すると、押した人には本番の取り込みが同じことをしているように見える——コマンドを
	// 分けた意味が見た目の上で崩れる（実機の指摘。docs/DEV-NOTES.md M25）。
	enum class TestRoundOutcome
	{
		// 取り込みを終えた（内訳は報告にある）。detail は報告の在り処など。
		Completed,
		// **描画先の図面を用意できなかった**ので、取り込みを始めなかった。実機 round 9 で、
		// 前の周の図面を閉じたあと作業ファイルを開き直せず、**どこにも属さない状態で
		// 描画して全 18 要素が 0 件**になった——数字だけ見れば「全部描画できなかった」だが、
		// 実際には描画先が無かっただけである。**そうなる前に止める。**
		DocumentFailed,
		ImportFailed, // 取り込みがエラーで中断した
		// **頼まれた中身を使えない**（M40 / M43。MCP の `vw_run_test` に IFC やテンプレートが
		// 渡されていない・見つからない・テンプレートが `.sta` でない・settings を読めない）。
		// 何も描画していない。
		InvalidRequest,
	};

	// detail は理由（空でもよい）。返るのは**短い本文**で、内訳と診断ログは報告が持つ。
	std::string formatTestRoundResult(TestRoundOutcome outcome, const std::string& detail);

	// **長い本文の末尾だけを残す**（診断ログの切り詰め。報告と MCP の `vw_log` が使う）。
	// maxBytes を超えるときは**古いほう（先頭）を削除し**、「前半 N バイトを省略」の 1 行を
	// 頭に付ける。切り口は **UTF-8 の文字境界の次の行頭**——3 バイトの日本語の途中で切ると
	// 壊れた UTF-8 になり、JSON の読み手に拒否される（docs/DEV-NOTES.md M25）。
	std::string keepTail(const std::string& text, std::size_t maxBytes);

	// **実機テストの報告**（Markdown）。先頭の見出しにどのビルドかを置く。
	std::string formatTestRoundReport(const FeedbackRound& round, const core::Document& document,
									  const core::DrawCounts& counts);

	// 報告 1 つの上限（バイト）。超える分は**診断ログの古いほうから**削除する（末尾＝結果に
	// 近いほうを残す）。MCP の応答 1 つに載せても Claude のコンテキストを消費し尽くさない大きさ。
	inline constexpr std::size_t kMaxTestReportBytes = 60000;
} // namespace HomeskzIfcImport::parse
