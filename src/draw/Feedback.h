//
//	draw/Feedback.h
//
//	**実機テストの 1 周**（docs/DEV-NOTES.md M23〜M25・M38）。開発版（dev）ビルドで、覚えた
//	条件（IFC・取り込み設定）のまま、テンプレートから開いた新しい図面へ取り込み直し、その結果を
//	**手元のファイルに控える**——この 1 周ぶんを受け持つ。
//
//	【誰が起こすか】2 つの入口が同じ runTestRound を通る。
//	  * メニュー「実機テストを実行…」（src/Extensions/ExtTestMenu）… allowDialogs=true。
//	    1 周目は IFC・設定・軸組図の通りを尋ね、2 周目からは「前回と同じ条件で」か
//	    「選び直す」かを 1 度だけ尋ねる。
//	  * MCP の `vw_run_test`（draw/McpBridge.cpp）… allowDialogs=false。**1 枚もダイアログを
//	    出さない**（誰も見ていない Vectorworks を止めない）。記憶が無ければ走らない。
//
//	【本番のコマンドは実機テストを知らない（M25）】M24 までは、往復が**本番の取り込み
//	コマンドの中**に織り込まれていた。`#ifdef VW_DEV_BUILD` の外にあるので**安定版にも同じ
//	制御フローが入っており**、本番の経路にバグを混ぜる余地になっていた。M25 で
//	**dev だけの実機テストのコマンド**へ丸ごと移し、両者が共有するのは**絵を作るところだけ**
//	（draw/ImportRun.h の `runImportRound`）にした。テストで走るのは本番と同じコードである。
//
//	【結果は PR へ投げない（M38）】M37 までは結果を PR へ自動で投稿し、殻のパレットが新しい
//	dev ビルドを見つけ次第入れて取り込み直していた（往復）。いまはローカルの Claude Code が
//	MCP ブリッジ越しに更新・再起動・取り込み・報告の読み出しを自分で起こすので、ここは
//	**報告をファイルへ書いて黙る**だけになった（core::testReportPathFor）。
//
//	【毎周テンプレートから描く（M39）】1 周目に開いていた図面を一時ファイルのテンプレート
//	（`.sta`）として保存し、どの周もそれを開いた新しい図面へ描く。M38 までの「取り消し」と
//	「前の周が作ったレイヤを消す」による戻しはやめた。描き上がりは周の終わりに一時ファイルへ
//	保存し、**次の周の頭で保存せずに閉じる**——再起動のときに保存の確認が出ないように
//	（docs/DEVELOPMENT.md「図面の用意（M39）」）。**閉じてよいのは自分で保存した図面だけ**で、
//	安全弁は core::isOwnedTestDocument が持つ（CLAUDE.md「開発の基本方針」8）。
//
//	【取り込みのあとに人の操作を残さない】**決めることは全部、取り込みが始まる前に決める。**
//	取り込みは 1 分以上かかるので、終わったところに確認が待っていると席を離れられない。
//	うまく行った周は結果ダイアログも出さない——モーダルのダイアログが開いている間は本体が
//	スタックに載ったままなので、**MCP ブリッジが受け付けを見送り、Claude が報告を読めない**
//	（src/Extensions/ExtMcpPalette.h「取り込みの最中は見送る」）。
//
//	【所見は報告に載せない】絵を見て気付いたことは**人が Claude とのチャットへ直接書く**
//	（M23 で一度プラグインに訊かせて外した。docs/DEV-NOTES.md「所見はプラグインの仕事では
//	なかった」）。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）と VWFC のダイアログを include する。
//	このヘッダは標準ライブラリまでしか参照しない。
//

#pragma once

#include <string>

namespace HomeskzIfcImport::draw
{
	// 実機テストが**そもそも使えるか**（dev ビルドか）。安定版では動かさない——開発の道具を
	// 利用者向けの配布物に持たせない。
	bool feedbackAvailable();

	// 1 周の結末。MCP の `vw_run_test` はこれをそのまま応答にする。
	struct TestRoundResult
	{
		bool ran = false;		// 取り込みを最後まで走らせたか
		int round = 0;			// 走らせた周（ran のときだけ意味がある）
		std::string message;	// 人に見せる短い結末（parse::formatTestRoundResult）
		std::string report;		// 報告の本文（ran のときだけ）
		std::string reportPath; // 報告を書いた場所（書けなければ空）
	};

	// **実機テストの 1 周**（M25）。**このコマンドだけが実機テストを知っている。**
	//
	//   1. 記憶が無ければ 1 周目として、IFC・取り込み設定・軸組図の通りを尋ねてから取り込む
	//      （allowDialogs のときだけ。MCP からは NotRemembered を返して何もしない）。
	//   2. 記憶があれば、前の周と同じ条件で取り込む。メニューから押したときだけ
	//      「前回と同じ条件で」か「選び直す」かを 1 度尋ねる。
	//
	// どちらも、取り込む前に図面を取り込み前へ戻し、終わったら報告を書いて記憶を進める。
	// 失敗したとき、allowDialogs なら結果ダイアログで伝える（MCP には message で返す）。
	TestRoundResult runTestRound(bool allowDialogs);

	// 直近の報告の在り処（core::testReportPathFor。記憶の置き場所が分からなければ空）。
	std::string testReportPath();
} // namespace HomeskzIfcImport::draw
