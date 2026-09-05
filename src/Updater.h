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
//	Both channels end the same way: a build that installed successfully is only
//	LOADED at the next start-up, so the "installed" dialog is not a plain notice
//	but a question with a 再起動 button. Choosing it quits Vectorworks (with the
//	usual save prompt) and starts it again; choosing 後で keeps the running build
//	until the user restarts on their own.
//
//	Neither half of that restart is performed by this process. These checks run
//	while the plug-in is being LOADED at start-up (splash still up), and
//	Vectorworks is not ready to shut itself down then: the SDK's quit — with or
//	without its bRestart flag — ends in 「サポートファイルの読み込みに失敗しました」.
//	So a detached helper is started instead, which sends the ORDINARY OS quit
//	request (the ⌘Q one, delivered by the event loop only once Vectorworks is
//	really running), waits for this process to disappear, and then opens the
//	application again. See Updater.cpp and UpdaterParse.h's MacRelaunchCommand.
//

#pragma once

#include <string>
#include <vector>

namespace HomeskzIfcImport
{
	// Run ONE of the bundled scripts (baseName without its extension —
	// "vw-update" / "vw-feedback"; macOS adds ".sh", Windows ".ps1") and capture
	// its stdout. Returns false if the script could not be located or started.
	//
	// **本体（ペイロード）へ貸し出すためにここに口がある。** 本体は自分の在り処から
	// 同梱物へたどり着けない（読み込まれるのは一時ディレクトリへ写した複製で、
	// dladdr / GetModuleFileName はバンドルの外を指す）ので、殻の道具を借りる
	// ——境界の関数ポインタ VwPayloadHost::runBundledScript の実体がこれである。
	bool RunBundledScriptNamed(const std::string& baseName, const std::vector<std::string>& args,
							   std::string& out);

	// Stable plug-in only. At Vectorworks start-up, compare the installed stable
	// build with the latest published one; if a newer one exists, ask (native
	// dialog) whether to install it. Silent when already current or on a network
	// error. Runs only once per session.
	void RunStableStartupCheck();

	// Dev plug-in only. At Vectorworks start-up, ask (native dialogs) which build
	// to use: the currently installed one, or another branch's prerelease. If a
	// different build is chosen it is installed (and the user is offered an
	// immediate restart, since that is what loads it); otherwise
	// nothing happens and start-up continues. The picker is skipped entirely when
	// there is nothing to choose between — no prereleases exist, or the only one
	// is the running build. Silent on a network error. Runs only once per session.
	void RunDevStartupCheck();
} // namespace HomeskzIfcImport
