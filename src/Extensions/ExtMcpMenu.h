//
//	Extensions/ExtMcpMenu.h
//
//	メニューコマンド「MCP ブリッジを開始 (みんなの構造設計支援)」（開発版は「…Dev)」）。
//	押すと**Claude と図面をつなぐ橋を架け**、止めるまでその状態で待つ。
//
//	【何のためか】プラグインの開発では「いま図面がどうなっているか」を知るのに、毎回
//	Vectorworks の画面を人が読んで伝えるしかなかった。橋を架けておくと、Claude が
//	自分でレイヤ・クラス・オブジェクトを数えて確かめられる——**実機でしか分からないこと**を
//	往復の少ない形で調べられる（CLAUDE.md「実機確認の作法」を補う道具）。
//
//	【何が起きるか】本体（ペイロード）の draw::runMcpBridge（src/draw/McpBridge.h）。
//	スプール（一時ディレクトリの `min-nano_structure-mcp`）に置かれた要求を拾って応え、
//	［キャンセル］か道具 `vw_stop_bridge` で止まる。**止めるまで戻らない**——
//	Vectorworks のメインスレッド以外から SDK を呼べないので、メニューコマンドの実行
//	そのものを橋の寿命にしている（src/draw/McpBridge.h「なぜループなのか」）。
//
//	【登録は殻に、処理は本体に】CLAUDE.md「殻と本体」の表どおり。ここにあるのは登録と
//	取り次ぎだけで、実処理は 1 行も無い——だから道具を足しても利用者に再起動を強いない。
//
//	【Needs = None】**文書が開いていなくても押せる。** 取り込みコマンドと違って描画先が
//	要るわけではなく、「文書を開く前に橋を架けておく」使い方（開いた直後の状態を見る）を
//	塞ぐ理由が無い。文書を要る道具は、その道具が「文書が開いていません」と答える。
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

		// 橋を架け、止められるまで戻らない。
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
