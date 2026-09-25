//
//	Extensions/ExtMcpMenu.cpp
//
//	「MCP ブリッジを表示」コマンドの登録と取り次ぎ。受け付けはパレット
//	（Extensions/ExtMcpPalette.h）の時計と本体の draw::serveMcpBridge（src/draw/McpBridge.h）が
//	持つので、ここはパレットを出すだけ。**ここに実処理は 1 行も置かない**（CLAUDE.md「殻と本体」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMcpMenu.h"
#include "Extensions/ExtMcpPalette.h"

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// メニュー項目の宣言。カテゴリは他のコマンドと**同じ "category"**（＝プラグイン名）
		// を引く——このプラグインのコマンドはワークスペースの中で 1 か所にまとまっている
		// のが筋である（CLAUDE.md「殻と本体」）。
		//
		// Needs = None: 文書が開いていなくても押せる（ExtMcpMenu.h「Needs = None」）。
		//
		// 関数ローカル static で持つ理由は他のコマンドと同じ（EMenuEnableFlags は SDK の
		// 別 TU にある非ローカル static なので、名前空間スコープ変数の初期化子で参照すると
		// 静的初期化順序に依存する。Extensions/ExtMenu.cpp）。
		const SMenuDef& menuDef()
		{
			static const SMenuDef def = {/*Needs*/ EMenuEnableFlags::None,
										 /*NeedsNot*/ EMenuEnableFlags::None,
										 /*Title*/ {PLUGIN_VWR_ID, "mcpTitle"},
										 /*Category*/ {PLUGIN_VWR_ID, "category"},
										 /*HelpText*/ {PLUGIN_VWR_ID, "mcpHelp"},
										 /*VersionCreated*/ 31,
										 /*VersionModified*/ 0,
										 /*VersionRetired*/ 0,
										 /*OverrideHelpID*/ ""};
			return def;
		}
	} // namespace
} // namespace HomeskzIfcImport

// 安定版と開発版は同時に読み込まれうるので、UUID とユニバーサル名は別にする。
//
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: 1214425c-4e50-41a6-a308-887bf928fe81  (dev build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuMcpBridge,
	/*Event sink*/ CMcpBridgeMenu_EventSink,
	/*Universal name*/ PLUGIN_MCP_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x1214425c, 0x4e50, 0x41a6, 0xa3, 0x08, 0x88, 0x7b, 0xf9, 0x28, 0xfe, 0x81);
#else
// UUID: 193186a0-c0be-4501-a1ca-3c42530cf667  (stable build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuMcpBridge,
	/*Event sink*/ CMcpBridgeMenu_EventSink,
	/*Universal name*/ PLUGIN_MCP_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x193186a0, 0xc0be, 0x4501, 0xa1, 0xca, 0x3c, 0x42, 0x53, 0x0c, 0xf6, 0x67);
#endif
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuMcpBridge::CExtMenuMcpBridge(CallBackPtr cbp) : VWExtensionMenu(cbp, menuDef()) {}

CExtMenuMcpBridge::~CExtMenuMcpBridge() = default;

// ---------------------------------------------------------------------------
CMcpBridgeMenu_EventSink::CMcpBridgeMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent) {}

CMcpBridgeMenu_EventSink::~CMcpBridgeMenu_EventSink() = default;

// ---------------------------------------------------------------------------
void CMcpBridgeMenu_EventSink::DoInterface()
{
	// **パレットを出すだけ。** 出たページの時計が受け付けを始める（ExtMcpPalette.h）。
	// M24 まではここで進捗ダイアログを開いてループしていたので、架けている間は図面を
	// 触れなかった（docs/DEV-NOTES.md M29）。
	//
	// **本体はここでは読み込まない。** 時計の最初の 1 刻みが PayloadUse で読み込む——
	// ここで読んでも、戻った時点で使う区間が閉じるだけで、得るものが無い。
	ShowMcpPalette();
}
