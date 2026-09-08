//
//	ModuleMain.cpp
//
//	Main entry point for the Vectorworks plug-in module. Vectorworks loads the
//	built .vwlibrary and calls plugin_module_main to register the extensions it
//	provides.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtColumnMark.h"
#include "Extensions/ExtFeedbackPalette.h"
#include "Extensions/ExtShearWall.h"
#include "Extensions/ExtMcpMenu.h"
#include "Extensions/ExtMenu.h"
#include "Extensions/ExtUpdateMenu.h"
#include "PayloadSession.h"

// Identifier used by Vectorworks to locate this plug-in's resources (.vwr) at
// run time. Must match the base name of the packaged .vwr ("min-nano_structure.vwr"
// for the stable build, "min-nano_structureDev.vwr" for the dev build). See
// BuildConfig.h.
const char* DefaultPluginVWRIdentifier()
{
	return PLUGIN_VWR_ID;
}

//------------------------------------------------------------------
// Report the SDK version this plug-in was compiled against so Vectorworks can
// decide whether it is safe to load.
extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_ver()
{
	return SDK_VERSION;
}

//------------------------------------------------------------------
// Module entry point.
// More info: https://github.com/Vectorworks/developer-sdk/blob/main/Info/Plug-in%20Module.md
// (The old developer.vectorworks.net wiki has been retired; the SDK docs now
// live in the Vectorworks/developer-sdk repository — see README "SDK ドキュメント".)
//
extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_main(Sint32 action, void* moduleInfo,
													   const VWIID& iid,
													   IVWUnknown*& inOutInterface, CallBackPtr cbp)
{
	// Initialize the VCOM (Vectorworks Component Object Model) mechanism.
	::GS_InitializeVCOM(cbp);

	// **本体（ペイロード）にも同じ材料が要る。** gSDK / gCBP は SDK の静的ライブラリが持つ
	// モジュールごとのグローバルなので、殻が初期化しても本体の側は空のまま。ここで
	// 預けておいた CallBackPtr を、本体を読み込むときに渡して初期化させる
	// （src/PayloadAbi.h / src/PayloadSession.h）。**この 2 つに割ってあることが、
	// アップデートに Vectorworks の再起動を要らなくしている全部である。**
	HomeskzIfcImport::RememberSdkCallbacks(cbp);

	// **ここでアップデートの確認はしない。** 以前は起動時（この関数の中）で自動的に
	// 走らせていたが、いまは
	//   * メニューコマンド「アップデータを確認」（Extensions/ExtUpdateMenu.h）
	//   * 取り込みコマンドの頭（Extensions/ExtMenu.cpp）
	// の 2 つが入口である（src/Updater.h「いつ確認するか」）。
	//
	// やめられたのは、プラグインが**殻と本体**に割れて、本体だけの更新なら再起動が
	// 要らなくなったため（src/PayloadAbi.h）。起動のたびに問う必要が無くなったうえ、
	// 起動を待たせず、**確認したいときに押せる**ほうが素直である。加えて、ここで
	// 走らせていたせいで再起動を Vectorworks 自身に頼めなかった（読み込み中は
	// 終了できない）という制約も、同時に外れている（src/Updater.cpp の Restart）。

	Sint32 reply = 0L;

	using namespace VWFC::PluginSupport;

	// Register the IFC import menu command extension.
	REGISTER_Extension<HomeskzIfcImport::CExtMenuImportIfc>(
		GROUPID_ExtensionMenu, action, moduleInfo, iid, inOutInterface, cbp, reply);

	// M12 柱・小屋束の記号 PIO。メニューコマンドと同じモジュールに同梱する
	// （別プラグインにしない。Extensions/ExtColumnMark.h 冒頭）。
	REGISTER_Extension<HomeskzIfcImport::CExtColumnMark>(
		GROUPID_ExtensionParametric, action, moduleInfo, iid, inOutInterface, cbp, reply);

	// M19 耐力壁（筋かい・面材）の PIO。柱記号と同じく同じモジュールへ同梱する
	// （Extensions/ExtShearWall.h 冒頭）。
	REGISTER_Extension<HomeskzIfcImport::CExtShearWall>(
		GROUPID_ExtensionParametric, action, moduleInfo, iid, inOutInterface, cbp, reply);

	// 「アップデータを確認」コマンド。起動時の自動確認をやめた代わりの入口
	// （Extensions/ExtUpdateMenu.h）。
	REGISTER_Extension<HomeskzIfcImport::CExtMenuCheckUpdate>(
		GROUPID_ExtensionMenu, action, moduleInfo, iid, inOutInterface, cbp, reply);

	// 「MCP ブリッジを開始」コマンド。Claude から図面を読める橋を架ける
	// （Extensions/ExtMcpMenu.h）。登録だけがここにあり、実処理は本体側。
	REGISTER_Extension<HomeskzIfcImport::CExtMenuMcpBridge>(
		GROUPID_ExtensionMenu, action, moduleInfo, iid, inOutInterface, cbp, reply);

#ifdef VW_DEV_BUILD
	// M24 実機フィードバックの往復を回すモードレスなパレット。**開発版だけ**——往復するのは
	// PR のビルドであって main の配布物ではない（Extensions/ExtFeedbackPalette.h）。
	// 登録の枠組みはメニュー・PIO と同じ（グループ ID が違うだけ。SDK リファレンス
	// Findings「モードレス（非モーダル）なパレット」）。
	REGISTER_Extension<HomeskzIfcImport::CExtFeedbackPalette>(
		VectorWorks::Extension::GROUPID_ExtensionWebPalettes, action, moduleInfo, iid,
		inOutInterface, cbp, reply);
#endif

	return reply;
}
