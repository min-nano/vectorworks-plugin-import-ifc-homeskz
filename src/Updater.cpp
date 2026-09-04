//
//	Updater.cpp
//
//	Native-dialog front end for the plug-in's self-update. All user interaction
//	uses the Vectorworks SDK (gSDK->AlertInform / gSDK->AlertQuestion). The
//	actual work (GitHub API, download, install) is delegated to a bundled
//	updater script, invoked non-interactively; see Updater.h for the contract.
//
//	The script and the way we locate ourselves are platform-specific:
//	  * macOS   -> vw-update.sh, run with /bin/bash; own path found via dladdr.
//	  * Windows -> vw-update.ps1, run with PowerShell; own path via
//	               GetModuleFileName.
//	Everything else (parsing, native dialogs, the update flows) is shared.
//
//	The same script also performs the RESTART offered after an install: it is
//	started detached in "relaunch" mode and opens the application again once this
//	process is gone. See CVectorworksUpdaterHost::Restart for why the SDK's own
//	bRestart flag is not used.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Updater.h"
#include "UpdaterHost.h"
#include "UpdaterParse.h"
#include "PayloadSession.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The pure parsing/quoting/path helpers live in UpdaterParse.h so they can be
// unit-tested without the SDK. Pull them into this file's scope; everything
// below is the platform-specific glue that uses them.
using namespace HomeskzIfcImport::UpdaterParse;

#if GS_MAC
#	include <CoreFoundation/CoreFoundation.h>
#	include <dlfcn.h>
#	include <mach-o/dyld.h>
#	include <unistd.h>
#endif

namespace
{
	// -----------------------------------------------------------------------
	// Bundled-script discovery + invocation, and the restart helper. Two platform
	// implementations of the same primitives:
	//   BundledScriptPath()  absolute path of the updater script we ship, or "".
	//   BundlePluginsDir()   the Plug-Ins folder this build was loaded from, or "".
	//   RunBundledScript(args, out) run the script with args, capture stdout.
	//   RestartCommand()     the quit-wait-relaunch one-liner for this machine,
	//                        or "" if the application could not be identified.
	//   SpawnDetachedShell(script)  run it with no window, no pipes and no wait,
	//                        so it outlives this process — the restart helper
	//                        must still be around after Vectorworks quits.
	// -----------------------------------------------------------------------

#if GS_MAC

	// Absolute path of the bundled updater script, or "" if it can't be resolved.
	//
	// The installed plug-in is just the .vwlibrary bundle, so the script travels
	// inside it (CMake copies it to Contents/Resources/vw-update.sh). We find our
	// own loaded binary with dladdr() — its path is
	//   <name>.vwlibrary/Contents/MacOS/<name>
	// — and rewrite the trailing "MacOS/<name>" to "Resources/vw-update.sh".
	std::string BundledScriptPath()
	{
		Dl_info info{};
		if (::dladdr(reinterpret_cast<const void*>(&BundledScriptPath), &info) == 0 ||
			info.dli_fname == nullptr)
			return "";

		// .../Contents/MacOS/<name> -> .../Contents/Resources/vw-update.sh
		return MacScriptPathFromBinary(info.dli_fname);
	}

	// Directory that CONTAINS this plug-in's .vwlibrary bundle — i.e. the exact
	// Plug-Ins folder Vectorworks actually loaded this build from. Returns "" if
	// it can't be resolved.
	//
	// This is what makes the updater install to the RIGHT place: the plug-in may
	// live in a custom Vectorworks user folder (Vectorworks ▸ 環境設定 ▸ ユーザ
	// フォルダ), not the default path. Installing next to the running bundle
	// guarantees the update replaces the copy that is actually loaded, so the new
	// build is picked up on the next restart. From
	//   .../<PlugIns>/<name>.vwlibrary/Contents/MacOS/<name>
	// we strip back to "<PlugIns>".
	std::string BundlePluginsDir()
	{
		Dl_info info{};
		if (::dladdr(reinterpret_cast<const void*>(&BundlePluginsDir), &info) == 0 ||
			info.dli_fname == nullptr)
			return "";

		// .../<PlugIns>/<name>.vwlibrary/Contents/MacOS/<name> -> .../<PlugIns>
		return MacPluginsDirFromBinary(info.dli_fname);
	}

