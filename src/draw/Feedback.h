//
//	draw/Feedback.h
//
//	**実機テストの 1 周**（docs/DEV-NOTES.md M23〜M25・M38〜M40）。開発版（dev）ビルドで、記憶
//	した条件（IFC・取り込み設定）のまま、テンプレートから開いた新しい図面へ取り込み直し、その
//	結果を**手元のファイルに記録する**——この 1 周ぶんを受け持つ。
//
//	【呼び出し元】2 つの入口が同じ runTestRound を通る。
//	  * メニュー「実機テストを実行…」（src/Extensions/ExtTestMenu）… allowDialogs=true。
//	    1 周目は IFC・設定・軸組図の通りを尋ね、2 周目からは「前回と同じ条件で」か
//	    「選び直す」かを 1 度だけ尋ねる。
//	  * MCP の `vw_run_test`（draw/McpBridge.cpp）… allowDialogs=false。**1 枚もダイアログを
//	    出さない**（誰も操作していない Vectorworks を止めない）。IFC とテンプレートを指定
//	    されれば尋ねずに 1 周目から始め（M40）、指定も記憶も無ければ実行しない。
//
//	【取り込みのあとに人の操作を残さない】**決めることは全部、取り込みが始まる前に決める。**
//	うまく行った周は結果ダイアログも出さない。
//	取り込みは 1 分以上かかるので、終わったところに確認が待っていると席を離れられない。
//	また、モーダルのダイアログが開いている間は本体がスタックに載ったままなので、**MCP
//	ブリッジが受け付けを見送り、Claude が報告を読めない**（src/Extensions/ExtMcpPalette.h
//	「取り込みの最中は見送る」）。
//
//	【所見は報告に載せない】描画結果を見て気付いたことは**人が Claude とのチャットへ直接
//	書く**（M23 で一度プラグインに尋ねさせて削除した。docs/DEV-NOTES.md「所見はプラグインの
//	仕事ではなかった」）。
//
//	【毎周テンプレートから描画する（M39）】1 周目に開いていた図面を一時ファイルのテンプレート
//	（`.sta`）として保存し、どの周もそれを開いた新しい図面へ描画する。描画結果は周の終わりに
//	一時ファイルへ保存し、**次の周の頭で保存せずに閉じる**——再起動のときに保存の確認が
//	出ないように（docs/DEVELOPMENT.md「図面の用意（M39）」）。**閉じてよいのは自分で保存した
//	図面だけ**で、その判定は core::isOwnedTestDocument が担う（CLAUDE.md「開発の基本方針」8）。
//	M38 までの「取り消し」と「前の周が作ったレイヤを消す」による復元は廃止した。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）と VWFC のダイアログを include する。
//	このヘッダは標準ライブラリまでしか参照しない。
//
//	【本番のコマンドは実機テストを知らない（M25）】実機テストは**dev だけの実機テストの
//	コマンド**に閉じ、本番の取り込みと共有するのは**描画するところだけ**（draw/ImportRun.h の
//	`runImportRound`）。テストで実行されるのは本番と同じコードである。
//	M24 までは、往復が**本番の取り込みコマンドの中**に組み込まれていた。`#ifdef VW_DEV_BUILD`
//	の外にあるので**安定版にも同じ制御フローが入っており**、本番の経路にバグを混ぜる余地に
//	なっていた。M25 で丸ごと移した。
//
//	【結果は PR へ投稿しない（M38）】ここは**報告をファイルへ書いて終わる**だけ
//	（core::testReportPathFor）。ローカルの Claude Code が MCP ブリッジ越しに更新・再起動・
//	取り込み・報告の読み出しを自分で起こす。M37 までは結果を PR へ自動で投稿し、殻の
//	パレットが新しい dev ビルドを見つけ次第インストールして取り込み直していた（往復）。
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
		bool ran = false;		// 取り込みを最後まで実行したか
		int round = 0;			// 実行した周（ran のときだけ意味がある）
		std::string message;	// 人に見せる短い結末（parse::formatTestRoundResult）
		std::string report;		// 報告の本文（ran のときだけ）
		std::string reportPath; // 報告を書いた場所（書けなければ空）
	};

	// **MCP の `vw_run_test` が指定する条件**（M40）。どちらも空なら従来どおり（記憶した
	// 条件で続きの周を実行し、記憶が無ければ実行しない）。メニューの周では使わない。
	struct TestRoundRequest
	{
		// 取り込む IFC の絶対パス。**指定されたら、記憶があっても新しい 1 周目**として、
		// 尋ねずに実行する（設定はテンプレートから開いた図面にあるもので組む。
		// draw::presetImportSettings）。
		std::string ifcPath;
		// テンプレート（`.sta`）の絶対パス。一時ファイルの置き場へ**複製してから**記憶する
		// （リポジトリの tests/fixtures/Default.sta を渡されても、ワークツリーが消えたあとの
		// 周が開くものを失わないように）。空なら記憶したテンプレートを使う。
		std::string templatePath;
	};

	// **実機テストの 1 周**（M25）。**このコマンドだけが実機テストを知っている。**
	//
	//   1. 記憶が無ければ 1 周目として、IFC・取り込み設定・軸組図の通りを尋ねてから取り込む
	//      （allowDialogs のときだけ。MCP からは NotRemembered を返して何もしない）。
	//   2. 記憶があれば、前の周と同じ条件で取り込む。メニューから押したときだけ
	//      「前回と同じ条件で」か「選び直す」かを 1 度尋ねる。
	//   3. MCP の周で IFC を指定されたら（request.ifcPath）、尋ねずに新しい 1 周目を
	//      実行する（M40。テンプレートは request.templatePath か、記憶したもの）。
	//
	// どれも、テンプレートから開いた新しい図面へ描画し、終わったら報告を書いて記憶を進める。
	// 失敗したとき、allowDialogs なら結果ダイアログで伝える（MCP には message で返す）。
	TestRoundResult runTestRound(bool allowDialogs, const TestRoundRequest& request = {});

	// 実機テストを終えた結末（endTestSession）。
	struct TestCleanupResult
	{
		bool done = false; // 図面を閉じ、一時ファイル・記憶・報告をすべて片付けたか
		std::string message; // 何をしたか（人と Claude に見せる）
	};

	// **実機テストを終える**（M42。MCP の `vw_test_cleanup`——Python サーバが占有を解く
	// ときに呼ぶ）。次の周まで状態を持ち越さないときの片付けで、次の順に行う:
	//   1. 自分で保存した図面のうち開いているものを保存せずに閉じる（CloseOwnedDocuments。
	//      閉じてよい相手の判定は周の頭と同じ core::isOwnedTestDocument）。
	//   2. いま動いているビルドのブランチと、記憶が指すファイルを含む一時フォルダを消す
	//      （core::sessionScratchDirs → removeScratchDir の安全弁）。
	//   3. 記憶と報告を消す（次の周は MCP なら ifc と template を渡して 1 周目から）。
	// **閉じ残した図面・消せなかったフォルダがあれば、記憶は残す**（次の周の頭か次の片付けで
	// もう一度試せるように）。ダイアログは出さない。
	TestCleanupResult endTestSession();

	// 直近の報告の在り処（core::testReportPathFor。記憶の置き場所が分からなければ空）。
	std::string testReportPath();
} // namespace HomeskzIfcImport::draw
