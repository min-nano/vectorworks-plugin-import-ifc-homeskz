//
//	ExtMenu.cpp
//
//	Implementation of the plug-in's menu command.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenu.h"

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// Description of the menu command. The SResString entries ({resource,
		// identifier}) point at strings in the plug-in's .vwr resource file; a
		// resource file is optional for a build to succeed. EMenuEnableFlags{}
		// means "no special selection requirements to enable the command".
		// PLUGIN_VWR_ID differs between the stable and dev builds (see
		// BuildConfig.h) so each loads its own strings. File-local (anonymous
		// namespace) rather than `static`.
		SMenuDef gMenuDef = {
			/*Needs*/ EMenuEnableFlags{},
			/*NeedsNot*/ EMenuEnableFlags{},
			/*Title*/ {PLUGIN_VWR_ID, "title"},
			/*Category*/ {PLUGIN_VWR_ID, "category"},
			/*HelpText*/ {PLUGIN_VWR_ID, "help"},
			/*VersionCreated*/ 31,
			/*VersionModified*/ 0,
			/*VersionRetired*/ 0,
			/*OverrideHelpID*/ ""};
	} // namespace
} // namespace HomeskzIfcImport

// Every extension needs a globally unique ID and universal name. The stable and
// dev builds MUST use different ones so both plug-ins can be loaded at once.
//
// NOLINT: IMPLEMENT_VWMenuExtension is an SDK macro whose expansion trips
// misc-const-correctness (a `static VWIID iid` it could mark const); that is the
// macro's code, not ours, so silence the check across the two invocations.
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: 2368a4b2-0497-4bcc-89f6-fc436736de2b  (dev build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuImportIfc,
	/*Event sink*/ CImportIfcMenu_EventSink,
	/*Universal name*/ PLUGIN_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x2368a4b2, 0x0497, 0x4bcc, 0x89, 0xf6, 0xfc, 0x43, 0x67, 0x36, 0xde, 0x2b);
#else
// UUID: 137bde33-b2f1-4382-b3dc-1eef297f1b12  (stable build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuImportIfc,
	/*Event sink*/ CImportIfcMenu_EventSink,
	/*Universal name*/ PLUGIN_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x137bde33, 0xb2f1, 0x4382, 0xb3, 0xdc, 0x1e, 0xef, 0x29, 0x7f, 0x1b, 0x12);
#endif
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuImportIfc::CExtMenuImportIfc(CallBackPtr cbp) : VWExtensionMenu(cbp, gMenuDef) {}

CExtMenuImportIfc::~CExtMenuImportIfc() = default;

// ---------------------------------------------------------------------------
CImportIfcMenu_EventSink::CImportIfcMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent) {}

CImportIfcMenu_EventSink::~CImportIfcMenu_EventSink() = default;

void CImportIfcMenu_EventSink::DoInterface()
{
	// Note: the dev-build picker is NOT run here. It runs once at Vectorworks
	// start-up (see plugin_module_main -> RunDevStartupCheck) because a compiled
	// plug-in can only be swapped in at load time, and because the command may be
	// re-invoked programmatically — a picker on the command path would then pop up
	// repeatedly. So the command just does its work below, every time it runs.

	// This is the whole point of the plug-in for now: tell the user it ran,
	// and show exactly which build is loaded so a freshly-installed update can
	// be verified at a glance. VW_BUILD_BRANCH is the git branch the build came
	// from and VW_BUILD_VERSION is its short commit hash, each shown on its own
	// line, so a dev/PR build can be traced back to its exact branch and source
	// revision at a glance. Both are "local" for a local build. The channel
	// (dev/stable) is intentionally not shown here: the display name already
	// carries "(Dev)" for dev builds.
	//
	// Shown as a modal dialog (the trailing false = NOT a minor alert) so the
	// start-up confirmation is unmissable. A modal dialog displays both the main
	// text and the second "advice" argument, so the channel and commit go in the
	// advice line. (A minor alert would render only in the status bar and drop
	// the advice text entirely.)
	gSDK->AlertInform(PLUGIN_DISPLAY_NAME " plug-in started",
					  "branch: " VW_BUILD_BRANCH "\ncommit: " VW_BUILD_VERSION,
					  false /* not a minor alert: show a modal dialog */);
}
