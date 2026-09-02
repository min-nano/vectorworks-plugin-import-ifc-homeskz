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
#include "Extensions/ExtFoundation.h"
#include "Extensions/ExtMenu.h"
#include "Updater.h"

// Identifier used by Vectorworks to locate this plug-in's resources (.vwr) at
// run time. Must match the base name of the packaged .vwr ("HomeskzIfcImport.vwr"
// for the stable build, "HomeskzIfcImportDev.vwr" for the dev build). See
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

	// At Vectorworks start-up, offer to change the build in use. This runs when
	// Vectorworks loads the module (which it does at start-up to build the
	// workspace) and each check is guarded so it fires only once per session. The
	// network request is time-bounded (see vw-update.sh) so it can't hang start-up.
	// Start-up is the right place because a compiled plug-in can only be swapped in
	// at load time, and because a plug-in may re-invoke its own command
	// programmatically — so the check must not live on the command path.
	//
	// 例外はここから外へ出さない（CLAUDE.md「エラーハンドリング・所有権」）。
	// **起動時に呼ばれる SDK コールバックなので、ここで例外が漏れると
	// VectorWorks の起動そのものを巻き込んで落とす。** 自動アップデートは
	// あくまで付随機能であり、失敗してもプラグインの登録（この関数の本題）は
	// 続けなければならないため、黙って諦める（オフライン時に無言なのと同じ扱い）。
	//
	// NOLINTBEGIN(bugprone-empty-catch): 起動時は報告先が無い（ここでダイアログを出すと
	// 起動を妨げるだけ）。黙って諦めるのが**この場所では正しい**振る舞いなので、
	// 握り潰しを禁じる規則をここだけ外す。
	try
	{
#ifndef VW_DEV_BUILD
		// Stable plug-in: check for a newer stable build and, if one exists, ask (with
		// a native Vectorworks dialog) whether to install it. Silent when already
		// current or offline.
		HomeskzIfcImport::RunStableStartupCheck();
#else
		// Dev plug-in: let the user pick which branch's build to use — keep the
		// installed one, or switch to another branch's prerelease (installed on
		// choosing, then restart to load). Silent on a network error.
		HomeskzIfcImport::RunDevStartupCheck();
#endif
	}
	catch (...)
	{
	}
	// NOLINTEND(bugprone-empty-catch)

	Sint32 reply = 0L;

	using namespace VWFC::PluginSupport;

	// Register our single menu command extension.
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

	// M21 基礎の PIO（立上り・底盤・地中梁・床付けを 1 つの立体オブジェクトとして描き、
	// OIP で寸法を編集できる。Extensions/ExtFoundation.h 冒頭）。
	REGISTER_Extension<HomeskzIfcImport::CExtFoundation>(
		GROUPID_ExtensionParametric, action, moduleInfo, iid, inOutInterface, cbp, reply);

	return reply;
}
