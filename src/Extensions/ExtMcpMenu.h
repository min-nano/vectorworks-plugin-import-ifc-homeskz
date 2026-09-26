//
//	Extensions/ExtMcpMenu.h
//
//	メニューコマンド「MCP ブリッジを表示… (みんなの構造設計支援)」（開発版は「…Dev)」）。
//	押すと**Claude と図面をつなぐ橋のパレット**（Extensions/ExtMcpPalette.h）を出す。
//
//	【何のためか】プラグインの開発では「いま図面がどうなっているか」を知るのに、毎回
//	Vectorworks の画面を人が読んで伝えるしかなかった。橋が架かっていると、Claude が
//	自分でレイヤ・クラス・オブジェクトを数えて確かめられる——**実機でしか分からないこと**を
//	往復の少ない形で調べられる（CLAUDE.md「実機確認の作法」を補う道具）。
//
//	【何が起きるか】パレットが出て、そのページの JS タイマーが数百 ms ごとに本体の
//	draw::serveMcpBridge（src/draw/McpBridge.h）を呼ぶ。**パレットは閉じても（隠れても）
//	受け付けを続け、Vectorworks が終わるまで止まらない**（M30。ExtMcpPalette.h）。
//	M24 まではこのコマンドの実行そのものが橋の寿命で、架けている間は進捗ダイアログが
//	図面を塞いでいた。**ユニバーサル名と UUID は据え置いた**——ワークスペースはそれで
//	コマンドを覚えている（CLAUDE.md「ビルド・リント・リリース」）。
//
//	【登録は殻に、処理は本体に】CLAUDE.md「殻と本体」の表どおり。ここにあるのは登録と
//	パレットを出す 1 行だけ。
//
//	【Needs = None】**文書が開いていなくても押せる。** 描画先が要るわけではなく、
//	「文書を開く前に橋を架けておく」使い方を塞ぐ理由が無い。文書を要る道具は、その道具が
//	「文書が開いていません」と答える。
//

#pragma once

#include "VectorworksSDK.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// メニュー項目を実行したときの本体。
	class CMcpBridgeMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CMcpBridgeMenu_EventSink(IVWUnknown* parent);
		~CMcpBridgeMenu_EventSink() override;

		// パレットを出す（受け付けはそのページの時計が始める）。
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
