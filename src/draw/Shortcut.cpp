//
//	draw/Shortcut.cpp
//
//	ショートカット／エイリアスの解決（意図と制約は draw/Shortcut.h）。**プラットフォーム
//	ごとに丸ごと別実装**で、共通部分は「拡張子を見る」ところしか無い。
//
//	**UTF-8 との往復をここにも書いている。** 殻にも同じ変換がある（src/PayloadHost.cpp の
//	Widen / Narrow）が、あちらは別のモジュールの中の非公開の小物で、本体からは呼べない
//	——境界を跨げるのは C の ABI に載せたものだけである（src/PayloadAbi.h）。
//

#include "PluginPrefix.h"
#include "draw/Shortcut.h"

#include <cstddef>
#include <string>

#if defined(_WINDOWS)
#	include <objbase.h>
#	include <shlobj.h>
#elif defined(__APPLE__)
#	include <CoreFoundation/CoreFoundation.h>
#endif

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 拡張子が `.lnk` か（大小を問わない）。**Windows でしか意味を持たない**ので、
		// mac 側からは呼ばない。
		bool HasLnkExtension(const std::string& path)
		{
			if (path.size() < 4)
				return false;
			const std::string tail = path.substr(path.size() - 4);
			return (tail[0] == '.') && (tail[1] == 'l' || tail[1] == 'L') &&
				   (tail[2] == 'n' || tail[2] == 'N') && (tail[3] == 'k' || tail[3] == 'K');
		}

