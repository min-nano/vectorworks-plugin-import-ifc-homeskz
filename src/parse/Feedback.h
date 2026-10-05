//
//	parse/Feedback.h
//
//	**実機テストの報告**（docs/DEV-NOTES.md M23 / M38）。開発版ビルドの「実機テストを実行…」
//	（または MCP の `vw_run_test`）が取り込みを走らせた結果を、Claude が読む Markdown へ
//	組み立てる。報告は手元のファイルに残り、ローカルの Claude Code が MCP の
//	`vw_test_report` で読む（M37 までは PR へコメントとして投稿していた）。
//
//	【なぜ要るか】`draw/` の実描画は CI では検証できず、ローカルの VectorWorks でしか
//	確かめられない（CLAUDE.md「テスト方針」）。その確認結果を人が手で写して伝えている限り、
//	1 往復ごとに「ファイルを選び直す・ログを貼る」という手間が乗る。**プラグイン自身に
//	報告させれば、人がするのは「絵を見て一言書く」だけになる。**
//
//	【何を載せるか】
//	  * 結末・所要時間・**要素ごとの内訳**（parse/Summary の elementRows。表は 1 つきり）
//	  * **前の周からの差分**——読む側が知りたいのは絶対値ではなく「直した結果どう動いたか」
//	  * 図面が取り込み前へ戻してあったか（絵の破綻を実装のせいにしないための 1 行）
//	  * 描画側が持ち帰った注意（`DrawCounts::diagnostics`）と記録（`notes`）
//	  * 診断ログ（上限を超える分は古いほうから削る）
//
//	【所見はここに載らない】**実機を見た人の所見**は人が Claude とのチャットへ直接書く
//	（docs/DEV-NOTES.md M23「所見はプラグインの仕事ではなかった」）。
//
//	【伏せない】M37 までは PR コメントが公開されるのでファイル名・ユーザー名・図面枠の
//	スタイル名を伏せていた。報告は利用者の計算機の中だけで読まれるので、伏せる理由が無い。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を include しない。ここは Document と
//	DrawCounts を読むだけの純粋な文字列組み立てなので、無 SDK で単体テストできる。
//

#pragma once

#include "core/Document.h"
#include "parse/Summary.h"

#include <cstddef>
#include <string>
#include <vector>

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

		int round = 1;				// 何周目か（1 から）
		std::string previousCommit; // 前の周のビルド（空なら 1 周目）
		std::string previousTally;	// 前の周の内訳（formatTally の 1 行表現）

		// **1 周目に採った「取り込み前に在ったレイヤ」の顔ぶれ**（core::FeedbackSession）。
		// 今回の DrawCounts::existingLayers と引き比べて、図面が取り込み前へ戻して
		// あるかを言う（restoredStateLine）。baselineKnown が false なら基準が無い
		// （1 周目、または古い版が書いた記憶）ので、判定せずその旨を書く。
		bool baselineKnown = false;
		std::vector<std::string> baselineLayers;

		// **取り込みの前に図面へ何をしたか**（draw/Feedback の prepareDrawingForRound が
		// 返す 1 行。空なら出さない）。診断ログにも同じ行が入るが、**ログは上限で切り詰め
		// られるので、そこだけを頼りにしない**——前の周の取り除きが効いたかは「図面の
		// 状態」と並べて読みたい一等地の情報である（実機 round 2 で、この行がログの省略部分
		// へ落ちて読めなかった）。
		std::string preparation;

		// **次の周が図面を丸ごと戻せるか。** 作業ファイルを用意できた周（draw/Feedback の
		// RoundDocument::Ready）なら、次の周は「取り消し」か作業ファイルの開き直しで
		// 取り込み前へ戻るので、テンプレートのレイヤへ描いた分も残らない。false のとき
		// だけ「取り除けません」の注記を出す（true の周に出すと、すぐ上の「取り消しで
		// 戻っています」と食い違う。PR #188 の実機確認）。
		bool restorable = false;

		bool anonymize = true; // 案件が分かるものを伏せるか
	};

	// -----------------------------------------------------------------------
	// **実機テストの周の結末**（M25）。結果ダイアログ（メニューから押した周）と MCP の応答
	// （`vw_run_test`）に、**このコマンド自身の言葉で**短く伝える。
	//
	// **取り込みコマンドの完了文言（`formatImportResult` / `formatImportError`）を借りない。**
	// 借りると、押した人には本番の取り込みが同じことをしているように見える——コマンドを
	// 分けた意味が見た目の上で崩れる（実機の指摘。docs/DEV-NOTES.md M25）。
	enum class TestRoundOutcome
	{
		// 取り込みを終えた（内訳は報告にある）。detail は報告の在り処など。
		Completed,
		// **描く図面を用意できなかった**ので、取り込みを始めなかった。実機 round 9 で、
		// 前の周の図面を閉じたあと作業ファイルを開き直せず、**どこにも属さない状態で
		// 描いて全 18 要素が 0 件**になった——数字だけ見れば「全部描けなかった」だが、
		// 実際には描く先が無かっただけである。**そうなる前に止める。**
		DocumentFailed,
		ImportFailed, // 取り込みがエラーで中断した
		// **記憶が無いのにダイアログを出せない**（MCP の `vw_run_test` で 1 周目を頼まれた）。
		// IFC と設定は人がメニューから選ぶしかない。
		NotRemembered,
	};

	// detail は理由（空でもよい）。返るのは**短い本文**で、内訳と診断ログは報告が持つ。
	std::string formatTestRoundResult(TestRoundOutcome outcome, const std::string& detail);

	// **内訳の 1 行表現**（`ストーリ:3/3,通り芯:44/44,…`）。命令が 0 の要素は載せない
	// （無い物の 0 を並べても差分の役に立たない）。次の周まで持ち越して差分を取るための
	// 形なので、**人向けの整形は一切しない**。
	std::string formatTally(const std::vector<ElementRow>& rows);

	// 2 つの内訳を突き合わせ、**変わった行だけ**を人が読める形で返す（変化が無ければ空）。
	// 片方にしか無い要素も「増えた／消えた」として出す——要素が丸ごと出なくなるのは
	// たいてい退行なので、黙って落とすと最悪の変化を見落とす。
	std::string formatTallyDiff(const std::string& previous, const std::string& current);

	// **長い本文の末尾だけを残す**（診断ログの切り詰め。報告と MCP の `vw_log` が使う）。
	// maxBytes を超えるときは**古いほう（先頭）を削り**、「前半 N バイトを省略」の 1 行を
	// 頭に付ける。切り口は **UTF-8 の文字境界の次の行頭**——3 バイトの日本語の途中で切ると
	// 壊れた UTF-8 になり、JSON の読み手に弾かれる（docs/DEV-NOTES.md M25）。
	std::string keepTail(const std::string& text, std::size_t maxBytes);

	// **実機テストの報告**（Markdown）。先頭の見出しに何周目か・どのビルドかを置く。
	std::string formatTestRoundReport(const FeedbackRound& round, const core::Document& document,
									  const core::DrawCounts& counts);

	// 報告 1 つの上限（バイト）。超える分は**診断ログの古いほうから**削る（末尾＝結果に
	// 近いほうを残す）。MCP の応答 1 つに載せても Claude の文脈を食い潰さない大きさ。
	inline constexpr std::size_t kMaxTestReportBytes = 60000;
} // namespace HomeskzIfcImport::parse
