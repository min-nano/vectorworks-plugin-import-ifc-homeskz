//
//	PayloadHost.cpp
//
//	PayloadHost.h の実装。**SDK の型は使わない**（境界を SDK から独立に保つため。
//	プリコンパイルヘッダ経由で宣言は入ってくるが、ここでは触らない）。プラットフォーム
//	判別も SDK の GS_MAC / GS_WIN ではなく素の __APPLE__ / _WIN32 で行う。
//
//	プラットフォーム依存はここに閉じている:
//	  * 自分の位置           … mac: dladdr / win: GetModuleHandleExW
//	  * 読み込み・解決・アンロード … mac: dlopen/dlsym/dlclose / win: LoadLibraryExW ほか
//	  * 残っているかの確認   … mac: dlopen(RTLD_NOLOAD) / win: GetModuleHandleW
//	ファイルの複製・削除・印（大きさと更新時刻）は std::filesystem で足りるので分岐しない。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "PayloadHost.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#	include <Windows.h>
#else
#	include <dlfcn.h>
#endif

namespace HomeskzIfcImport
{
	namespace
	{
#if defined(_WIN32)
		std::wstring Widen(const std::string& s)
		{
			if (s.empty())
				return L"";
			const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
			std::wstring w((size_t)n, L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
			return w;
		}

		std::string Narrow(const std::wstring& w)
		{
			if (w.empty())
				return "";
			const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
												nullptr, nullptr);
			std::string s((size_t)n, '\0');
			::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr,
								  nullptr);
			return s;
		}

		// GetLastError() を読める形に（メッセージまでは要らない。番号があれば追える）。
		std::string LastError(const char* what)
		{
			return std::string(what) + " が失敗しました（GetLastError=" +
				   std::to_string((unsigned long)::GetLastError()) + "）";
		}
#endif
	} // namespace

	// -----------------------------------------------------------------------
	PayloadModule::~PayloadModule()
	{
		// **ここでは降ろさない**（PayloadHost.h の注記）。open したまま捨てられた場合は
		// モジュールがプロセスに残るだけで、壊れはしない。
	}

	bool PayloadModule::open(const std::string& path, std::string& error)
	{
		error.clear();
		if (fHandle != nullptr)
		{
			error = "すでに読み込んでいる（" + fPath + "）";
			return false;
		}
		if (path.empty())
		{
			error = "パスが空";
			return false;
		}

#if defined(_WIN32)
		// LOAD_WITH_ALTERED_SEARCH_PATH: 依存 DLL を**そのファイルの隣**から探させる
		// （Vectorworks 本体のフォルダではなく）。
		HMODULE handle =
			::LoadLibraryExW(Widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (handle == nullptr)
		{
			error = LastError("LoadLibraryExW");
			return false;
		}
		fHandle = (void*)handle;
#else
		// RTLD_NOW: 未解決シンボルを**読み込んだ時点で**弾く（呼んだ瞬間に落ちるより、
		// 読み込みが失敗して理由が出るほうが調査になる）。
		// RTLD_LOCAL: このモジュールの記号をプロセス全体へ晒さない。本体は自分の SDK
		// （libVWSDK.a）の複製を持つので、晒すと殻の側の同名記号と混ざりうる。
		void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (handle == nullptr)
		{
			const char* why = ::dlerror();
			error =
				std::string("dlopen が失敗しました: ") + ((why != nullptr) ? why : "(理由なし)");
			return false;
		}
		fHandle = handle;
#endif
		fPath = path;
		return true;
	}

	bool PayloadModule::close(std::string& error)
	{
		error.clear();
		if (fHandle == nullptr)
			return true;

#if defined(_WIN32)
		const BOOL ok = ::FreeLibrary((HMODULE)fHandle);
		fHandle = nullptr;
		if (ok == 0)
		{
			error = LastError("FreeLibrary");
			return false;
		}
#else
		const int rc = ::dlclose(fHandle);
		fHandle = nullptr;
		if (rc != 0)
		{
			const char* why = ::dlerror();
			error =
				std::string("dlclose が失敗しました: ") + ((why != nullptr) ? why : "(理由なし)");
			return false;
		}
#endif
		return true;
	}

	void* PayloadModule::symbol(const char* name) const
	{
		if (fHandle == nullptr || name == nullptr)
			return nullptr;
#if defined(_WIN32)
		return (void*)::GetProcAddress((HMODULE)fHandle, name);
#else
		return ::dlsym(fHandle, name);
#endif
	}

	// -----------------------------------------------------------------------
	std::string OwnModulePath()
	{
#if defined(_WIN32)
		HMODULE self = nullptr;
		if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
									 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								 reinterpret_cast<LPCWSTR>(&OwnModulePath), &self) == 0 ||
			self == nullptr)
			return "";
		std::wstring buf(MAX_PATH, L'\0');
		DWORD len = ::GetModuleFileNameW(self, buf.data(), (DWORD)buf.size());
		while (len == buf.size())
		{
			buf.resize(buf.size() * 2, L'\0');
			len = ::GetModuleFileNameW(self, buf.data(), (DWORD)buf.size());
		}
		if (len == 0)
			return "";
		buf.resize(len);
		return Narrow(buf);