#if defined(_WINDOWS)
		// UTF-8 → UTF-16（Win32 の W 系 API 用）。
		std::wstring Widen(const std::string& text)
		{
			if (text.empty())
				return {};
			const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
												   static_cast<int>(text.size()), nullptr, 0);
			if (size <= 0)
				return {};
			std::wstring wide(static_cast<std::size_t>(size), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
								  wide.data(), size);
			return wide;
		}

		// UTF-16 → UTF-8。
		std::string Narrow(const wchar_t* text)
		{
			if (text == nullptr || text[0] == L'\0')
				return {};
			const int size =
				::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
			if (size <= 1)
				return {};
			// -1 を渡したので終端の NUL も数に入っている。文字列には含めない。
			std::string narrow(static_cast<std::size_t>(size - 1), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size, nullptr, nullptr);
			return narrow;
		}

		// **COM を使っている間だけ初期化する。** Vectorworks 自身が初期化済みの
		// スレッドから呼ばれることもあるので、戻り値で「後始末が要るか」を分ける
		// （`RPC_E_CHANGED_MODE` は負＝「別のモードで既に初期化済み」で、それでも使えるが
		// こちらが `CoUninitialize` してはならない。`S_FALSE` は 0 以上＝こちらが数えた
		// 1 回ぶんなので、釣り合いを取って解放する）。
		class ComScope
		{
		public:
			ComScope() : fOwned(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) >= 0) {}
			~ComScope()
			{
				if (fOwned)
					::CoUninitialize();
			}
			ComScope(const ComScope&) = delete;
			ComScope& operator=(const ComScope&) = delete;

		private:
			bool fOwned;
		};

		// **COM の成否は `HRESULT` を直に見る**（`SUCCEEDED` / `FAILED` を使わない）。
		// あのマクロは C キャストを挟んで展開されるので、静的解析の報告がマクロの中を
		// 指してしまい、こちらのコードのどこが悪いのか読めなくなる（src/PayloadHost.cpp の
		// 「標準ライブラリの中の話を黙らせるより、OS の API を直に叩くほうが素直」と同じ
		// 考え方）。負なら失敗、というのが `HRESULT` の約束である。
		bool Failed(HRESULT hr)
		{
			return hr < 0;
		}

		// `Resolve` へ渡す旗。**`SLR_FLAGS` の `|` を使わない**——Windows SDK の
		// `DEFINE_ENUM_FLAG_OPERATORS` が定義する `operator|` は、組み合わせた値を
		// もとの enum へ戻すので、静的解析が「その enum の値域に無い」と報告する
		// （tidy-windows で実測。`std::filesystem` の `file_size` と同じ筋の偽陽性）。
		// `Resolve` の引数は `DWORD` なので、**こちらで数として組み立てれば済む**。
		//
		// **UI を出させない**（`SLR_NO_UI`）——無人で何十件も回る途中で「リンク先が
		// 見つかりません」が出ると、そこで止まる。探し回らせない（`SLR_NOSEARCH` /
		// `SLR_NOTRACK`）のも同じ理由で、これが無いと 1 件あたり数秒待つことがある。
		constexpr DWORD kResolveFlags =
			static_cast<DWORD>(SLR_NO_UI) | static_cast<DWORD>(SLR_NOUPDATE) |
			static_cast<DWORD>(SLR_NOSEARCH) | static_cast<DWORD>(SLR_NOTRACK);

		// `GetPath` へ渡す受け皿の大きさ（文字数）。**`MAX_PATH` の掛け算で書かない**
		// ——`int` で掛けた結果を `size_type` へ広げる形になり、静的解析がそれを咎める
		// （tidy-windows で実測）。`MAX_PATH` は 260 なので、その数倍を定数で置く。
		constexpr std::size_t kPathBufferChars = 1024;

		// `.lnk` の指す先を読む。
		bool ResolveWindowsShortcut(const std::string& path, std::string& target)
		{
			const ComScope com;
			IShellLinkW* link = nullptr;
			HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
											IID_IShellLinkW, reinterpret_cast<void**>(&link));
			if (Failed(hr) || link == nullptr)
				return false;

			bool resolved = false;
			IPersistFile* file = nullptr;
			hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
			if (!Failed(hr) && file != nullptr)
			{
				const std::wstring wide = Widen(path);
				if (!Failed(file->Load(wide.c_str(), STGM_READ)))
				{
					// 戻り値は見ない——**解決できなくても、保存されているパスは読める**。
					// 読めた先が実在するかは、呼び出し側（core/FixtureScan）が確かめる。
					(void)link->Resolve(nullptr, kResolveFlags);
					std::wstring buffer(kPathBufferChars, L'\0');
					WIN32_FIND_DATAW found{};
					if (!Failed(link->GetPath(buffer.data(), static_cast<int>(buffer.size()),
											  &found, SLGP_UNCPRIORITY)))
					{
						target = Narrow(buffer.c_str());
						resolved = !target.empty();
					}
				}
				file->Release();
			}
			link->Release();
			return resolved;
		}
#endif // _WINDOWS

