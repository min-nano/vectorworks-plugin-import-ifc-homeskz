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

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Updater.h"
#include "UpdaterHost.h"
#include "UpdaterParse.h"

#include <cstdio>
#include <string>
#include <vector>

// The pure parsing/quoting/path helpers live in UpdaterParse.h so they can be
// unit-tested without the SDK. Pull them into this file's scope; everything
// below is the platform-specific glue that uses them.
using namespace SamplePlugin::UpdaterParse;

#if GS_MAC
#	include <dlfcn.h>
#endif

namespace
{
	// -----------------------------------------------------------------------
	// Bundled-script discovery + invocation. Two platform implementations of
	// the same three primitives:
	//   BundledScriptPath()  absolute path of the updater script we ship, or "".
	//   BundlePluginsDir()   the Plug-Ins folder this build was loaded from, or "".
	//   RunBundledScript(args, out) run the script with args, capture stdout.
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
		if (::dladdr(reinterpret_cast<const void*>(&BundledScriptPath), &info) == 0
			|| info.dli_fname == nullptr)
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
		if (::dladdr(reinterpret_cast<const void*>(&BundlePluginsDir), &info) == 0
			|| info.dli_fname == nullptr)
			return "";

		// .../<PlugIns>/<name>.vwlibrary/Contents/MacOS/<name> -> .../<PlugIns>
		return MacPluginsDirFromBinary(info.dli_fname);
	}

	// Run "vw-update.sh <args>" and capture its stdout into out. Blocks until the
	// script finishes. Returns false if the script could not be located/started.
	bool RunBundledScript(const std::vector<std::string>& args, std::string& out)
	{
		std::string script = BundledScriptPath();
		if (script.empty())
			return false;

		// Point the script at the folder this build was actually loaded from, so
		// it reads the installed commit from — and installs over — the copy
		// Vectorworks really uses (not a guessed default path).
		std::string env;
		std::string pluginsDir = BundlePluginsDir();
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
		char buf[4096];
		size_t n = 0;
		while ((n = ::fread(buf, 1, sizeof(buf), pipe)) > 0)
			out.append(buf, n);
		::pclose(pipe);
		return true;
	}

#elif GS_WIN

	// UTF-8 <-> UTF-16 helpers (the Win32 *W APIs and paths are UTF-16; the rest
	// of this file, and the script's I/O, are UTF-8).
	std::wstring Widen(const std::string& s)
	{
		if (s.empty()) return L"";
		int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
		std::wstring w(n, L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
		return w;
	}

	std::string Narrow(const std::wstring& w)
	{
		if (w.empty()) return "";
		int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
		std::string s(n, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
		return s;
	}

	// Full path of THIS module (the loaded .vlb), as UTF-8, or "" on failure.
	// GetModuleHandleEx with an address inside this module resolves our own DLL
	// regardless of the executable that loaded it.
	std::string OwnModulePath()
	{
		HMODULE self = nullptr;
		if (::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&OwnModulePath), &self) == 0
			|| self == nullptr)
			return "";

		std::wstring buf(MAX_PATH, L'\0');
		DWORD len = ::GetModuleFileNameW(self, &buf[0], (DWORD)buf.size());
		// Grow once if the path was longer than MAX_PATH.
		while (len == buf.size())
		{
			buf.resize(buf.size() * 2, L'\0');
			len = ::GetModuleFileNameW(self, &buf[0], (DWORD)buf.size());
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
		std::string script = BundledScriptPath();
		if (script.empty())
			return false;

		// Point the script at the folder this build was actually loaded from, so
		// it reads the installed commit from — and installs over — the copy
		// Vectorworks really uses (not a guessed default path). The child
		// PowerShell inherits this process environment.
		std::string pluginsDir = BundlePluginsDir();
		if (!pluginsDir.empty())
			::SetEnvironmentVariableW(L"VW_PLUGINS_DIR", Widen(pluginsDir).c_str());

		std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File "
			+ CmdQuote(script);
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
		char buf[4096];
		size_t n = 0;
		while ((n = ::fread(buf, 1, sizeof(buf), pipe)) > 0)
			out.append(buf, n);
		::_pclose(pipe);

		if (!pluginsDir.empty())
			::SetEnvironmentVariableW(L"VW_PLUGINS_DIR", nullptr);
		return true;
	}

#endif	// GS_WIN

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
			: fPrompt(kPromptID), fPopup(kPopupID), fItems(items), fSelection(initialSel) {}
		virtual ~CBuildPickerDialog() {}

		short	GetSelection() const { return fSelection; }

	protected:
		// Build the dialog and its controls (called by RunDialogLayout).
		virtual bool CreateDialogLayout() override
		{
			// hasHelp = false -> a plain OK / Cancel dialog, no help button.
			if (! this->CreateDialog("使用する開発版ビルドを選択", "OK", "キャンセル", false))
				return false;
			if (! fPrompt.CreateControl(this, "使用するビルドを選択してください:"))
				return false;
			if (! fPopup.CreateControl(this, 52 /* width in standard chars */))
				return false;
			this->AddFirstGroupControl(& fPrompt);
			this->AddBelowControl(& fPrompt, & fPopup);
			return true;
		}

		// Fill the drop-down and preselect the initial item (control now exists).
		virtual void OnInitializeContent() override
		{
			VWDialog::OnInitializeContent();
			for (const TXString& item : fItems)
				fPopup.AddItem(item);
			if (fSelection >= 0 && size_t(fSelection) < fItems.size())
				fPopup.SelectIndex(size_t(fSelection));
		}

		// Bind the drop-down's selected index to fSelection (both directions).
		virtual void OnDDXInitialize() override
		{
			this->AddDDX_PulldownMenu(kPopupID, & fSelection);
		}

		// Required by VWDialog even with no per-control event handlers.
		DEFINE_EVENT_DISPATH_MAP;

	private:
		enum { kPromptID = 3, kPopupID = 4 };	// 1 = OK, 2 = Cancel are reserved.
		VWStaticTextCtrl		fPrompt;
		VWPullDownMenuCtrl		fPopup;
		std::vector<TXString>	fItems;
		short					fSelection;
	};

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
	class CVectorworksUpdaterHost : public SamplePlugin::IUpdaterHost
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
		bool Ask(const std::string& text, const std::string& advice,
				 const std::string& okText, const std::string& cancelText) override
		{
			// AlertQuestion returns 0 = negative/cancel, 1 = positive/OK, 2/3 =
			// custom buttons A/B. defaultButton 1 = the OK button is the default.
			short r = gSDK->AlertQuestion(
				text.c_str(), advice.c_str(),
				/*defaultButton*/ 1,
				okText.c_str(), cancelText.c_str(),
				/*customButtonA*/ "", /*customButtonB*/ "");
			return r == 1;
		}

		// Show the native build picker; return the chosen 0-based index, or -1 if
		// the user cancelled.
		int PickBuild(const std::vector<std::string>& items, int initialSel) override
		{
			std::vector<TXString> txItems;
			for (const std::string& s : items)
				txItems.push_back(TXString(s.c_str()));

			CBuildPickerDialog dlg(txItems, static_cast<short>(initialSel));
			if (dlg.RunDialogLayout("") != VWFC::VWUI::kDialogButton_Ok)
				return -1;						// cancelled -> keep the loaded build
			return dlg.GetSelection();
		}
	};
}

namespace SamplePlugin
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
		RunStableStartupCheckWith(host);
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
		RunDevStartupCheckWith(host, VW_BUILD_BRANCH, VW_BUILD_VERSION);
	}
}
