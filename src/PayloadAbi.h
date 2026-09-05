//
//	PayloadAbi.h
//
//	**殻（Vectorworks が読み込むプラグイン）と、本体（ペイロード）の間の唯一の約束事。**
//
//	【なぜ 2 つに割れているか】コンパイル済みプラグインは Vectorworks の**起動時にしか
//	読み込まれず**、読み込み済みのモジュールはプロセスが生きている間は差し替えられない
//	（[SDK リファレンス「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。
//	そこで**中身（解析・描画・PIO の作図）を Vectorworks が知らない別の動的モジュールへ
//	出し、殻が自分で読み込む**。すると中身の入れ替えは「ファイルを置き換えて読み直す」
//	だけになり、**アップデートに Vectorworks の再起動が要らない**（macOS で実測済み）。
//
//	    Vectorworks ──読み込む──▶ 殻（登録・更新・ペイロードの読み込み） … 起動時に 1 度きり
//	                                  │ dlopen / LoadLibrary
//	                                  ▼
//	                              本体（core / parse / draw）            … いつでも読み直せる
//
//	殻に残るのは「Vectorworks に番地を握られるもの」だけ——メニューと 2 つの PIO の**登録**
//	（SMenuDef / SParametricDef / パラメータ定義）と、自動アップデートである。**実処理は
//	すべて本体側**で、殻はこの ABI 越しに呼ぶ。
//
//	【なぜ C の ABI か】本体は**降ろして読み直す**ので、境界に C++ の型を置けない:
//
//	  * 例外を越えさせない（越えた先のモジュールが消えていれば巻き戻せない）。
//	  * std::string / std::vector を跨いで渡さない（**殻と本体は別々にビルドされ、別々に
//	    配られる**——アロケータや実装の一致に頼れない）。
//	  * 仮想関数テーブルを持つオブジェクトを殻へ残さない（降ろした瞬間に vtable が消える）。
//
//	【渡す構造体の寿命】**VwPayloadHost は本体がその場で写す**（PayloadHostHolder.h）。
//	殻がどこにそれを置いたか（ローカルか、メンバか）は本体からは分からないので、ポインタを
//	持ち続けてはならない。殻の側も**降ろすまで生かす**（二重の歯止め。片方だけ古い組み合わせ
//	が実際に起きうる＝ホットリロードの目的そのもの）。これを落とすと、本体が腐ったポインタ
//	から関数ポインタを読んで**スタックの番地へ分岐し、Vectorworks ごと落ちる**——SDK
//	リファレンス側で実際に落としてある（同 Findings「殻の記憶域を本体に持たせると落ちる」）。
//
//	【返る文字列の寿命】本体が返す `const char*` は、**次に本体を呼ぶまで**か**降ろすまで**
//	しか生きていない。殻は受け取ったその場で std::string へ写すこと。
//
//	【版が食い違ったら呼ばない】殻と本体は独立に配られるので、食い違いは実行時にしか検出
//	できない。`vw_payload_abi_version()` が殻の VW_PAYLOAD_ABI_VERSION と一致しない本体は
//	**読み込んだだけで捨てる**（殻は「プラグインごと入れ替えてください」と案内する）。
//	**境界の形を変えたら必ず版を上げること。**
//
//	【SDK を include しない】この 1 ファイルだけは SDK にもプラットフォームにも依存しない。
//	殻と本体の両方が include するので、依存を持ち込むと境界の意味が薄れる。CallBackPtr も
//	MCObjectHandle も void* として渡す（実体は SDK の型）。
//
//	使う側:
//	  * 殻   … src/PayloadHost.h（読み込み・呼び出し・アンロード）、src/PayloadSession.h
//	  * 本体 … src/payload/PayloadMain.cpp（下の関数を export する）
//

#pragma once

#include <cstddef>

// 境界の版。**形を変えたら上げる。**
//   1 … 取り込みコマンドと 2 つの PIO のリセットを載せた最初の形
//   2 … 実機フィードバックの往復（M23）。殻の ID と同梱スクリプトの実行を殻から借り、
//       取り込みは「もう 1 周するか」を返すようになった
#define VW_PAYLOAD_ABI_VERSION 2u

// 本体側の export 指定。Windows は明示しないと DLL の外から見えない。
#if defined(_WIN32)
#	define VW_PAYLOAD_EXPORT extern "C" __declspec(dllexport)
#else
#	define VW_PAYLOAD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

