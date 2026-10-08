//
//	PayloadHost.h
//
//	**殻の側。** 本体（ペイロード）を自分で読み込み、C の ABI（PayloadAbi.h）で呼び出し、
//	アンロードするまでを扱う。Vectorworks はこのモジュールの存在を知らない——だから**入れ替えに
//	Vectorworks の再起動が要らない**（macOS で実測済み。[SDK リファレンス「プラグイン
//	モジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。
//
//	【いつ読み込み、いつアンロードするか】**読み込んだら保持したままにする**（アンロード
//	するのは入れ替えのときだけ）。**入れ替えの判定だけを入口ごとに行う**
//	（src/PayloadSession.h。同梱ファイルの大きさ・更新時刻が変わっていたらアンロードして
//	再読み込みする）。
//	SDK リファレンスの実機確認プラグインは「メニューを開くたびに読み込んで、終わったら
//	アンロードする」作りだが、本プラグインは**PIO のリセットが同じ本体を使う**——取り込み
//	直後は数百個の記号・耐力壁がリセットされるので、1 回 0.3〜0.4 秒かかる読み込みを毎回
//	行うと実用的な速度にならない。
//
//	【必ず複製してから読む】同梱のファイルを直接は読み込まない。世代ごとに一時ディレクトリへ
//	複製して、その複製を読み込む。理由は 2 つ:
//	  * **Windows は読み込み中の DLL を消せない・置き換えられない。** 直接読むと、
//	    Vectorworks を動かしたままアップデートできなくなる（＝この仕組みの意味が無くなる）。
//	  * 同じパスを使い回すと、OS のキャッシュで「置き換えたのに古いまま」を検出できないことが
//	    ある。
//
//	【SDK に依存しない】この 2 ファイルは SDK の型を使わない（プリコンパイルヘッダ経由で
//	宣言は入るが使わない）。パスの組み立ては純粋な関数に分けてあり、tests/ から SDK 抜きで
//	確かめられる（自動アップデートの UpdaterParse.h と同じ作法）。
//

#pragma once

#include "PayloadAbi.h"

#include <string>

namespace HomeskzIfcImport
{
	// -----------------------------------------------------------------------
	// 純粋な部分（パスの組み立て）。**プラットフォーム依存の呼び出しを含まない**ので、
	// そのまま単体テストできる（tests/PayloadPathTests.cpp）。
	namespace payloadpath
	{
		// 配布する本体のファイル名。拡張子を .vwpayload にしてあるのは、**Vectorworks に
		// プラグインとして検出させないため**（.vlb / .vwlibrary だと Plug-Ins フォルダの
		// 走査で検出され、殻と二重に読み込まれてしまう）。名前は殻ごとに違う
		// （stable / dev が同じ Plug-Ins に同居しても取り違えない）。
		inline std::string FileNameFor(const std::string& pluginName)
		{
			return pluginName + ".vwpayload";
		}

		// macOS: .../<Plug-Ins>/<name>.vwlibrary/Contents/MacOS/<name>
		//     → .../<Plug-Ins>/<fileName>
		//
		// **バンドルの中には置かない。** mac のバンドルは署名がリソースまで対象にするので、
		// Contents/Resources のファイルを差し替えると署名が壊れる（次の起動で読み込め
		// なくなりうる）。隣に置けば、本体を何度置き換えても殻の署名に触れない。
		inline std::string MacPayloadPathFromBinary(const std::string& binaryPath,
													const std::string& fileName)
		{
			const std::string::size_type at = binaryPath.rfind("/Contents/MacOS/");
			if (at == std::string::npos)
				return "";
			const std::string bundle = binaryPath.substr(0, at);
			const std::string::size_type slash = bundle.rfind('/');
			if (slash == std::string::npos)
				return "";
			return bundle.substr(0, slash + 1) + fileName;
		}

		// Windows: .../<Plug-Ins>/<name>.vlb → .../<Plug-Ins>/<fileName>
		inline std::string WinPayloadPathFromModule(const std::string& modulePath,
													const std::string& fileName)
		{
			const std::string::size_type slash = modulePath.find_last_of("\\/");
			if (slash == std::string::npos)
				return "";
			return modulePath.substr(0, slash + 1) + fileName;
		}

		// 読み込むのは**同梱物そのものではなく一時ディレクトリへ複製したもの**（上記）。
		// tag には世代を区別できる文字列を渡す（同じ名前を使い回さないことが要点）。
		inline std::string TempCopyPath(const std::string& tempDir, const std::string& tag,
										const std::string& fileName, char separator)
		{
			std::string dir = tempDir;
			if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
				dir.pop_back();
			return dir + separator + "vwpayload-" + tag + "-" + fileName;
		}
	} // namespace payloadpath

	// -----------------------------------------------------------------------
	// 同梱ファイルの「版が変わったか」を判定する印。**中身のハッシュは取らない**——入口の
	// たびに数 MB を読むのは重いし、アップデートは必ずファイルを書き替えるので大きさと
	// 更新時刻で足りる（検出し損ねても次の入口で検出されるだけであり、誤って古いまま
	// 動かし続けることはあっても壊れはしない）。
	struct PayloadStamp
	{
		unsigned long long size = 0;
		long long modified = 0; // epoch 秒。取れなければ 0
		bool valid = false;

		bool operator==(const PayloadStamp& other) const
		{
			return valid && other.valid && size == other.size && modified == other.modified;
		}
		bool operator!=(const PayloadStamp& other) const
		{
			return !(*this == other);
		}
	};