	// Run "vw-update.sh <args>" and capture its stdout into out. Blocks until the
	// script finishes. Returns false if the script could not be located/started.
	bool RunBundledScript(const std::vector<std::string>& args, std::string& out)
	{
		const std::string script = BundledScriptPath();
		if (script.empty())
			return false;

		// Point the script at the folder this build was actually loaded from, so
		// it reads the installed commit from — and installs over — the copy
		// Vectorworks really uses (not a guessed default path).
		std::string env;
		const std::string pluginsDir = BundlePluginsDir();
		if (!pluginsDir.empty())
			env = "VW_PLUGINS_DIR=" + ShellQuote(pluginsDir) + " ";

		std::string cmd = env + "/bin/bash " + ShellQuote(script);
		for (const std::string& a : args)
			cmd += " " + ShellQuote(a);
		cmd += " 2>/dev/null";

		FILE* pipe = ::popen(cmd.c_str(), "r");
		if (pipe == nullptr)
			return false;

		out.clear();
		std::array<char, 4096> buf{};
		size_t n = 0;
		while ((n = ::fread(buf.data(), 1, buf.size(), pipe)) > 0)
			out.append(buf.data(), n);
		::pclose(pipe);
		return true;
	}

	// The Vectorworks application bundle to launch when restarting, or "" if it
	// cannot be resolved. _NSGetExecutablePath gives the HOST executable (the one
	// that loaded us — Vectorworks itself), i.e.
	//   /Applications/Vectorworks 2026/Vectorworks.app/Contents/MacOS/Vectorworks
	// and we hand `open` the enclosing .app (see MacAppBundleFromExecutable).
	std::string HostAppPath()
	{
		// _NSGetExecutablePath fills the buffer when it fits, and otherwise
		// returns non-zero after writing the required length back into `size` —
		// so a too-small first guess costs one extra call, never a truncation.
		std::uint32_t size = 1024;
		std::string buf(size, '\0');
		if (::_NSGetExecutablePath(buf.data(), &size) != 0)
		{
			buf.assign(size, '\0');
			if (::_NSGetExecutablePath(buf.data(), &size) != 0)
				return "";
		}
		buf.resize(std::strlen(buf.c_str())); // drop the unused tail

		return MacAppBundleFromExecutable(buf);
	}

	// Bundle identifier of the HOST application (Vectorworks), or "" if it cannot
	// be read. CFBundleGetMainBundle() is the application that loaded us, not this
	// plug-in, which is exactly what the restart needs: an identifier to address
	// the quit request to that does not depend on the (localized, versioned)
	// application name.
	std::string HostBundleId()
	{
		// No `const` on these: CFBundleRef / CFStringRef are pointer typedefs, so
		// a trailing const would qualify the POINTER, not what it points at
		// (clang-tidy's misc-misplaced-const rejects it).
		CFBundleRef mainBundle = ::CFBundleGetMainBundle();
		if (mainBundle == nullptr)
			return "";
		CFStringRef identifier = ::CFBundleGetIdentifier(mainBundle);
		if (identifier == nullptr)
			return "";

		std::array<char, 512> buf{};
		if (::CFStringGetCString(identifier, buf.data(), buf.size(), kCFStringEncodingUTF8) == 0)
			return "";
		return buf.data();
	}

	// Run `script` with /bin/sh DETACHED, returning immediately: nohup + & so the
	// helper keeps running after Vectorworks exits (that is the whole point — it
	// waits for this process to die and then launches the app again), and all
	// output is discarded because nobody is left to read it. Returns false if the
	// shell could not be started.
	bool SpawnDetachedShell(const std::string& script)
	{
		const std::string cmd = "nohup /bin/sh -c " + ShellQuote(script) + " >/dev/null 2>&1 &";
		return std::system(cmd.c_str()) == 0;
	}

	// This process' id, for the helper to address and wait on.
	std::string OwnProcessId()
	{
		return std::to_string(static_cast<long long>(::getpid()));
	}

	// The command that restarts Vectorworks: quit, wait, launch again.
	std::string RestartCommand()
	{
		const std::string app = HostAppPath();
		const std::string bundleId = HostBundleId();
		if (app.empty() || bundleId.empty())
			return ""; // nothing safe to address or relaunch
		return MacRelaunchCommand(OwnProcessId(), app, bundleId);
	}

#elif GS_WIN

	// UTF-8 <-> UTF-16 helpers (the Win32 *W APIs and paths are UTF-16; the rest
	// of this file, and the script's I/O, are UTF-8).
	std::wstring Widen(const std::string& s)
	{
		if (s.empty())
			return L"";
		const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
		std::wstring w(n, L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
		return w;
	}

	std::string Narrow(const std::wstring& w)
	{
		if (w.empty())
			return "";
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
											nullptr, nullptr);
		std::string s(n, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
		return s;
	}

	// Full path of THIS module (the loaded .vlb), as UTF-8, or "" on failure.
	// GetModuleHandleEx with an address inside this module resolves our own DLL
	// regardless of the executable that loaded it.
	std::string OwnModulePath()
	{
		HMODULE self = nullptr;
		if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
									 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								 reinterpret_cast<LPCWSTR>(&OwnModulePath), &self) == 0 ||
			self == nullptr)
			return "";