#else
		Dl_info info{};
		if (::dladdr(reinterpret_cast<const void*>(&OwnModulePath), &info) == 0 ||
			info.dli_fname == nullptr)
			return "";
		return info.dli_fname;
#endif
	}

	std::string BundledPayloadPath()
	{
		const std::string self = OwnModulePath();
		if (self.empty())
			return "";
		// 名前は殻ごとに違う（stable / dev が同じ Plug-Ins に同居しても取り違えない）。
		const std::string name = payloadpath::FileNameFor(PLUGIN_VWR_ID);
#if defined(_WIN32)
		return payloadpath::WinPayloadPathFromModule(self, name);
#else
		return payloadpath::MacPayloadPathFromBinary(self, name);
#endif
	}

	PayloadStamp StampOf(const std::string& path)
	{
		PayloadStamp stamp;
		if (path.empty())
			return stamp;

		std::error_code ec;
		const std::uintmax_t size = std::filesystem::file_size(path, ec);
		if (ec)
			return stamp;
		const std::filesystem::file_time_type when = std::filesystem::last_write_time(path, ec);
		if (ec)
			return stamp;

		stamp.size = static_cast<unsigned long long>(size);
		// file_time_type の epoch は処理系依存なので、**絶対時刻としては使わない**。
		// ここで要るのは「前と違うか」だけなので、生の tick をそのまま秒に丸めて持つ。
		stamp.modified = static_cast<long long>(
			std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count());
		stamp.valid = true;
		return stamp;
	}

	std::string TempDirectory()
	{
		std::error_code ec;
		const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
		if (ec)
			return "";
		return dir.string();
	}

	char PathSeparator()
	{
#if defined(_WIN32)
		return '\\';
#else
		return '/';
#endif
	}

	bool CopyFileTo(const std::string& from, const std::string& to, std::string& error)
	{
		error.clear();
		std::error_code ec;
		// 上書きで写す（世代ごとに別名にするので通常は新規だが、作り直しでも通るように）。
		std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
		if (ec)
		{
			error = "複製に失敗しました（" + from + " → " + to + "）: " + ec.message();
			return false;
		}
		return true;
	}

	bool RemoveFileAt(const std::string& path, std::string& error)
	{
		error.clear();
		std::error_code ec;
		const bool removed = std::filesystem::remove(path, ec);
		if (ec)
		{
			error = "削除に失敗しました（" + path + "）: " + ec.message();
			return false;
		}
		if (!removed)
		{
			error = "そのファイルはありませんでした（" + path + "）";
			return false;
		}
		return true;
	}

	bool IsModuleStillLoaded(const std::string& path)
	{
		if (path.empty())
			return false;
#if defined(_WIN32)
		// 読み込まれていればハンドルが返る（参照数は増えない）。
		return ::GetModuleHandleW(Widen(path).c_str()) != nullptr;
#else
		// RTLD_NOLOAD: **すでに読み込まれているときだけ**ハンドルを返す。返ってきた
		// ハンドルは参照を 1 つ持つので、必ず dlclose して元へ戻す。
		void* handle = ::dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);
		if (handle == nullptr)
			return false;
		(void)::dlclose(handle);
		return true;
