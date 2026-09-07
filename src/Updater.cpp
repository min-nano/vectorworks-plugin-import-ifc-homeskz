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
//	The RESTART offered after an install is asked of Vectorworks itself
//	(CloseAllFilesAndQuitVectorworks), not of a detached helper process — see
//	CVectorworksUpdaterHost::Restart and the note in Updater.h.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Updater.h"
#include "UpdaterHost.h"
#include "UpdaterParse.h"
#include "PayloadSession.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

// The pure parsing/quoting/path helpers live in UpdaterParse.h so they can be
// unit-tested without the SDK. Pull them into this file's scope; everything
// below is the platform-specific glue that uses them.
using namespace HomeskzIfcImport::UpdaterParse;

#if GS_MAC
#	include <dlfcn.h>
#endif

namespace
{
	// -----------------------------------------------------------------------
	// Bundled-script discovery + invocation. Two platform implementations of the
	// same primitives:
	//   BundledScriptPath()  absolute path of the updater script we ship, or "".
	//   BundlePluginsDir()   the Plug-Ins folder this build was loaded from, or "".
	//   RunBundledScript(args, out) run the script with args, capture stdout.
	//
	// **再起動はここには無い。** Vectorworks 自身に頼むので（CVectorworksUpdaterHost::
	// Restart）、プロセスを起こす仕掛けも、アプリの在り処を突き止める仕掛けも要らない。
	// -----------------------------------------------------------------------

#if GS_MAC

	// Absolute path of the bundled updater script, or "" if it can't be resolved.
	//
	// The installed plug-in is just the .vwlibrary bundle, so the script travels
	// inside it (CMake copies it to Contents/Resources/vw-update.sh). We find our
	// own loaded binary with dladdr() — its path is
	//   <name>.vwlibrary/Contents/MacOS/<name>
	// — and rewrite the trailing "MacOS/<name>" to "Resources/vw-update.sh".
	std::string BundledScriptPath(const std::string& baseName = "vw-update")
	{
		Dl_info info{};
		if (::dladdr(reinterpret_cast<const void*>(&BundledScriptPath), &info) == 0 ||
			info.dli_fname == nullptr)
			return "";

		// .../Contents/MacOS/<name> -> .../Contents/Resources/<baseName>.sh
		return MacScriptPathFromBinary(info.dli_fname, baseName);
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
	bool RunBundledScript(const std::vector<std::string>& args, std::string& out,
						  const std::string& baseName = "vw-update")
	{
		const std::string script = BundledScriptPath(baseName);
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

	// The bundled scripts sit next to the module (see CMakeLists.txt).
	std::string BundledScriptPath(const std::string& baseName = "vw-update")
	{
		return WinScriptPathFromDir(OwnModuleDir(), baseName);
	}

	// The Plug-Ins folder this build was loaded from == the module's own folder.
	std::string BundlePluginsDir()
	{
		return OwnModuleDir();
	}

	// Run "vw-update.ps1 <args>" via PowerShell and capture its stdout into out.
	// Blocks until the script finishes. Returns false if it could not be started.
	bool RunBundledScript(const std::vector<std::string>& args, std::string& out,
						  const std::string& baseName = "vw-update")
	{
		const std::string script = BundledScriptPath(baseName);
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

		// Vectorworks を終了して起動し直す。**Vectorworks 自身に頼む**——SDK の
		// CloseAllFilesAndQuitVectorworks(bAskForSave, bRestart) がその両方を行い、
		// 開いている文書の保存確認も通常どおり通る（保存ダイアログで取り消せば
		// Vectorworks は落ちず、更新はディスクに残ったまま次回の起動で反映される）。
		//
		// **以前はこれが使えなかった。** 更新の確認がプラグインの読み込み中
		// （スプラッシュ表示中）に走っていた頃は、SDK に終了を頼むと bRestart の
		// 有無にかかわらず「サポートファイルの読み込みに失敗しました」で落ちたため、
		// 終了要求も起動し直しも切り離したヘルパープロセスへ任せていた
		// （docs/DEVELOPMENT.md）。確認がメニューコマンドと取り込みコマンドへ移り、
		// **Vectorworks が完全に動いている最中にしか呼ばれなくなった**ので、その回り道は
		// 要らなくなった（Updater.h）。
		//
		// false を返すのは SDK をまだ掴めていないときだけ（呼び出し側は「手動で
		// 再起動してください」と案内する）。true は「終了を頼んだ」以上の意味を持たない。
		bool Restart() override
		{
			if (gSDK == nullptr)
				return false;
			gSDK->CloseAllFilesAndQuitVectorworks(/*bAskForSave*/ true, /*bRestart*/ true);
			return true;
		}

		// **入れ替えた本体（ペイロード）をこの実行のまま効かせる。** 降ろしておけば、次に
		// 本体を使うとき（取り込み・PIO のリセット）に新しいファイルが読み直される
		// （src/PayloadSession.h）。更新の確認は本体を確保する前に走るので、ここが
		// 呼ばれる時点では本体はスタックに載っていない。
		bool DropLoadedPayload() override
		{
			return HomeskzIfcImport::ReleaseLoadedPayload();
		}
	};
} // namespace

namespace HomeskzIfcImport
{
	// **本体（ペイロード）へ貸し出す道具。** 本体は自分の在り処から同梱物へたどり着け
	// ない——読み込まれるのは一時ディレクトリへ写した複製なので、dladdr /
	// GetModuleFileName が返すのはバンドルの外の道である（src/PayloadHost.h「必ず複製
	// してから読む」）。そこで殻がここを開けておき、境界越しに関数ポインタで渡す
	// （src/PayloadAbi.h の VwPayloadHost::runBundledScript）。
	bool RunBundledScriptNamed(const std::string& baseName, const std::vector<std::string>& args,
							   std::string& out)
	{
		return RunBundledScript(args, out, baseName);
	}

	// The public entry point is thin: it wires the real host + the compiled-in
	// build identity into the SDK-independent flows (UpdaterFlow.cpp), which hold
	// the actual logic (and the tests).
	//
	// **「一度きり」の見張りは持たない。** 起動時に自動で走っていた頃は
	// plugin_module_main が複数回呼ばれても 1 度で済ませる必要があったが、いまの入口は
	// 手で押すコマンドと取り込みコマンドなので、呼ばれた回数だけ確認するのが正しい。
	bool CheckForUpdates(UpdateCheckKind kind)
	{
		CVectorworksUpdaterHost host;
#ifdef VW_DEV_BUILD
		// The build that is actually loaded and running right now is compiled in
		// (VW_BUILD_BRANCH/VERSION), so it is unambiguous even if a different
		// build is staged on disk.
		return RunDevUpdateCheckWith(host, kind, VW_BUILD_BRANCH, VW_BUILD_VERSION, VW_SHELL_ID);
#else
		return RunStableUpdateCheckWith(host, kind, VW_SHELL_ID);
#endif
	}
} // namespace HomeskzIfcImport