		std::wstring buf(MAX_PATH, L'\0');
		DWORD len = ::GetModuleFileNameW(self, buf.data(), (DWORD)buf.size());
		// Grow once if the path was longer than MAX_PATH.
		while (len == buf.size())
		{
			buf.resize(buf.size() * 2, L'\0');
			len = ::GetModuleFileNameW(self, buf.data(), (DWORD)buf.size());
		}
		if (len == 0)
			return "";
		buf.resize(len);
		return Narrow(buf);
	}

	// Directory that contains this module. On Windows the plug-in is a bare
	// "<name>.vlb" living directly in the Plug-Ins folder, so this is both where
	// the updater script sits and the Plug-Ins folder to install into.
	std::string OwnModuleDir()
	{
		// ...\<PlugIns>\<name>.vlb -> ...\<PlugIns>
		return WinModuleDirFromPath(OwnModulePath());
	}

	// The bundled updater script sits next to the module (see CMakeLists.txt).
	std::string BundledScriptPath()
	{
		return WinScriptPathFromDir(OwnModuleDir());
	}

	// The Plug-Ins folder this build was loaded from == the module's own folder.
	std::string BundlePluginsDir()
	{
		return OwnModuleDir();
	}

	// Run "vw-update.ps1 <args>" via PowerShell and capture its stdout into out.
	// Blocks until the script finishes. Returns false if it could not be started.
	bool RunBundledScript(const std::vector<std::string>& args, std::string& out)
	{
		const std::string script = BundledScriptPath();
		if (script.empty())
			return false;

		// Point the script at the folder this build was actually loaded from, so
		// it reads the installed commit from — and installs over — the copy
		// Vectorworks really uses (not a guessed default path). The child
		// PowerShell inherits this process environment.
		const std::string pluginsDir = BundlePluginsDir();
		if (!pluginsDir.empty())
			::SetEnvironmentVariableW(L"VW_PLUGINS_DIR", Widen(pluginsDir).c_str());

		std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File " + CmdQuote(script);
		for (const std::string& a : args)
			cmd += " " + CmdQuote(a);
		cmd += " 2>NUL";

		FILE* pipe = ::_popen(cmd.c_str(), "r");
		if (pipe == nullptr)
		{
			if (!pluginsDir.empty())
				::SetEnvironmentVariableW(L"VW_PLUGINS_DIR", nullptr);
			return false;
		}

		out.clear();
		std::array<char, 4096> buf{};
		size_t n = 0;
		while ((n = ::fread(buf.data(), 1, buf.size(), pipe)) > 0)
			out.append(buf.data(), n);
		::_pclose(pipe);

		if (!pluginsDir.empty())
			::SetEnvironmentVariableW(L"VW_PLUGINS_DIR", nullptr);
		return true;
	}

	// The Vectorworks executable to start when restarting, or "" if it cannot be
	// resolved. GetModuleFileNameW(nullptr) returns the HOST executable (the one
	// that loaded this .vlb — Vectorworks itself), which is exactly what
	// Start-Process needs.
	std::string HostAppPath()
	{
		std::wstring buf(MAX_PATH, L'\0');
		DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
		while (len == buf.size()) // path longer than the buffer -> grow and retry
		{
			buf.resize(buf.size() * 2, L'\0');
			len = ::GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
		}
		if (len == 0)
			return "";
		buf.resize(len);
		return Narrow(buf);
	}

	// Run `script` with PowerShell DETACHED and return immediately: the helper
	// must outlive Vectorworks (it asks it to quit, waits for the process to die,
	// then starts the application again), so it is created as its own process
	// with no window and no pipes — unlike RunBundledScript, nothing is read
	// back. Returns false if the process could not be created.
	//
	// The script is passed as ONE double-quoted -Command argument, which works
	// because WinRelaunchCommand quotes everything inside it with single quotes.
	bool SpawnDetachedShell(const std::string& script)
	{
		const std::string cmd =
			"powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command " +
			CmdQuote(script);

		// CreateProcessW may modify the command line buffer, hence a writable copy.
		std::wstring wcmd = Widen(cmd);
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};
		if (::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
							 nullptr, nullptr, &si, &pi) == 0)
			return false;

		// We never wait on it; drop both handles so nothing is leaked.
		::CloseHandle(pi.hThread);
		::CloseHandle(pi.hProcess);
		return true;
	}

	// This process' id, for the helper to address and wait on.
	std::string OwnProcessId()
	{
		return std::to_string(static_cast<long long>(::GetCurrentProcessId()));
	}

	// The command that restarts Vectorworks: quit, wait, launch again.
	std::string RestartCommand()
	{
		const std::string exe = HostAppPath();
		if (exe.empty())
			return ""; // nothing to relaunch
		return WinRelaunchCommand(OwnProcessId(), exe);
	}