	// -----------------------------------------------------------------------
	// 読み込んだモジュール 1 つ（薄いラッパー）。**デストラクタではアンロードしない**（下記）。
	class PayloadModule
	{
	public:
		PayloadModule() = default;
		// **デストラクタではアンロードしない。** アンロード（close）は明示的な操作で、失敗の
		// 理由を呼び出し側へ返す必要がある。open したまま破棄された場合はモジュールが
		// プロセスに残るだけで、壊れはしない。
		~PayloadModule() = default;

		PayloadModule(const PayloadModule&) = delete;
		PayloadModule& operator=(const PayloadModule&) = delete;

		// 読み込む。失敗したら false ＋ OS が返す理由（dlerror / エラーコード）。
		bool open(const std::string& path, std::string& error);

		// アンロードする。**呼ぶ前に、このモジュールのコードがスタックに無いことを確かめること。**
		bool close(std::string& error);

		bool isOpen() const
		{
			return fHandle != nullptr;
		}
		const std::string& path() const
		{
			return fPath;
		}

		// export された関数を取得する。無ければ nullptr。
		void* symbol(const char* name) const;

	private:
		void* fHandle = nullptr;
		std::string fPath;
	};

	// -----------------------------------------------------------------------
	// **本体の 1 世代ぶんの管理。** 複製 → 読み込み → init までを load が行い、
	// unload がアンロードして複製を片付ける。入れ替えの判断は PayloadSession が持つ。
	class Payload
	{
	public:
		Payload() = default;
		~Payload();

		Payload(const Payload&) = delete;
		Payload& operator=(const Payload&) = delete;

		// 読み込んで使える状態にする。callbacks は殻が plugin_module_main で受け取った
		// SDK の CallBackPtr。失敗したら false ＋ 利用者に表示する理由。
		bool load(void* callbacks, std::string& error);

		// アンロードして複製を削除する（何度呼んでもよい）。
		void unload();

		bool isLoaded() const
		{
			return fLoaded;
		}

		// 読み込んだ本体の素性（ホットリロードが機能したかを示すのに使う）。
		const std::string& commit() const
		{
			return fCommit;
		}
		const std::string& branch() const
		{
			return fBranch;
		}
		// 同梱物の在り処（見つからなかったときの案内に使う）。
		const std::string& sourcePath() const
		{
			return fSourcePath;
		}
		// 読み込んだ時点の同梱物の印（入れ替えの判定に使う）。
		const PayloadStamp& stamp() const
		{
			return fStamp;
		}

		// **本番の取り込みコマンド**を 1 周ぶん走らせる（ファイル選択から結果ダイアログ
		// まで本体が行う）。呼べなかったときだけ false。M25 で往復の処理を取り除いたので、
		// 返す情報はもう無い（src/draw/ImportCommand.h）。
		bool runImport(std::string& error);

		// **MCP ブリッジの受け付けを 1 回**（M30。src/draw/McpBridge.h）。待たずに戻る。
		// shellReport は殻が済ませた要求の結末（無ければ空。PayloadAbi.h の
		// VwPayloadMcpServeFn）。out にはパレットに表示する状態の JSON が入る（本体が返した
		// 文字列は**ここで複製する**）。呼べなかったときだけ false。
		bool mcpServe(const std::string& shellReport, std::string& out, std::string& error);

		// PIO のリセット（再描画）を本体に行わせる。outEvent には EObjectEvent の値が入る。
		bool recalculate(unsigned int kind, void* objectHandle, int& outEvent, std::string& error);

	private:
		// **本体へ渡した VwPayloadHost の実体。** load のローカルにしてはならない——
		// 本体がこのポインタを持ち続けても壊れないよう、**アンロードするまで保持する**
		// （PayloadAbi.h の「渡す構造体の寿命」）。ここに置くのがその保証。
		VwPayloadHost fHost{};
		PayloadModule fModule;
		std::string fTempPath;
		std::string fSourcePath;
		std::string fCommit;
		std::string fBranch;
		PayloadStamp fStamp;
		VwPayloadRunImportFn fImportFn = nullptr;
		VwPayloadMcpServeFn fMcpServeFn = nullptr;
		VwPayloadRecalculateFn fRecalcFn = nullptr;
		VwPayloadShutdownFn fShutdownFn = nullptr;
		bool fLoaded = false;
	};

	// -----------------------------------------------------------------------
	// プラットフォーム依存の補助関数（実装は PayloadHost.cpp）。

	// 自分（Vectorworks が読み込んだ殻）のバイナリの絶対パス。
	std::string OwnModulePath();

	// 同梱されている本体の絶対パス（殻の隣）。見つからなければ空。
	std::string BundledPayloadPath();

	// そのファイルの印（大きさ・更新時刻）。読めなければ valid=false。
	PayloadStamp StampOf(const std::string& path);

	// 一時ディレクトリと、このプラットフォームのパス区切り。
	std::string TempDirectory();
	char PathSeparator();

	// ファイルの複製と削除（失敗したら false ＋ 理由）。
	bool CopyFileTo(const std::string& from, const std::string& to, std::string& error);
	bool RemoveFileAt(const std::string& path, std::string& error);

	// そのパスのモジュールが**まだプロセスに残っているか**（アンロードできたかの確認）。
	bool IsModuleStillLoaded(const std::string& path);
} // namespace HomeskzIfcImport
