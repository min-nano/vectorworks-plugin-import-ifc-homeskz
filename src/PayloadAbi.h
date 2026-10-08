//
//	PayloadAbi.h
//
//	**殻（Vectorworks が読み込むプラグイン）と、本体（ペイロード）の間の唯一の約束事。**
//
//	【なぜ 2 つに分けているか】コンパイル済みプラグインは Vectorworks の**起動時にしか
//	読み込まれず**、読み込み済みのモジュールはプロセスが生きている間は差し替えられない
//	（[SDK リファレンス「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。
//	そこで**中身（解析・描画・PIO の作図）を Vectorworks が知らない別の動的モジュールへ
//	出し、殻が自分で読み込む**。すると中身の入れ替えは「ファイルを置き換えて再読み込みする」
//	だけになり、**アップデートに Vectorworks の再起動が要らない**（macOS で実測済み）。
//
//	    Vectorworks ──読み込む──▶ 殻（登録・更新・ペイロードの読み込み） … 起動時に 1 度きり
//	                                  │ dlopen / LoadLibrary
//	                                  ▼
//	                              本体（core / parse / draw）            … いつでも再読み込みできる
//
//	殻に残るのは「Vectorworks に番地を保持されるもの」だけ——メニューと PIO の**登録**
//	（SMenuDef / SParametricDef / パラメータ定義）と、自動アップデートである。**実処理は
//	すべて本体側**で、殻はこの ABI 越しに呼ぶ。
//
//	【なぜ C の ABI か】本体は**アンロードして再読み込みする**ので、境界に C++ の型を置けない:
//
//	  * 例外を越えさせない（越えた先のモジュールが消えていれば巻き戻せない）。
//	  * std::string / std::vector を境界を越えて受け渡さない（**殻と本体は別々にビルドされ、別々に
//	    配られる**——アロケータや実装の一致に頼れない）。
//	  * 仮想関数テーブルを持つオブジェクトを殻へ残さない（アンロードした瞬間に vtable が消える）。
//
//	【渡す構造体の寿命】**VwPayloadHost は本体がその場で複製する**（PayloadHostHolder.h）。
//	殻がどこにそれを置いたか（ローカルか、メンバか）は本体からは分からないので、ポインタを
//	持ち続けてはならない。殻の側も**アンロードするまで保持する**（二重の歯止め。片方だけ古い
//	組み合わせが実際に起きうる＝ホットリロードの目的そのもの）。これを怠ると、本体が無効に
//	なったポインタから関数ポインタを読んで**スタックの番地へ分岐し、Vectorworks ごと異常
//	終了する**——SDK リファレンス側で実際に再現してある（同 Findings「殻の記憶域を本体に
//	持たせると落ちる」）。
//
//	【返る文字列の寿命】本体が返す `const char*` は、**次に本体を呼ぶまで**か**アンロード
//	するまで**しか有効でない。殻は受け取ったその場で std::string へ複製すること。
//
//	【版が食い違ったら呼ばない】殻と本体は独立に配られるので、食い違いは実行時にしか検出
//	できない。`vw_payload_abi_version()` が殻の VW_PAYLOAD_ABI_VERSION と一致しない本体は
//	**読み込んだだけで破棄する**（殻は「プラグインごと入れ替えてください」と案内する）。
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
//   2 … 実機フィードバックの往復（M23）。同梱スクリプトの実行を殻から借り、取り込みは
//       「次は更新を尋ねずに入れてよいか」を返すようになった
//   3 … MCP ブリッジ（vw_payload_run_mcp_bridge）を追加した
//   4 … モードレスの往復（M24）。殻のパレットが本体へ「往復の記憶」を尋ね（loop_status）、
//       止めたことを伝える（loop_end）関数が増えた
//   5 … 実機テストを本番の取り込みから分けた（M25）。往復は run_test が持ち、run_import は
//       「次は更新を尋ねずに入れてよいか」を返さなくなった（本番の経路から往復が消えた）
//   6 … MCP ブリッジを常駐にした（M30）。「止められるまで戻らない」run_mcp_bridge を削除し、
//       殻のパレットの時計が 1 回ずつ呼ぶ mcp_serve に替えた
//   7 … PR への自動投稿と自動の入れ替えを削除し、MCP から更新・再起動を要求できるように
//       した（M38）。loop_status / loop_end を削除し、run_test は outActive を持たなくなり、
//       mcp_serve は殻が済ませた頼みごとの結末（shellReport）を受け取るようになった
//   8 … メニュー「実機テストを実行…」を削除した（M43）。run_test を削除し、実機テストは
//       MCP の `vw_run_test`（mcp_serve の中）だけになった
#define VW_PAYLOAD_ABI_VERSION 8u

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
		// 誤りを防ぐ（abiVersion と二重の歯止め）。
		unsigned int size;
		// 殻がコンパイルされた VW_PAYLOAD_ABI_VERSION。
		unsigned int abiVersion;

		// **SDK の CallBackPtr。** 本体はこれを GS_InitializeVCOM へ渡して、自分の側の
		// gSDK / gCBP / gVWMM を埋める（それらは静的ライブラリが持つ**モジュールごとの**
		// グローバルなので、読み込んだだけでは空のまま）。
		void* callbacks;

		// **同梱スクリプトを 1 本走らせて標準出力を受け取る。** 本体は自分の在り処から
		// 同梱物の場所を特定できない——読み込まれるのは**一時ディレクトリへ複製したもの**なので
		// （PayloadHost.h「必ず複製してから読む」）、dladdr / GetModuleFileName が返すのは
		// バンドルの外のパスである。だから殻の機能を借りる。
		//
		//   scriptName … 拡張子を除いた名前（"vw-update"）。**どちらの
		//                拡張子を付けるかは殻が決める**（mac は .sh、Windows は .ps1）。
		//   args/argc  … スクリプトへ渡す引数（UTF-8）。
		//   out        … 標準出力（UTF-8）。**殻が所有し、次にこの関数を呼ぶまで有効**——
		//                本体は受け取ったその場で複製すること（返る文字列の寿命は他と同じ）。
		//
		// 戻り値は 0（kVwPayloadOk）で成功、それ以外は起動できなかったということ。
		int (*runBundledScript)(const char* scriptName, const char* const* args, unsigned int argc,
								const char** out);
	};

	// -----------------------------------------------------------------------
	// 本体自身の素性。「いま動いている本体はどのビルドか」を殻が言えるように
	//（アップデート後にホットリロードが機能したことを確かめる唯一の手段でもある）。
	struct VwPayloadInfo
	{
		unsigned int size;
		const char* commit; // 短縮 sha（"local" のこともある）
		const char* branch; //
	};

	// -----------------------------------------------------------------------
	// リセットを要求する PIO の種別。**殻の PIO ごとに 1 つ**（殻は登録だけを持ち、描画は
	// 本体が行う）。値は ABI の一部なので**並べ替えない・詰め直さない**。
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
#define VW_PAYLOAD_SYM_MCP_SERVE "vw_payload_mcp_serve"
#define VW_PAYLOAD_SYM_RECALC "vw_payload_recalculate"
#define VW_PAYLOAD_SYM_SHUTDOWN "vw_payload_shutdown"

	// その型。
	using VwPayloadAbiVersionFn = unsigned int (*)();
	using VwPayloadInitFn = int (*)(const VwPayloadHost*);
	using VwPayloadInfoFn = int (*)(VwPayloadInfo*);
	// **本番の取り込みコマンド 1 周ぶん**（ファイル選択 → 設定 → 取り込み → 結果
	// ダイアログ）。M25 で往復の処理を取り除いたので、返すものは「呼べたか」だけになった
	// （src/draw/ImportCommand.h）。
	using VwPayloadRunImportFn = int (*)();

	// **MCP ブリッジの受け付け 1 回**（M30。src/draw/McpBridge.h）。置かれている要求を
	// 処理して**すぐ戻る**——殻のパレット（src/Extensions/ExtMcpPalette.h）の時計が数百 ms
	// ごとに呼ぶ。out にはパレットに表示する状態の JSON が入る（寿命は他の文字列と同じ
	// ——**次に本体を呼ぶまで**。殻はその場で複製する）。
	//
	// **殻にしかできない要求**（更新・再起動。M38）は、本体が要求を受け取って表示状態の
	// `action`（id・種類・引数）に載せて返す。本体は自分をアンロードできないので、入れ替えは
	// 本体から戻ったあとの殻でしか実行できない（src/PayloadSession.h）。殻はそれを実行したら、
	// 結末の JSON（`{"id":…,"ok":…,"result":…,"error":…}`）を次の呼び出しの shellReport に渡し、
	// **そのとき載っている本体**（入れ替えたなら新しいほう）が応答を書く。無ければ nullptr。
	using VwPayloadMcpServeFn = int (*)(const char* shellReport, const char** out);
	using VwPayloadRecalculateFn = int (*)(unsigned int, void*, int*);
	using VwPayloadShutdownFn = void (*)();

	// -----------------------------------------------------------------------
	// 戻り値。**0 が成功**で、それ以外は理由を表す（例外は境界を越えさせないので、失敗は
	// すべてこの整数で返る）。
	enum VwPayloadStatus
	{
		kVwPayloadOk = 0,
		kVwPayloadErrAbi = 1, // 版か構造体の大きさが合わない
		kVwPayloadErrHost = 2, // 殻から渡されたものが足りない（callbacks が空 等）
		kVwPayloadErrVcom = 3,		// GS_InitializeVCOM に失敗した / gSDK が空のまま
		kVwPayloadErrNotInit = 4,	// init が済んでいない
		kVwPayloadErrUnknownId = 5, // 知らない PIO 種別
		kVwPayloadErrException = 6, // 中で例外が出た（境界の手前で捕捉して破棄した）
	};
} // extern "C"