#endif // GS_WIN

	// The script-output parsing helpers (Trim / ValueOf / DevBuild /
	// ParseDevBuilds) are SDK-independent and live in UpdaterParse.h so they can
	// be unit-tested; they are pulled in via the `using namespace` at the top of
	// this file and used unchanged below.

	// -----------------------------------------------------------------------
	// Native pull-down list dialog (VWFC::VWUI) for choosing a build.
	//
	// A single modal dialog with one drop-down listing every choice at once:
	// entry 0 is the currently installed build, the rest are other branches'
	// prereleases. The selected 0-based index is delivered via DDX into
	// fSelection. All signatures follow the Vectorworks 2026 SDK headers
	// (VWFC/VWUI/{Dialog,PullDownMenuCtrl,StaticTextCtrl}.h); the control classes
	// and the event-map macros come in via PluginPrefix.h -> VectorworksSDK.h.
	// -----------------------------------------------------------------------
	class CBuildPickerDialog : public VWDialog
	{
	public:
		CBuildPickerDialog(const std::vector<TXString>& items, short initialSel)
			: fPrompt(kPromptID), fPopup(kPopupID), fItems(items), fSelection(initialSel)
		{
		}
		~CBuildPickerDialog() override = default;

		short GetSelection() const
		{
			return fSelection;
		}

	protected:
		// Build the dialog and its controls (called by RunDialogLayout).
		bool CreateDialogLayout() override
		{
			// hasHelp = false -> a plain OK / Cancel dialog, no help button.
			if (!this->CreateDialog("使用する開発版ビルドを選択", "OK", "キャンセル", false))
				return false;
			if (!fPrompt.CreateControl(this, "使用するビルドを選択してください:"))
				return false;
			if (!fPopup.CreateControl(this, 52 /* width in standard chars */))
				return false;
			this->AddFirstGroupControl(&fPrompt);
			this->AddBelowControl(&fPrompt, &fPopup);
			return true;
		}

		// Fill the drop-down and preselect the initial item (control now exists).
		void OnInitializeContent() override
		{
			VWDialog::OnInitializeContent();
			for (const TXString& item : fItems)
				fPopup.AddItem(item);
			if (fSelection >= 0 && size_t(fSelection) < fItems.size())
				fPopup.SelectIndex(size_t(fSelection));
		}

		// Bind the drop-down's selected index to fSelection (both directions).
		void OnDDXInitialize() override
		{
			this->AddDDX_PulldownMenu(kPopupID, &fSelection);
		}

		// Required by VWDialog even with no per-control event handlers.
		DEFINE_EVENT_DISPATH_MAP;

	private:
		enum
		{
			kPromptID = 3,
			kPopupID = 4
		}; // 1 = OK, 2 = Cancel are reserved.
		VWStaticTextCtrl fPrompt;
		VWPullDownMenuCtrl fPopup;
		std::vector<TXString> fItems;
		short fSelection;
	};

	// EVENT_DISPATCH_MAP_BEGIN is an SDK macro; its expansion declares a local the
	// check would want const — the macro's code, not ours.
	// NOLINTNEXTLINE(misc-const-correctness)
	EVENT_DISPATCH_MAP_BEGIN(CBuildPickerDialog);
	EVENT_DISPATCH_MAP_END;

	// -----------------------------------------------------------------------
	// The concrete host the plug-in uses at run time. It implements the four
	// IUpdaterHost seams the SDK-independent flows (UpdaterFlow.cpp) call, in
	// terms of the real Vectorworks SDK dialogs, the bundled script, and the
	// VWFC picker above. Swapping a fake in for this interface is what lets those
	// flows be unit-tested without the SDK (see tests/UpdaterFlowTests.cpp).
	//
	// Note: the SDK's TXString constructs implicitly from a (UTF-8) const char*,
	// so we pass std::string::c_str() directly and let that conversion happen.
	// -----------------------------------------------------------------------
	class CVectorworksUpdaterHost : public HomeskzIfcImport::IUpdaterHost
	{
	public:
		bool RunScript(const std::vector<std::string>& args, std::string& out) override
		{
			return RunBundledScript(args, out);
		}

		void Inform(const std::string& text, const std::string& advice) override
		{
			// false => modal dialog (not a minor/status-bar alert), so the advice
			// line is shown too. Matches the existing menu-command alert.
			gSDK->AlertInform(text.c_str(), advice.c_str(), false);
		}

		// Yes/no question. Returns true if the user chose the affirmative button.
		bool Ask(const std::string& text, const std::string& advice, const std::string& okText,
				 const std::string& cancelText) override
		{
			// AlertQuestion returns 0 = negative/cancel, 1 = positive/OK, 2/3 =
			// custom buttons A/B. defaultButton 1 = the OK button is the default.
			const short r =
				gSDK->AlertQuestion(text.c_str(), advice.c_str(),
									/*defaultButton*/ 1, okText.c_str(), cancelText.c_str(),
									/*customButtonA*/ "", /*customButtonB*/ "");
			return r == 1;
		}

		// Show the native build picker; return the chosen 0-based index, or -1 if
		// the user cancelled.
		int PickBuild(const std::vector<std::string>& items, int initialSel) override
		{
			std::vector<TXString> txItems;
			txItems.reserve(items.size());
			for (const std::string& s : items)
				txItems.emplace_back(s.c_str());

			CBuildPickerDialog dlg(txItems, static_cast<short>(initialSel));
			if (dlg.RunDialogLayout("") != VWFC::VWUI::kDialogButton_Ok)
				return -1; // cancelled -> keep the loaded build
			return dlg.GetSelection();
		}

		// Quit Vectorworks and launch it again, so the freshly installed build is
		// loaded. Returns false if the restart could not even be arranged (the
		// application could not be identified, or the helper would not start), in
		// which case Vectorworks is left running untouched and the caller tells
		// the user to restart by hand.
		//
		// Neither half is done by this process; both are handed to a detached
		// helper (see the comment above MacRelaunchCommand in UpdaterParse.h).
		// The short version: this code runs while the plug-in is being LOADED at
		// start-up, and Vectorworks can neither quit nor be replaced safely at
		// that moment — the SDK's own quit/restart both end in 「サポートファイル
		// の読み込みに失敗しました」. The helper instead sends the ordinary
		// OS-level quit request (the ⌘Q / close-box one), which the event loop
		// picks up only once Vectorworks is really running, and relaunches the
		// application after the process is gone.
		//
		// So all this does is start the helper; the quit arrives from outside a
		// moment later, with the usual save prompt. Backing out of that prompt
		// keeps Vectorworks running, the helper gives up after its timeout, and
		// the installed build is picked up at the next start-up anyway.
		bool Restart() override
		{
			const std::string command = RestartCommand();
			if (command.empty())
				return false;
			return SpawnDetachedShell(command);
		}

		// **入れ替えた本体（ペイロード）をこの実行のまま効かせる。** 降ろしておけば、次に
		// 本体を使うとき（取り込み・PIO のリセット）に新しいファイルが読み直される
		// （src/PayloadSession.h）。起動時のチェックから呼ばれる限り本体はまだ載って
		// いないので、たいていは「何もせず true」。
		bool DropLoadedPayload() override
		{
			return HomeskzIfcImport::ReleaseLoadedPayload();
		}
	};
} // namespace

namespace HomeskzIfcImport
{
	// The public entry points are thin: they enforce "run once per session" and
	// wire the real host + compiled-in build identity into the SDK-independent
	// flows (UpdaterFlow.cpp), which hold the actual logic (and the tests).

	void RunStableStartupCheck()
	{
		// plugin_module_main can be called more than once per session; only do
		// the check the first time.
		static bool sDone = false;
		if (sDone)
			return;
		sDone = true;

		CVectorworksUpdaterHost host;
		RunStableStartupCheckWith(host, VW_SHELL_ID);
	}

	void RunDevStartupCheck()
	{
		// plugin_module_main can be called more than once per session; only offer
		// the picker the first time (mirrors RunStableStartupCheck).
		static bool sDone = false;
		if (sDone)
			return;
		sDone = true;

		// The build that is actually loaded and running right now is compiled in
		// (VW_BUILD_BRANCH/VERSION), so it is unambiguous even if a different
		// build is staged on disk.
		CVectorworksUpdaterHost host;
		RunDevStartupCheckWith(host, VW_BUILD_BRANCH, VW_BUILD_VERSION, VW_SHELL_ID);
	}
} // namespace HomeskzIfcImport
