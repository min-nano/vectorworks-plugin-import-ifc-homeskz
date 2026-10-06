//
//	Extensions/ExtMcpMenu.h
//
//	メニューコマンド「MCP ブリッジを表示… (みんなの構造設計支援)」（開発版は「…Dev)」）。
//	実行すると**Claude と図面をつなぐブリッジのパレット**（Extensions/ExtMcpPalette.h）を
//	表示する。**開発版だけ**に登録する。
//
//	【何のためか】ブリッジが接続されていると、ローカルの Claude Code が自分でレイヤ・
//	クラス・オブジェクトを数え、診断ログと実機テストの報告を読み、新しいビルドを
//	インストールして再起動し、実機テストを実行できる（M38）——**実機でしか分からない
//	こと**を、PR のコメントを介さずに調べられる。それまでは「いま図面がどうなっているか」を
//	知るのに、毎回 Vectorworks の画面を人が読んで伝えるしかなかった。
//
//	【何が起きるか】パレットが表示され、そのページの JS タイマーが数百 ms ごとに本体の
//	draw::serveMcpBridge（src/draw/McpBridge.h）を呼ぶ。**パレットは閉じても（隠れても）
//	受け付けを続け、Vectorworks が終了するまで止まらない**（M30。ExtMcpPalette.h）。
//	**ユニバーサル名と UUID は変更していない**——ワークスペースはそれでコマンドを記録して
//	いる（CLAUDE.md「ビルド・リント・リリース」）。
//	M24 まではこのコマンドの実行期間がそのままブリッジの寿命で、接続している間は進捗
//	ダイアログが図面の操作を妨げていた。
//
//	【登録は殻に、処理は本体に】CLAUDE.md「殻と本体」の表どおり。ここにあるのは登録と
//	パレットを表示する 1 行だけ。
//
//	【Needs = None】**文書が開いていなくても実行できる。** 描画先が要るわけではなく、
//	「文書を開く前にブリッジを接続しておく」使い方を妨げる理由が無い。文書を要する
//	MCP の道具は、その道具が「文書が開いていません」と答える。
//

#pragma once

#include "VectorworksSDK.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// メニュー項目を実行したときの処理。
	class CMcpBridgeMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CMcpBridgeMenu_EventSink(IVWUnknown* parent);
		~CMcpBridgeMenu_EventSink() override;

		// パレットを表示する（受け付けはそのページの時計が始める）。
		void DoInterface() override;
	};

	// ------------------------------------------------------------------------
	// 拡張そのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtMenuMcpBridge : public VWExtensionMenu
	{
		DEFINE_VWMenuExtension;

	public:
		CExtMenuMcpBridge(CallBackPtr cbp);
		~CExtMenuMcpBridge() override;
	};
} // namespace HomeskzIfcImport
