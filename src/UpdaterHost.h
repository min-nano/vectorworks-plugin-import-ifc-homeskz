//
//	UpdaterHost.h
//
//	The seam that lets the update FLOWS be tested without the Vectorworks SDK.
//
//	RunStableStartupCheck / RunDevStartupCheck are two small state machines:
//	"ask the script, decide, maybe show a dialog, maybe install, report". The
//	decisions are already pure (UpdaterParse.h); what remained SDK-bound was the
//	four side-effecting operations those flows perform:
//	  * run the bundled updater script and capture its stdout,
//	  * show an informational dialog,
//	  * ask a yes/no question,
//	  * show the build picker and return the chosen index.
//	Those four are gathered behind IUpdaterHost. The flows (UpdaterFlow.cpp)
//	depend ONLY on this interface, so they compile and run on any toolchain.
//
//	At run time Updater.cpp supplies the real implementation (gSDK dialogs +
//	popen'd script + the VWFC picker). In tests a fake implementation records the
//	calls and returns canned answers, so the whole flow — every branch and the
//	exact dialog wording — is exercised without the SDK (tests/UpdaterFlowTests.cpp).
//

#pragma once

#include <string>
#include <vector>

namespace SamplePlugin
{
	// The side effects the update flows perform. One method per operation that
	// would otherwise touch the SDK / the OS.
	struct IUpdaterHost
	{
		virtual ~IUpdaterHost() = default;

		// Run the bundled updater script with the given args and capture its
		// stdout into `out`. Returns false if the script could not be started
		// (missing/unresolved) — the flows treat that as "stay silent".
		virtual bool RunScript(const std::vector<std::string>& args, std::string& out) = 0;

		// Show a modal informational dialog (text + a secondary advice line).
		virtual void Inform(const std::string& text, const std::string& advice) = 0;

		// Ask a yes/no question. Returns true if the user chose the affirmative
		// (okText) button.
		virtual bool Ask(const std::string& text, const std::string& advice,
						 const std::string& okText, const std::string& cancelText) = 0;

		// Show the build picker listing `items` (entry 0 is the installed build),
		// preselecting `initialSel`. Returns the chosen 0-based index, or a
		// negative value if the user cancelled.
		virtual int PickBuild(const std::vector<std::string>& items, int initialSel) = 0;
	};

	// The SDK-independent update flows, parameterized by the host above. These
	// hold NO once-per-session state (the public wrappers in Updater.cpp do), so
	// tests can drive them repeatedly. runningBranch/runningCommit identify the
	// build currently loaded (compiled-in at run time; injected in tests).
	void RunStableStartupCheckWith(IUpdaterHost& host);
	void RunDevStartupCheckWith(IUpdaterHost& host,
								const std::string& runningBranch,
								const std::string& runningCommit);
}