extern "C"
{
	// -----------------------------------------------------------------------
	// 殻が本体へ渡すもの。**本体はこれ以外に殻を知らない。**
	struct VwPayloadHost
	{
		// sizeof(VwPayloadHost)。版が食い違ったときに「短い構造体を長いつもりで読む」
		// 事故を防ぐ（abiVersion と二重の歯止め）。
		unsigned int size;
		// 殻がコンパイルされた VW_PAYLOAD_ABI_VERSION。
		unsigned int abiVersion;

		// **SDK の CallBackPtr。** 本体はこれを GS_InitializeVCOM へ渡して、自分の側の
		// gSDK / gCBP / gVWMM を埋める（それらは静的ライブラリが持つ**モジュールごとの**
		// グローバルなので、読み込んだだけでは空のまま）。
		void* callbacks;

		// **いま動いている殻の ID**（殻にコンパイルされた VW_SHELL_ID）。本体はこれを、
		// 新しく入れたビルドの殻の ID と突き合わせて「再起動が要るか／本体の読み直しで
		// 済むか」を決める（src/UpdaterParse.h の NeedsRestartAfterInstall）。**本体は
		// 自分の殻の ID を知らない**——殻にしかコンパイルされていないので、ここで借りる。
		// 文字列は殻が所有し、降ろすまで生きている（本体は受け取った時点で写す）。
		const char* shellId;

		// **同梱スクリプトを 1 本走らせて標準出力を受け取る。** 本体は自分の在り処から
		// 同梱物へたどり着けない——読み込まれるのは**一時ディレクトリへ写した複製**なので
		// （PayloadHost.h「必ず複製してから読む」）、dladdr / GetModuleFileName が返すのは
		// バンドルの外の道である。だから殻の道具を借りる。
		//
		//   scriptName … 拡張子を除いた名前（"vw-update" / "vw-feedback"）。**どちらの
		//                拡張子を付けるかは殻が決める**（mac は .sh、Windows は .ps1）。
		//   args/argc  … スクリプトへ渡す引数（UTF-8）。
		//   out        … 標準出力（UTF-8）。**殻が所有し、次にこの関数を呼ぶまで有効**——
		//                本体は受け取ったその場で写すこと（返る文字列の寿命は他と同じ）。
		//
		// 戻り値は 0（kVwPayloadOk）で成功、それ以外は起動できなかったということ。
		int (*runBundledScript)(const char* scriptName, const char* const* args, unsigned int argc,
								const char** out);
	};

	// -----------------------------------------------------------------------
	// 本体自身の素性。「いま動いている本体はどのビルドか」を殻が言えるように
	//（アップデート後にホットリロードが効いたことを確かめる唯一の手段でもある）。
	struct VwPayloadInfo
	{
		unsigned int size;
		const char* commit; // 短縮 sha（"local" のこともある）
		const char* branch; //
	};

	// -----------------------------------------------------------------------
	// リセットを頼む PIO の種別。**殻の PIO ごとに 1 つ**（殻は登録だけを持ち、絵は
	// 本体が描く）。値は ABI の一部なので**並べ替えない・詰め直さない**。
	enum VwPayloadPioKind
	{
		kVwPayloadPioColumnMark = 0, // 柱・小屋束の記号
		kVwPayloadPioShearWall = 1,	 // 耐力壁（筋かい・面材）
	};

	// -----------------------------------------------------------------------
	// 本体が export する関数の名前（dlsym / GetProcAddress で引く）。綴りを 1 か所に
	// 持つ——殻と本体で食い違うと「見つからない」としか出ない。
#define VW_PAYLOAD_SYM_ABI "vw_payload_abi_version"
#define VW_PAYLOAD_SYM_INIT "vw_payload_init"
#define VW_PAYLOAD_SYM_INFO "vw_payload_info"
#define VW_PAYLOAD_SYM_IMPORT "vw_payload_run_import"
#define VW_PAYLOAD_SYM_RECALC "vw_payload_recalculate"
#define VW_PAYLOAD_SYM_SHUTDOWN "vw_payload_shutdown"

	// その型。
	using VwPayloadAbiVersionFn = unsigned int (*)();
	using VwPayloadInitFn = int (*)(const VwPayloadHost*);
	using VwPayloadInfoFn = int (*)(VwPayloadInfo*);
	// 取り込みコマンド 1 周ぶん。**outAgain に 0 以外が入って戻ったら、殻はもう一度
	// 呼ぶ**（実機フィードバックの往復。src/Extensions/ExtMenu.cpp）。周と周のあいだに
	// 殻が本体を手放すことで、**新しく入った本体がその場で読み直される**——降ろせるのは
	// 本体のコードがスタックに 1 つも無いときだけなので、この形（戻ってから殻が回す）で
	// なければホットリロードは成立しない（src/PayloadSession.h）。
	using VwPayloadRunImportFn = int (*)(int* outAgain);
	using VwPayloadRecalculateFn = int (*)(unsigned int, void*, int*);
	using VwPayloadShutdownFn = void (*)();

	// -----------------------------------------------------------------------
	// 戻り値。**0 が成功**で、それ以外は理由を表す（例外は越えさせないので、失敗は
	// すべてこの整数で返る）。
	enum VwPayloadStatus
	{
		kVwPayloadOk = 0,
		kVwPayloadErrAbi = 1, // 版か構造体の大きさが合わない
		kVwPayloadErrHost = 2, // 殻から渡されたものが足りない（callbacks が空 等）
		kVwPayloadErrVcom = 3,		// GS_InitializeVCOM に失敗した / gSDK が空のまま
		kVwPayloadErrNotInit = 4,	// init が済んでいない
		kVwPayloadErrUnknownId = 5, // 知らない PIO 種別
		kVwPayloadErrException = 6, // 中で例外が出た（境界の手前で握り潰した）
	};
} // extern "C"
