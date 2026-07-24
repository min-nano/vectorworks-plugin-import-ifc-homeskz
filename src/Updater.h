//
//	Updater.h
//
//	Drives the plug-in's self-update using NATIVE Vectorworks dialogs
//	(gSDK->AlertInform / gSDK->AlertQuestion). The network + install mechanics
//	are delegated to a bundled updater script (macOS: vw-update.sh via bash;
//	Windows: vw-update.ps1 via PowerShell) shipped alongside the installed
//	plug-in (it runs non-interactively and prints machine-readable output; see
//	its q-stable / q-dev / do-install modes), while every user-facing dialog is
//	shown by the plug-in itself. So nobody has to open a terminal.
//
//	  * The STABLE plug-in checks for a newer build at Vectorworks start-up:
//	    RunStableStartupCheck() — silent when already current, otherwise asks
//	    (native yes/no) whether to install.
//	  * The DEV plug-in lets the user pick which branch's build to use, also at
//	    Vectorworks start-up: RunDevStartupCheck() — installs the chosen build
//	    only if it differs from the one already installed. Doing this at start-up
//	    (rather than each time the command runs) matters because a plug-in may
//	    re-invoke its own command programmatically, and the build in use can only
//	    change at start-up anyway — so the picker belongs where the build is
//	    actually loaded, not on every command run.
//

#pragma once

namespace SamplePlugin
{
	// Stable plug-in only. At Vectorworks start-up, compare the installed stable
	// build with the latest published one; if a newer one exists, ask (native
	// dialog) whether to install it. Silent when already current or on a network
	// error. Runs only once per session.
	void RunStableStartupCheck();

	// Dev plug-in only. At Vectorworks start-up, ask (native dialogs) which build
	// to use: the currently installed one, or another branch's prerelease. If a
	// different build is chosen it is installed (restart to load it); otherwise
	// nothing happens and start-up continues. Silent on a network error. Runs
	// only once per session.
	void RunDevStartupCheck();
} // namespace SamplePlugin
