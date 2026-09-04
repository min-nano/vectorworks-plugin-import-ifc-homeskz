//
//	UpdaterHost.h
//
//	The seam that lets the update FLOWS be tested without the Vectorworks SDK.
//
//	RunStableStartupCheck / RunDevStartupCheck are two small state machines:
//	"ask the script, decide, maybe show a dialog, maybe install, report". The
//	decisions are already pure (UpdaterParse.h); what remained SDK-bound was the
//	side-effecting operations those flows perform:
//	  * run the bundled updater script and capture its stdout,
//	  * show an informational dialog,
//	  * ask a yes/no question,
//	  * show the build picker and return the chosen index,
//	  * restart Vectorworks (so a freshly installed build is loaded).
//	Those are gathered behind IUpdaterHost. The flows (UpdaterFlow.cpp)
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

namespace HomeskzIfcImport
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

		// 載っている本体（ペイロード）を降ろす。**インストールした本体をこの実行のまま
		// 効かせるための最後の一押し**で、次に本体を使うとき（取り込み・PIO のリセット）に
		// 新しいファイルが読み直される（src/PayloadSession.h）。降ろせなかった——本体の
		// コードがまだ走っている——ときだけ false。
		//
		// 起動時のチェックから呼ばれる限り、そもそも本体はまだ載っていないので、これは
		// たいてい「何もせず true」である。それでも呼ぶのは、**この判断（再起動が要らない）
		// と実際の載せ替えを 1 か所で完結させておく**ため——アップデートの確認をあとで
		// コマンドからも走らせるようにしたとき、ここが無いと黙って古いまま動き続ける。
		virtual bool DropLoadedPayload() = 0;

		// Quit Vectorworks and start it again, so the build just installed is
		// actually loaded (a compiled plug-in is only ever picked up at start-up).
		// Returns false if the restart could not even be ARRANGED (the host could
		// not work out what to relaunch, or could not start the helper that does
		// it) — Vectorworks is then left running untouched and the flow tells the
		// user to restart by hand. A true return only means "the quit was
		// requested": open documents still get the usual save prompt, and backing
		// out there simply leaves the old build running until the next start-up.
		virtual bool Restart() = 0;
	};

	// The SDK-independent update flows, parameterized by the host above. These
	// hold NO once-per-session state (the public wrappers in Updater.cpp do), so
	// tests can drive them repeatedly. runningBranch/runningCommit identify the
	// build currently loaded (compiled-in at run time; injected in tests).
	// runningShellId は**いま動いている殻の ID**（コンパイル時に焼かれた VW_SHELL_ID。
	// テストでは注入する）。入れたビルドの殻が同じなら、本体を読み直すだけで反映される
	// ＝**再起動を尋ねない**（src/UpdaterParse.h の NeedsRestartAfterInstall）。
	void RunStableStartupCheckWith(IUpdaterHost& host, const std::string& runningShellId);
	void RunDevStartupCheckWith(IUpdaterHost& host, const std::string& runningBranch,
								const std::string& runningCommit,
								const std::string& runningShellId);
} // namespace HomeskzIfcImport