#if defined(__APPLE__)
		// CFString を作る（作れなければ nullptr。呼び出し側が CFRelease する）。
		CFStringRef MakeString(const std::string& text)
		{
			return CFStringCreateWithBytes(kCFAllocatorDefault,
										   reinterpret_cast<const UInt8*>(text.data()),
										   static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8,
										   /*isExternalRepresentation*/ false);
		}

		// CFURL のファイルシステムパスを UTF-8 で取り出す。
		std::string PathOf(CFURLRef url)
		{
			CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
			if (path == nullptr)
				return {};
			const CFIndex length = CFStringGetLength(path);
			const CFIndex capacity =
				CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
			std::string text(static_cast<std::size_t>(capacity), '\0');
			const Boolean ok =
				CFStringGetCString(path, text.data(), capacity, kCFStringEncodingUTF8);
			CFRelease(path);
			if (!ok)
				return {};
			text.resize(std::char_traits<char>::length(text.c_str()));
			return text;
		}

		// **Finder のエイリアスを解決する。** エイリアスは拡張子が元のまま（`.ifc` に
		// 見える）なので、名前では素のファイルと見分けられない——だから「エイリアスか」を
		// OS に訊くところから始める（`kCFURLIsAliasFileKey`）。シンボリックリンクにも
		// true が返るが、解決した先は同じなので害は無い。
		//
		// 解決は**UI もマウントも起こさない**指定で行う（無人で回る途中に何も出さない。
		// 外付けディスクを勝手に繋ぎに行かせない）。
		bool ResolveMacAlias(const std::string& path, std::string& target)
		{
			CFStringRef cfPath = MakeString(path);
			if (cfPath == nullptr)
				return false;
			CFURLRef url = CFURLCreateWithFileSystemPath(
				kCFAllocatorDefault, cfPath, kCFURLPOSIXPathStyle, /*isDirectory*/ false);
			CFRelease(cfPath);
			if (url == nullptr)
				return false;

			bool resolved = false;
			CFBooleanRef isAlias = nullptr;
			// **多段ポインタを `void*` へ渡すのは、この API の形そのものである。**
			// `&isAlias` は `CFBooleanRef*`（＝`const __CFBoolean**`）で、
			// `CFURLCopyResourcePropertyForKey` は結果の置き場所を `void*` で受け取る
			// ——CoreFoundation の「値を写して返す」API は一様にこの形なので、呼ぶ側に
			// 避ける書き方が無い（受け皿を `CFTypeRef` にしても `const void**` で
			// 多段のままである）。
			//
			// clang-tidy の bugprone-multi-level-implicit-pointer-conversion は「明示
			// キャストを使え」と言うが、**その明示キャストも同じように咎める**（tidy-mac で
			// 実測。`static_cast<void*>` にしても消えなかった）ので、コードの側では満たし
			// ようがない。**だから 1 か所だけ黙らせる**——`.clang-tidy` で全体から外すと、
			// 本当に危ない多段変換まで見逃すことになる。意図は `static_cast` が示す。
			// NOLINTBEGIN(bugprone-multi-level-implicit-pointer-conversion)
			if (CFURLCopyResourcePropertyForKey(url, kCFURLIsAliasFileKey,
												static_cast<void*>(&isAlias), nullptr) &&
				isAlias != nullptr)
			// NOLINTEND(bugprone-multi-level-implicit-pointer-conversion)
			{
				if (CFBooleanGetValue(isAlias))
				{
					CFDataRef bookmark =
						CFURLCreateBookmarkDataFromFile(kCFAllocatorDefault, url, nullptr);
					if (bookmark != nullptr)
					{
						Boolean stale = false;
						CFURLRef targetUrl = CFURLCreateByResolvingBookmarkData(
							kCFAllocatorDefault, bookmark,
							kCFBookmarkResolutionWithoutUIMask |
								kCFBookmarkResolutionWithoutMountingMask,
							nullptr, nullptr, &stale, nullptr);
						if (targetUrl != nullptr)
						{
							target = PathOf(targetUrl);
							resolved = !target.empty();
							CFRelease(targetUrl);
						}
						CFRelease(bookmark);
					}
				}
				CFRelease(isAlias);
			}
			CFRelease(url);
			return resolved;
		}
#endif // __APPLE__
	} // namespace

	// -----------------------------------------------------------------------
	bool resolveShortcut(const std::string& path, std::string& target)
	{
		target.clear();
		if (path.empty())
			return false;
#if defined(_WINDOWS)
		// **`.lnk` 以外は見ない。** Windows にエイリアスは無いので、ここで false を返せば
		// 呼び出し側は素のファイルとして扱う（`.ifc` 1 件につき 1 回来るが、拡張子を見て
		// すぐ帰るだけなので走査の速さに響かない）。
		if (!HasLnkExtension(path))
			return false;
		return ResolveWindowsShortcut(path, target);
#elif defined(__APPLE__)
		// mac の `.lnk` は Windows で作られたただのファイル。解決する手立ては無い。
		if (HasLnkExtension(path))
			return false;
		return ResolveMacAlias(path, target);
#else
		(void)HasLnkExtension(path);
		return false;
#endif
	}
} // namespace HomeskzIfcImport::draw
