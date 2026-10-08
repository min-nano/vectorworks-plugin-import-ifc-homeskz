//
//	draw/Feedback.h
//
//	**実機テストの 1 周**（docs/DEV-NOTES.md M23〜M25・M38〜M40・M43）。開発版（dev）ビルドで、
//	頼まれた条件（IFC・テンプレート・取り込み設定）で、テンプレートから開いた新しい図面へ
//	取り込み、その結果を**手元のファイルに記録する**——この 1 周ぶんを受け持つ。
//
//	【条件は毎周もらう（M43）】周をまたいで持ち越すのは、自分で保存した図面の記録
//	（core/FeedbackSession）だけ。M42 までは 1 周目の条件を記憶して 2 周目以降を名指し無しで
//	走らせていたが、周を起こすローカルの Claude Code のセッションが条件を持っているので
//	要らなくなった。
//
//	【呼び出し元】MCP の `vw_run_test`（draw/McpBridge.cpp）だけ。**1 枚もダイアログを
//	出さない**（誰も操作していない Vectorworks を止めない）。IFC とテンプレートは毎周
//	渡してもらい、設定はテンプレートの図面にあるもので組んだ既定を settings で上書きする。
//	M42 まではメニュー「実機テストを実行…」も同じ周を通っていたが、人が手で確かめるなら
//	本番の取り込みで足りる（診断ログは `vw_log` で読める）ので、M43 で削除した。
//
//	【所見は報告に載せない】描画結果を見て気付いたことは**人が Claude とのチャットへ直接
//	書く**（M23 で一度プラグインに尋ねさせて削除した。docs/DEV-NOTES.md「所見はプラグインの
//	仕事ではなかった」）。
//
//	【毎周テンプレートから描画する（M39）】どの周もテンプレート（`.sta`）を開いた新しい図面へ
//	描画する。描画結果は周の終わりに一時ファイルへ保存し、**次の周の頭で保存せずに
//	閉じる**——再起動のときに保存の確認が出ないように（docs/DEVELOPMENT.md「図面の用意（M39）」）。**閉じてよいのは自分で保存した
//	図面だけ**で、その判定は core::isOwnedTestDocument が担う（CLAUDE.md「開発の基本方針」8）。
//	M38 までの「取り消し」と「前の周が作ったレイヤを消す」による復元は廃止した。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは標準ライブラリと core/ までしか参照しない。
//
//	【本番のコマンドは実機テストを知らない（M25）】実機テストは**dev だけの MCP の
//	`vw_run_test`** に閉じ、本番の取り込みと共有するのは**描画するところだけ**（draw/ImportRun.h の
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

#include "core/Json.h"

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
		std::string message;	// 人に見せる短い結末（parse::formatTestRoundResult）
		std::string report;		// 報告の本文（ran のときだけ）
		std::string reportPath; // 報告を書いた場所（書けなければ空）
	};

	// **MCP の `vw_run_test` が渡す条件**（M40 / M43）。
	struct TestRoundRequest
	{
		// 取り込む IFC の絶対パス（必須）。
		std::string ifcPath;
		// テンプレート（`.sta`）の絶対パス（必須）。写さずにそのまま開く（`.sta` を開くと
		// 中身を複製した無題の新規文書が開き、ファイルそのものは変わらない）。
		std::string templatePath;
		// 取り込み設定の上書き（core::applyTestSettings。null なら上書きしない）。既定は
		// テンプレートから開いた図面にあるもので組む（draw::presetImportSettings）。
		core::Json settings;
	};

	// **実機テストの 1 周**（M25）。**ここだけが実機テストを知っている。**
	//
	// request の IFC・テンプレート・設定で、尋ねずに取り込む——前の周の図面を閉じ、
	// テンプレートから開いた新しい図面へ描画し、終わったら描画結果を保存して報告を書く。
	// IFC かテンプレートが無い・settings を読めないときは InvalidRequest を返して何もしない。
	// 失敗は message で返す。
	TestRoundResult runTestRound(const TestRoundRequest& request);

	// 実機テストを終えた結末（endTestSession）。
	struct TestCleanupResult
	{
		bool done = false; // 図面を閉じ、一時ファイル・記録・報告をすべて片付けたか
		std::string message; // 何をしたか（人と Claude に見せる）
	};

	// **実機テストを終える**（M42。MCP の `vw_test_cleanup`——Python サーバが占有を解く
	// ときに呼ぶ）。次の周まで状態を持ち越さないときの片付けで、次の順に行う:
	//   1. 自分で保存した図面のうち開いているものを保存せずに閉じる（CloseOwnedDocuments。
	//      閉じてよい相手の判定は周の頭と同じ core::isOwnedTestDocument）。
	//   2. いま動いているビルドのブランチと、記録が指すファイルを含む一時フォルダを消す
	//      （core::sessionScratchDirs → removeScratchDir の安全弁）。
	//   3. 記録と報告を消す。
	// **閉じ残した図面・消せなかったフォルダがあれば、記録は残す**（次の周の頭か次の片付けで
	// もう一度試せるように）。ダイアログは出さない。
	TestCleanupResult endTestSession();

	// 直近の報告の在り処（core::testReportPathFor。記録の置き場所が分からなければ空）。
	std::string testReportPath();
} // namespace HomeskzIfcImport::draw
