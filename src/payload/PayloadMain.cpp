//
//	payload/PayloadMain.cpp
//
//	**本体（ペイロード）の入口。** ここが持っているのは「殻から呼ばれたものを、中の実装へ
//	取り次ぐ」処理だけ。実処理は draw::runImportCommand（本番の取り込み）・
//	draw::serveMcpBridge（MCP ブリッジ。実機テストの draw::runTestRound もこの中から
//	呼ばれる）と draw::recalculate*（PIO のリセット）にある。
//	メニュー・PIO の登録と自動アップデートは殻の側（そちらはめったに変わらない＝再起動も
//	めったに要らない）。
//
//	Vectorworks はこのモジュールを知らない——読み込むのは殻（src/PayloadHost.cpp）で、
//	境界は C の ABI（src/PayloadAbi.h）。だから**アンロードして、置き換えて、再読み込み
//	できる**＝プラグインのアップデートに Vectorworks の再起動が要らない。
//
//	【SDK をどう使えるようにするか】gSDK / gCBP / gVWMM は静的ライブラリ（libVWSDK.a /
//	VWSDK.lib）が持つ**モジュールごとのグローバル**である。このモジュールは自分の複製を
//	持っているので、読み込んだだけでは全部 nil のまま。殻が受け取った CallBackPtr を受け取って
//	::GS_InitializeVCOM へ渡すと、そこで設定される——通常のプラグインの plugin_module_main が
//	行っているのと同じことを、必要な値を外から受け取って行う形
//	（[SDK リファレンス「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)
//	で実測済み）。
//
//	【境界を越えさせないもの】例外（すべてここで捕捉する）、C++ のオブジェクト、アンロード
//	した後も使われる文字列（返す const char* は殻がその場で複製する約束）。
//

#include "PluginPrefix.h"

#include "BuildConfig.h"
#include "PayloadAbi.h"
#include "PayloadHostHolder.h"
#include "draw/ColumnMarkPio.h"
#include "draw/Feedback.h"
#include "draw/HostServices.h"
#include "draw/ImportCommand.h"
#include "draw/McpBridge.h"
#include "draw/ShearWallPio.h"

#include <exception>
#include <string>
#include <vector>

namespace
{
	using namespace HomeskzIfcImport;

	// **殻から渡されたものは、ポインタで持たずに複製する。** そうしないと、殻の load から
	// 戻った時点で無効なポインタを持つことになる（理由と異常終了の仕方は PayloadHostHolder.h）。
	payload::HostHolder gHost;
	bool gPayloadReady = false;

	// **殻へ返す文字列の置き場所。** 返した const char* は「次に本体を呼ぶまで」有効である
	// 約束（src/PayloadAbi.h）なので、静的に 1 つ持って毎回書き換える。
	std::string gMcpViewText;
} // namespace

// ---------------------------------------------------------------------------
// **SDK の静的ライブラリをリンクするモジュールが必ず定義しなければならない 2 つ。**
// どちらも libVWSDK.a / VWSDK.lib の中から参照されるので、Vectorworks にプラグインとして
// 登録されないこのモジュールでも要る（無いとリンクで未解決になる。SDK リファレンス
// 「プラグインモジュールの読み込みと入れ替え」）。

// ① GS_InitializeVCOM がこれを呼ぶ（Include/VectorworksSDK.h の注記どおり）。
extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_ver()
{
	return SDK_VERSION;
}

// ② リソース（.vwr）の識別子。TXResStr / TXLegacyResource / GS_GetLayoutFromRsrc から
//    参照される。**このモジュールは .vwr を持たない**（メニュー名・PIO 名・パラメータ名の
//    文字列はすべて殻の側が登録に使うもの）が、リンクを通すために定義だけ要る。値は殻と
//    同じものにしておく。
const char* DefaultPluginVWRIdentifier()
{
	return PLUGIN_VWR_ID;
}

// ---------------------------------------------------------------------------
// ここから下が殻との境界（src/PayloadAbi.h）。**例外を外へ出さない。**

VW_PAYLOAD_EXPORT unsigned int vw_payload_abi_version()
{
	return VW_PAYLOAD_ABI_VERSION;
}

