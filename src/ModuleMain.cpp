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

	// CallBackPtr を記録しておき、本体（ペイロード）を読み込むときに渡して初期化させる
	// （src/PayloadAbi.h / src/PayloadSession.h）。
	// **本体にも同じ CallBackPtr が要る。** gSDK / gCBP は SDK の静的ライブラリが持つ
	// モジュールごとのグローバルなので、殻が初期化しても本体の側は空のまま。**殻と本体を
	// 分けていることが、アップデートに Vectorworks の再起動を不要にしている要点である。**
	HomeskzIfcImport::RememberSdkCallbacks(cbp);

	// **ここでアップデートの確認はしない。** 確認の入口は
	//   * メニューコマンド「アップデータを確認」（Extensions/ExtUpdateMenu.h）
	//   * 取り込みコマンドの先頭（Extensions/ExtMenu.cpp）
	// の 2 つである（src/Updater.h「いつ確認するか」）。
	//
	// 以前は起動時（この関数の中）で自動的に実行していた。それをやめられたのは、プラグインが
	// **殻と本体**に分かれて、本体だけの更新なら再起動が要らなくなったため
	// （src/PayloadAbi.h）。起動のたびに確認する必要が無くなったうえ、起動を待たせず、
	// **確認したいときに実行できる**ほうが自然である。加えて、ここで実行していたために
	// 再起動を Vectorworks 自身に要求できなかった（読み込み中は終了できない）という制約も、
	// 同時に解消されている（src/Updater.cpp の Restart）。

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
	// 戻して取り込み直す）はこのコマンドがすべて担い、本番の取り込みコマンドはそれを知らない
	// （Extensions/ExtTestMenu.h）。安定版はこのクラスを持つがどこにも登録しない。
	REGISTER_Extension<HomeskzIfcImport::CExtMenuTest>(GROUPID_ExtensionMenu, action, moduleInfo,
													   iid, inOutInterface, cbp, reply);

	// 「MCP ブリッジを表示」コマンドと、ブリッジを常駐させるモードレスなパレット（M30）。
	// **開発版だけ**（M38）——ブリッジはローカルの Claude Code から実機テストを回す開発の道具で、
	// 取り込み・更新・再起動まで実行できる（Extensions/ExtMcpPalette.h）。安定版はクラスを
	// 持つがどこにも登録しない。
	REGISTER_Extension<HomeskzIfcImport::CExtMenuMcpBridge>(
		GROUPID_ExtensionMenu, action, moduleInfo, iid, inOutInterface, cbp, reply);
	REGISTER_Extension<HomeskzIfcImport::CExtMcpPalette>(
		VectorWorks::Extension::GROUPID_ExtensionWebPalettes, action, moduleInfo, iid,
		inOutInterface, cbp, reply);

	// MCP ブリッジの受け付けを周期的に呼ぶ殻の時計（M41。OS のタイマー）を開始する。
	// パレットを開かなくても、図面が 1 枚も開いていなくても受け付ける
	// （Extensions/ExtMcpPalette.h「なぜ OS のタイマーなのか」）。
	// この関数は何度も呼ばれるが、時計は 1 度しか登録しない。**更新の確認はしない**——
	// 時計の呼び出しが更新を実行するのは Claude が vw_update を要求したときだけ。
	HomeskzIfcImport::StartMcpBridgeClock();
#endif

	return reply;
}
