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
#include "Extensions/ExtShearWall.h"
#include "Extensions/ExtTestMenu.h"
#include "Extensions/ExtMcpMenu.h"
#include "Extensions/ExtMcpPalette.h"
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

#ifdef VW_DEV_BUILD
	// M25 「実機テストを実行」コマンド。**開発版だけ**——実機テスト（記憶した条件で図面を
	// 戻して取り込み直す）はこのコマンドが丸ごと持ち、本番の取り込みコマンドはそれを知らない
	// （Extensions/ExtTestMenu.h）。安定版はこのクラスを持つがどこにも登録しない。
	REGISTER_Extension<HomeskzIfcImport::CExtMenuTest>(GROUPID_ExtensionMenu, action, moduleInfo,
													   iid, inOutInterface, cbp, reply);

	// 「MCP ブリッジを表示」コマンドと、橋を常駐させるモードレスなパレット（M30）。
	// **開発版だけ**（M38）——橋はローカルの Claude Code から実機テストを回す開発の道具で、
	// 取り込み・更新・再起動まで起こせる（Extensions/ExtMcpPalette.h）。安定版はクラスを
	// 持つがどこにも登録しない。
	REGISTER_Extension<HomeskzIfcImport::CExtMenuMcpBridge>(
		GROUPID_ExtensionMenu, action, moduleInfo, iid, inOutInterface, cbp, reply);
	REGISTER_Extension<HomeskzIfcImport::CExtMcpPalette>(
		VectorWorks::Extension::GROUPID_ExtensionWebPalettes, action, moduleInfo, iid,
		inOutInterface, cbp, reply);

	// 橋の受け付けを刻む殻の時計（M41。OS のタイマー）。パレットを開かなくても、図面が
	// 1 枚も開いていなくても受け付ける（Extensions/ExtMcpPalette.h「なぜ OS のタイマーなのか」）。
	// この関数は何度も呼ばれるが、時計は 1 度しか仕掛けない。**更新の確認はしない**——
	// 刻みが更新を起こすのは Claude が vw_update を頼んだときだけ。
	HomeskzIfcImport::StartMcpBridgeClock();
#endif

	return reply;
}
