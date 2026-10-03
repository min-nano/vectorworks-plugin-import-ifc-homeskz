//
//	Extensions/ExtTestMenu.h
//
//	メニューコマンド「実機テストを実行… (みんなの構造設計支援Dev)」。**開発版だけ**
//	（M25。docs/DEV-NOTES.md）。名前にプラグイン名を入れるのは「アップデータを確認」と
//	同じ作法——ワークスペースの編集画面ではカテゴリが見えないので、コマンド名だけで
//	どのプラグインのものか分かるようにしておく。
//
//	【なぜ本番の取り込みと分けたか】M24 までは、実機フィードバックの往復が**本番の取り込み
//	コマンドの中**に織り込まれていた——記憶を読んでファイル選択を飛ばす分岐、「次は更新を
//	尋ねずに入れてよいか」を意味する戻り値、投稿できたら結果ダイアログを出さない分岐。
//	`#ifdef VW_DEV_BUILD` の外にあるので**安定版にも同じ制御フローが入っており**、開発の
//	都合で本番の経路にバグを混ぜる余地になっていた。
//
//	そこで**テストの入口を別のコマンドとして立てた**。実機テスト（記憶・図面の戻し・結果の控え）は
//	すべてこちらが持ち、本番の取り込みコマンドは往復を 1 つも知らない。**両者が共有するのは
//	絵を作るところだけ**（本体の draw/ImportRun.h）なので、テストで走るのは本番と同じ
//	コードである。
//
//	【押すと何が起きるか】本体の draw::runTestRound（src/draw/Feedback.h）:
//	  * 1 周目 … IFC・取り込み設定・軸組図の通りを尋ねてから取り込む
//	  * 続きの周 … 「前回と同じ条件で」か「選び直す」かを 1 度だけ尋ね、図面を取り込み前へ
//	    戻してから取り込む
//	結果（内訳・前の周との差分・図面の状態・診断ログ）は**手元のファイルに残し**、MCP の
//	`vw_test_report` で Claude が読む（M38。PR へは投稿しない）。同じ周は Claude が MCP の
//	`vw_run_test` からも起こせる（そちらはダイアログを 1 枚も出さない）。
//
//	【更新の確認】頭で `Silent`（あるときだけ尋ねる）。以前あった `Auto`（尋ねずに入れる）は
//	M38 で往復のパレットと一緒に外した——尋ねずに入れたいときは Claude が MCP の
//	`vw_update` を呼ぶ。
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
	// メニュー項目を実行したときの本体。
	class CTestMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CTestMenu_EventSink(IVWUnknown* parent);
		~CTestMenu_EventSink() override;

		// 更新を確かめてから、実機テストを 1 周走らせる。
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
