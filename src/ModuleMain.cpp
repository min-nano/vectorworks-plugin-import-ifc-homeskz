//
//	ModuleMain.cpp
//
//	Main entry point for the Vectorworks plug-in module. Vectorworks loads the
//	built .vwlibrary and calls plugin_module_main to register the extensions it
//	provides.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenu.h"
#include "Updater.h"

// Identifier used by Vectorworks to locate this plug-in's resources (.vwr) at
// run time. Must match the base name of the packaged .vwr ("SamplePlugin.vwr"
// for the stable build, "SamplePluginDev.vwr" for the dev build). See
// BuildConfig.h.
const char* DefaultPluginVWRIdentifier() { return PLUGIN_VWR_ID; }

//------------------------------------------------------------------
// Report the SDK version this plug-in was compiled against so Vectorworks can
// decide whether it is safe to load.
extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_ver() { return SDK_VERSION; }

//------------------------------------------------------------------
// Module entry point.
// More info: https://github.com/Vectorworks/developer-sdk/blob/main/Info/Plug-in%20Module.md
// (The old developer.vectorworks.net wiki has been retired; the SDK docs now
// live in the Vectorworks/developer-sdk repository — see README "SDK ドキュメント".)
//
extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_main(
	Sint32			action,
	void*			moduleInfo,
	const VWIID&	iid,
	IVWUnknown*&	inOutInterface,
	CallBackPtr		cbp )
{
	// Initialize the VCOM (Vectorworks Component Object Model) mechanism.
	::GS_InitializeVCOM( cbp );

	// At Vectorworks start-up, offer to change the build in use. This runs when
	// Vectorworks loads the module (which it does at start-up to build the
	// workspace) and each check is guarded so it fires only once per session. The
	// network request is time-bounded (see vw-update.sh) so it can't hang start-up.
	// Start-up is the right place because a compiled plug-in can only be swapped in
	// at load time, and because a plug-in may re-invoke its own command
	// programmatically — so the check must not live on the command path.
#ifndef VW_DEV_BUILD
	// Stable plug-in: check for a newer stable build and, if one exists, ask (with
	// a native Vectorworks dialog) whether to install it. Silent when already
	// current or offline.
	SamplePlugin::RunStableStartupCheck();
#else
	// Dev plug-in: let the user pick which branch's build to use — keep the
	// installed one, or switch to another branch's prerelease (installed on
	// choosing, then restart to load). Silent on a network error.
	SamplePlugin::RunDevStartupCheck();
#endif

	Sint32	reply	= 0L;

	using namespace VWFC::PluginSupport;

	// Register our single menu command extension.
	REGISTER_Extension<SamplePlugin::CExtMenuSample>(
		GROUPID_ExtensionMenu, action, moduleInfo, iid, inOutInterface, cbp, reply );

	return reply;
}
