//
//	Extensions/ExtTestMenu.h
//
//	メニューコマンド「実機テストを実行… (みんなの構造設計支援Dev)」。**開発版だけ**
//	（M25。docs/DEV-NOTES.md）。名前にプラグイン名を入れるのは「アップデータを確認」と
//	同じ作法——ワークスペースの編集画面ではカテゴリが見えないので、コマンド名だけで
//	どのプラグインのものか分かるようにしておく。
//
//	【押すと何が起きるか】本体の draw::runTestRound（src/draw/Feedback.h）:
//	  * 1 周目 … IFC・取り込み設定・軸組図の通りを尋ねてから取り込む
//	  * 続きの周 … 「前回と同じ条件で」か「選び直す」かを 1 度だけ尋ね、図面を取り込み前へ
//	    戻してから取り込む
//	結果（内訳・前の周との差分・図面の状態・診断ログ）は**手元のファイルに記録し**、MCP の
//	`vw_test_report` で Claude が読む（M38。PR へは投稿しない）。同じ周は Claude が MCP の
//	`vw_run_test` からも実行できる（そちらはダイアログを 1 枚も出さない）。
//
//	【更新の確認】先頭で `Silent`（あるときだけ尋ねる）。尋ねずにインストールしたいときは
//	Claude が MCP の `vw_update` を呼ぶ。以前あった `Auto`（尋ねずにインストールする）は
//	M38 で往復のパレットと一緒に削除した。
//
//	【なぜ本番の取り込みと分けたか】実機テスト（記憶・図面の戻し・結果の記録）はすべて
//	こちらが持ち、本番の取り込みコマンドは往復を 1 つも知らない。**両者が共有するのは
//	描画結果を作るところだけ**（本体の draw/ImportRun.h）なので、テストで実行されるのは
//	本番と同じコードである。
//	M24 までは、実機フィードバックの往復が**本番の取り込みコマンドの中**に組み込まれて
//	いた——記憶を読んでファイル選択を省略する分岐、「次は更新を尋ねずにインストールして
//	よいか」を意味する戻り値、投稿できたら結果ダイアログを出さない分岐。
//	`#ifdef VW_DEV_BUILD` の外にあるので**安定版にも同じ制御フローが入っており**、開発の
//	都合で本番の経路にバグを混入させる余地になっていた。そこで**テストの入口を別の
//	コマンドとして分けた**。
//
//	【登録は殻に、処理は本体に】CLAUDE.md「殻と本体」の表どおり。ここにあるのは登録と
//	取り次ぎだけで、実処理は 1 行も無い。
//
//	【Needs = DocIsActive】取り込みと同じ。描画先の文書が要る。
//

#pragma once

#include "VectorworksSDK.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// メニュー項目を実行したときの処理。
	class CTestMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CTestMenu_EventSink(IVWUnknown* parent);
		~CTestMenu_EventSink() override;

		// 更新を確かめてから、実機テストを 1 周実行する。
		void DoInterface() override;
	};

	// ------------------------------------------------------------------------
	// 拡張そのもの（ModuleMain が REGISTER_Extension で登録する。**dev だけ**）。
	class CExtMenuTest : public VWExtensionMenu
	{
		DEFINE_VWMenuExtension;

	public:
		CExtMenuTest(CallBackPtr cbp);
		~CExtMenuTest() override;
	};
} // namespace HomeskzIfcImport