VW_PAYLOAD_EXPORT int vw_payload_init(const VwPayloadHost* host)
{
	try
	{
		// **受け取ってその場で複製する**（版と大きさの確認も入れ物の側で行う）。以降、殻から
		// 渡された記憶域には二度とアクセスしない。
		const int adopted = gHost.adopt(host);
		if (adopted != kVwPayloadOk)
			return adopted;

		// **ここが要点。** 自分の側の gSDK / gCBP / gVWMM を設定する。
		const VCOMError err = ::GS_InitializeVCOM(gHost.callbacks());
		if (err != kVCOMError_NoError)
		{
			gHost.forget();
			return kVwPayloadErrVcom;
		}
		if (gSDK == nil)
		{
			gHost.forget();
			return kVwPayloadErrVcom;
		}

		// **殻から借りた機能を本体の中へ登録する**（draw/HostServices.h）。借りるのは同梱
		// スクリプトの実行だけで、本体からは実行できない——同梱物の場所が要るが、本体が
		// 読み込まれるのは一時ディレクトリの複製だから。
		// **複製して保持する**のは境界の決めごとどおり（PayloadHostHolder.h）。
		draw::HostServices services;
		if (gHost.canRunScripts())
		{
			services.runScript = [](const std::string& baseName,
									const std::vector<std::string>& args, std::string& out)
			{ return gHost.runScript(baseName, args, out); };
		}
		draw::setHostServices(services);

		gPayloadReady = true;
		return kVwPayloadOk;
	}
	catch (...)
	{
		gHost.forget();
		gPayloadReady = false;
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_info(VwPayloadInfo* out)
{
	try
	{
		// **init の前でも答える。** 殻は「読み込んだものが何か」を先に示せたほうがよい
		// （ABI が合わずに破棄するときも、何を破棄したのか表示できる）。返す const char* は
		// 静的な文字列リテラルなので、アンロードするまで有効である。
		if (out == nullptr || out->size < sizeof(VwPayloadInfo))
			return kVwPayloadErrAbi;
		out->commit = VW_BUILD_VERSION;
		out->branch = VW_BUILD_BRANCH;
		return kVwPayloadOk;
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_run_import()
{
	try
	{
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;
		// 取り込みは自分の中で例外を捕捉し、ユーザーへはダイアログで表示する
		// （draw/ImportRun.cpp）。ここは**境界の最後の防御**として、そこで漏れたものを
		// 捕捉するだけ。**実機テストのことは何も知らない**——実機テストは MCP の
		// `vw_run_test`（vw_payload_mcp_serve の中）だけが起こす（M25 / M43）。
		draw::runImportCommand();
		return kVwPayloadOk;
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_mcp_serve(const char* shellReport, const char** out)
{
	try
	{
		if (out == nullptr)
			return kVwPayloadErrAbi;
		*out = nullptr;
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;
		// 1 回ぶん処理して**すぐ戻る**（draw/McpBridge.h）。中で例外を捕捉して表示状態に載せる。
		// 殻が実行した要求の結末は、ここで複製してから渡す（寿命は殻の呼び出しの間だけ）。
		gMcpViewText =
			draw::serveMcpBridge(shellReport != nullptr ? std::string(shellReport) : std::string());
		*out = gMcpViewText.c_str();
		return kVwPayloadOk;
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_recalculate(unsigned int kind, void* objectHandle, int* outEvent)
{
	try
	{
		if (outEvent == nullptr)
			return kVwPayloadErrAbi;
		// kObjectEventNoErr は VWFC::PluginSupport にあり、ここは大域スコープなので
		// 修飾して参照する（draw/ColumnMarkPio.h の注記と同じ理由）。
		*outEvent = VWFC::PluginSupport::kObjectEventNoErr;
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;

		// MCObjectHandle は境界を void* で受け渡す（src/PayloadAbi.h「SDK を include しない」）。
		auto* const object = reinterpret_cast<MCObjectHandle>(objectHandle);
		switch (kind)
		{
		case kVwPayloadPioColumnMark:
			*outEvent = draw::recalculateColumnMark(object);
			return kVwPayloadOk;
		case kVwPayloadPioShearWall:
			*outEvent = draw::recalculateShearWall(object);
			return kVwPayloadOk;
		default:
			// 殻のほうが新しく、こちらの知らない PIO を要求してきた。**描画せずに正常
			// 終了として返す**（殻は kObjectEventNoErr を返し、既に描画してあるものを
			// 消さない）。
			return kVwPayloadErrUnknownId;
		}
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT void vw_payload_shutdown()
{
	// アンロードする直前に殻が呼ぶ。**殻へ渡したものを手放す**のがここの役割——このモジュールの
	// 番地を保持されたままアンロードすると、次にアクセスした瞬間に異常終了する。
	gPayloadReady = false;
	// 殻から借りたものを手放す（このモジュールの番地も、殻の番地も持ち越さない）。
	draw::clearHostServices();
	gHost.forget();
}
