//
//	Extensions/ExtRegressionMenu.h
//
//	メニューコマンド「回帰テストを実行… (みんなの構造設計支援Dev)」。**開発版だけ**
//	（M28。docs/DEV-NOTES.md）。名前にプラグイン名を入れるのは他のコマンドと同じ作法
//	——ワークスペースの編集画面ではカテゴリが見えないので、コマンド名だけでどのプラグイン
//	のものか分かるようにしておく。
//
//	【何をするか】**指定したフォルダにある過去の物件を 1 件ずつ取り込み、前回の結果
//	（基準）と引き比べる。** 中身は本体の draw::runRegressionCommand（src/draw/Regression.h）
//	で、**ここに実処理は 1 行も無い**（CLAUDE.md「殻と本体」）。
//
//	【実機テストとの違い】「実機テストを実行…」（Extensions/ExtTestMenu.h）は**1 つの物件を、
//	ビルドが変わるたびに**取り込み直して PR へ投稿する——縦（時間）に伸びる往復である。
//	こちらは**多数の物件を、1 つのビルドで**通して基準と引き比べる——横に広がる。PR へは
//	何も投稿せず、往復も回さない。
//
//	【ここでは更新を確認しない】他の入口（取り込み・実機テスト・専用のコマンド）は頭で
//	更新を確認するが、**回帰テストは確認しない**。測りたいのは**いま入っているビルド**で
//	あって、走らせる直前に別の版へ入れ替わってしまっては「どの版の数字か」が変わる
//	——古い版と引き比べたくて意図的に入れていることもある。新しい版で測りたければ、
//	先に「アップデータを確認」を押せばよい（src/Updater.h「いつ確認するか」）。
//
//	【走っている間は往復の駆動を止める】取り込みの最中は進捗ダイアログの DoYield で
//	パレットの JS タイマーが動きうる。素通しすると駆動が周を始めようとして本体を降ろしに
//	いくので、実機テストと同じ番人（`FeedbackLoopBusyScope`）を掛ける
//	（src/FeedbackLoop.h）。**回帰テストは何十分も走る**ので、ここが特に効く。
//
//	【Needs = DocIsActive】取り込みと同じ。描画先の文書が要る——しかも**その文書を作業用に
//	複製してから**1 件ずつ取り込む（src/draw/Regression.h）。
//

#pragma once

#include "VectorworksSDK.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// メニュー項目を実行したときの本体。
	class CRegressionMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CRegressionMenu_EventSink(IVWUnknown* parent);
		~CRegressionMenu_EventSink() override;

		// 回帰テストを 1 回走らせる（本体の draw::runRegressionCommand へ取り次ぐ）。
		void DoInterface() override;
	};

	// ------------------------------------------------------------------------
	// 拡張そのもの（ModuleMain が REGISTER_Extension で登録する。**dev だけ**）。
	class CExtMenuRegression : public VWExtensionMenu
	{
		DEFINE_VWMenuExtension;

	public:
		CExtMenuRegression(CallBackPtr cbp);
		~CExtMenuRegression() override;
	};
} // namespace HomeskzIfcImport
