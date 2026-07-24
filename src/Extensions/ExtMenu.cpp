//
//	ExtMenu.cpp
//
//	Implementation of the sample menu command.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenu.h"

using namespace SamplePlugin;

namespace SamplePlugin
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
} // namespace SamplePlugin

// Every extension needs a globally unique ID and universal name. The stable and
// dev builds MUST use different ones so both plug-ins can be loaded at once.
//
// NOLINT: IMPLEMENT_VWMenuExtension is an SDK macro whose expansion trips
// misc-const-correctness (a `static VWIID iid` it could mark const); that is the
// macro's code, not ours, so silence the check across the two invocations.
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: cc72fd30-f2f6-4c39-8e7b-d81d5421898a  (dev build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuSample,
	/*Event sink*/ CSampleMenu_EventSink,
	/*Universal name*/ PLUGIN_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0xcc72fd30, 0xf2f6, 0x4c39, 0x8e, 0x7b, 0xd8, 0x1d, 0x54, 0x21, 0x89, 0x8a);
#else
// UUID: 4be7d497-0a1b-4c0e-aef9-aee94befc55e  (stable build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuSample,
	/*Event sink*/ CSampleMenu_EventSink,
	/*Universal name*/ PLUGIN_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x4be7d497, 0x0a1b, 0x4c0e, 0xae, 0xf9, 0xae, 0xe9, 0x4b, 0xef, 0xc5, 0x5e);
#endif
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuSample::CExtMenuSample(CallBackPtr cbp) : VWExtensionMenu(cbp, gMenuDef) {}

CExtMenuSample::~CExtMenuSample() = default;

// ---------------------------------------------------------------------------
CSampleMenu_EventSink::CSampleMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent) {}

CSampleMenu_EventSink::~CSampleMenu_EventSink() = default;

void CSampleMenu_EventSink::DoInterface()
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