#endif
	}

	// -----------------------------------------------------------------------
	// Payload — 本体との付き合い 1 世代ぶん（PayloadHost.h）。
	// -----------------------------------------------------------------------
	namespace
	{
		// 世代の通し番号。**複製先の名前を毎回変える**ためのもの（Windows は読み込み中の
		// ファイルを置き換えられないので、使い回すと 2 回目が古いまま読まれる）。
		unsigned NextGeneration()
		{
			static unsigned sGeneration = 0;
			return ++sGeneration;
		}
	} // namespace

	Payload::~Payload()
	{
		this->unload();
	}

	bool Payload::load(void* callbacks, std::string& error)
	{
		error.clear();
		if (fLoaded)
			return true;
		if (callbacks == nullptr)
		{
			error = "SDK のコールバックが空です（殻の初期化が済んでいない）。";
			return false;
		}

		fSourcePath = BundledPayloadPath();
		if (fSourcePath.empty())
		{
			error = "本体（" + payloadpath::FileNameFor(PLUGIN_VWR_ID) +
					"）の置き場所を割り出せませんでした。";
			return false;
		}

		// **印は複製の前に取る。** 読み込んだ世代がどのファイルだったかを覚えるのが
		// 目的なので、途中で置き換えられても「読んだもの＝印」の対応が崩れない。
		fStamp = StampOf(fSourcePath);

		const std::string tempDir = TempDirectory();
		if (tempDir.empty())
		{
			error = "一時ディレクトリを取得できませんでした。";
			return false;
		}
		const std::string tag = std::to_string(NextGeneration());
		fTempPath = payloadpath::TempCopyPath(tempDir, tag, payloadpath::FileNameFor(PLUGIN_VWR_ID),
											  PathSeparator());

		// 複製してから読む（PayloadHost.h の「必ず複製してから読む」）。
		std::string why;
		if (!CopyFileTo(fSourcePath, fTempPath, why))
		{
			// **いちばん多いのは「本体だけが置かれていない」**——配布 zip には殻と本体の
			// 2 つが入っており、片方だけ置くと（あるいは殻しか入れ替えない古いアップデータ
			// で更新すると）この状態になる。**直し方を名指しで出す**: 症状は「取り込みが
			// 始まらない」だけなので、これが無いと何をすればよいか分からない。
			error = "本体（" + payloadpath::FileNameFor(PLUGIN_VWR_ID) +
					"）が見つかりません。\n"
					"配布 zip の中の同名ファイルを、プラグイン本体と同じ場所へ置いてください:\n" +
					fSourcePath + "\n（" + why + "）";
			fTempPath.clear();
			return false;
		}

		if (!fModule.open(fTempPath, why))
		{
			error = "本体を読み込めませんでした。\n" + why;
			this->unload();
			return false;
		}

		auto abiFn = reinterpret_cast<VwPayloadAbiVersionFn>(fModule.symbol(VW_PAYLOAD_SYM_ABI));
		auto initFn = reinterpret_cast<VwPayloadInitFn>(fModule.symbol(VW_PAYLOAD_SYM_INIT));
		auto infoFn = reinterpret_cast<VwPayloadInfoFn>(fModule.symbol(VW_PAYLOAD_SYM_INFO));
		fImportFn = reinterpret_cast<VwPayloadRunImportFn>(fModule.symbol(VW_PAYLOAD_SYM_IMPORT));
		fRecalcFn = reinterpret_cast<VwPayloadRecalculateFn>(fModule.symbol(VW_PAYLOAD_SYM_RECALC));
		fShutdownFn =
			reinterpret_cast<VwPayloadShutdownFn>(fModule.symbol(VW_PAYLOAD_SYM_SHUTDOWN));
		if (abiFn == nullptr || initFn == nullptr || infoFn == nullptr || fImportFn == nullptr ||
			fRecalcFn == nullptr || fShutdownFn == nullptr)
		{
			error = "本体の形が違います（必要な関数が見つかりません）。\n"
					"殻と本体の版が食い違っている可能性があります。";
			this->unload();
			return false;
		}

		// **版が違うなら呼ばない。** 殻と本体は独立に配られるので、ここでしか気付けない。
		const unsigned int abi = abiFn();
		if (abi != VW_PAYLOAD_ABI_VERSION)
		{
			error = "殻と本体の版が違います（本体=" + std::to_string(abi) +
					" 殻=" + std::to_string(VW_PAYLOAD_ABI_VERSION) +
					"）。\nプラグインごと入れ替えてください。";
			this->unload();
			return false;
		}

		// 素性は init の前でも取れる（読んだものが何かを先に言えるように）。
		VwPayloadInfo info{};
		info.size = static_cast<unsigned int>(sizeof(VwPayloadInfo));
		if (infoFn(&info) != kVwPayloadOk)
		{
			error = "本体の素性を取得できませんでした。";
			this->unload();
			return false;
		}
		// **その場で写す**（向こうの const char* は次の呼び出しまでしか生きていない）。
		fCommit = (info.commit != nullptr) ? info.commit : "";
		fBranch = (info.branch != nullptr) ? info.branch : "";

		// **これはメンバである（load のローカルではない）。** 古い本体は渡された
		// VwPayloadHost のポインタを持ち続けることがあり、その先がローカルだと load から
		// 戻った時点で腐る（PayloadAbi.h / PayloadHostHolder.h）。**降ろすまで生かす**の
		// が殻の側の歯止め。
		fHost = VwPayloadHost{};
		fHost.size = static_cast<unsigned int>(sizeof(VwPayloadHost));
		fHost.abiVersion = VW_PAYLOAD_ABI_VERSION;
		fHost.callbacks = callbacks;

		const int status = initFn(&fHost);
		if (status != kVwPayloadOk)
		{
			error = "本体を初期化できませんでした（コード " + std::to_string(status) + "）。";
			this->unload();
			return false;
		}
		fLoaded = true;
		return true;
	}

	bool Payload::runImport(std::string& error)
	{
		error.clear();
		if (!fLoaded || fImportFn == nullptr)
		{
			error = "本体が読み込まれていません。";
			return false;
		}
		const int status = fImportFn();
		if (status != kVwPayloadOk)
		{
			error = "取り込みを開始できませんでした（コード " + std::to_string(status) + "）。";
			return false;
		}
		return true;
	}

	bool Payload::recalculate(unsigned int kind, void* objectHandle, int& outEvent,
							  std::string& error)
	{
		error.clear();
		outEvent = 0;
		if (!fLoaded || fRecalcFn == nullptr)
		{
			error = "本体が読み込まれていません。";
			return false;
		}
		const int status = fRecalcFn(kind, objectHandle, &outEvent);
		if (status != kVwPayloadOk)
		{
			error = "リセットを実行できませんでした（コード " + std::to_string(status) + "）。";
			return false;
		}
		return true;
	}

	void Payload::unload()
	{
		// 順序が肝。① 本体に殻への参照を手放させる ② 降ろす ③ 複製を消す。
		if (fLoaded && fShutdownFn != nullptr)
			fShutdownFn();
		fLoaded = false;
		fImportFn = nullptr;
		fRecalcFn = nullptr;
		fShutdownFn = nullptr;
		fCommit.clear();
		fBranch.clear();
		fStamp = PayloadStamp{};
		// 本体は shutdown で手放したはず。殻の側も、渡していたものをここで捨てる
		// （降ろした後に触られても、少なくとも「腐った値」ではなくなる）。
		fHost = VwPayloadHost{};

		std::string ignored;
		(void)fModule.close(ignored);
		if (!fTempPath.empty())
		{
			// **降りたことを別の目で確かめてから消す。** dlclose が 0 を返しても消えて
			// いるとは限らないし、Windows では読み込み中のファイルは消せない。残しても
			// 世代ごとに名前が違うので次回に響かない。
			if (!IsModuleStillLoaded(fTempPath))
				(void)RemoveFileAt(fTempPath, ignored);
			fTempPath.clear();
		}
	}
} // namespace HomeskzIfcImport
