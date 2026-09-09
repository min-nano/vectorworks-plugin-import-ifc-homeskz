//
//	payload/PayloadMain.cpp
//
//	**本体（ペイロード）の入口。** Vectorworks はこのモジュールを知らない——読み込むのは
//	殻（src/PayloadHost.cpp）で、境界は C の ABI（src/PayloadAbi.h）。だから**降ろして、
//	置き換えて、読み直せる**＝プラグインのアップデートに Vectorworks の再起動が要らない。
//
//	ここが持っているのは「殻から呼ばれたものを、中の実装へ取り次ぐ」ところだけ。実処理は
//	draw::runImportCommand（本番の取り込み）・draw::runTestRound（実機テスト。M25）と
//	draw::recalculate*（PIO のリセット）にある。
//	メニュー・PIO の登録と自動アップデートは殻の側（そちらは滅多に変わらない＝再起動も
//	滅多に要らない）。
//
//	【SDK をどう使えるようにするか】gSDK / gCBP / gVWMM は静的ライブラリ（libVWSDK.a /
//	VWSDK.lib）が持つ**モジュールごとのグローバル**である。このモジュールは自分の複製を
//	持っているので、読み込んだだけでは全部 nil のまま。殻が受け取った CallBackPtr をもらって
//	::GS_InitializeVCOM へ渡すと、そこで埋まる——普通のプラグインの plugin_module_main が
//	やっているのと同じことを、外から材料をもらって行う形
//	（[SDK リファレンス「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)
//	で実測済み）。
//
//	【境界を越えさせないもの】例外（すべてここで受ける）、C++ のオブジェクト、降ろした後も
//	使われる文字列（返す const char* は殻がその場で写す約束）。
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

	// **殻から渡されたものは、ポインタで持たずに写す。** そうしないと、殻の load から
	// 戻った時点で腐ったポインタを持つことになる（理由と落ち方は PayloadHostHolder.h）。
	payload::HostHolder gHost;
	bool gPayloadReady = false;

	// **殻へ返す文字列の置き場所。** 返した const char* は「次に本体を呼ぶまで」生きている
	// 約束（src/PayloadAbi.h）なので、静的に 1 つ持って毎回書き換える。
	std::string gLoopStatusText;
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
		// **受け取ってその場で写す**（版と大きさの確認も入れ物の側でやる）。以降、殻から
		// 渡された記憶域には二度と触らない。
		const int adopted = gHost.adopt(host);
		if (adopted != kVwPayloadOk)
			return adopted;

		// **ここが要（かなめ）。** 自分の側の gSDK / gCBP / gVWMM を埋める。
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

		// **殻から借りた道具を本体の中へ預ける**（draw/HostServices.h）。同梱スクリプトの
		// 実行と殻の ID は、本体からは手が届かない——前者は同梱物の在り処が要り（本体が
		// 読まれるのは一時ディレクトリの複製）、後者は殻にしかコンパイルされていない。
		// **写して持つ**のは境界の決めごとどおり（PayloadHostHolder.h）。
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
		// **init の前でも答える。** 殻は「読んだものが何か」を先に言えたほうがよい
		// （ABI が合わずに捨てるときも、何を捨てたのか出せる）。返す const char* は
		// 静的な文字列リテラルなので、降ろすまで生きている。
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
		// 取り込みは自分の中で例外を受け、ユーザーへはダイアログで見せる
		// （draw/ImportRun.cpp）。ここは**境界の最後の砦**として、そこで漏れたものを
		// 受けるだけ。**往復のことは何も持ち帰らない**——それは vw_payload_run_test の
		// 仕事である（M25。src/PayloadAbi.h）。
		draw::runImportCommand();
		return kVwPayloadOk;
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_run_test(int allowDialogs, int* outActive)
{
	try
	{
		if (outActive != nullptr)
			*outActive = 0;
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;
		// **往復を知っているのは本体のここだけ**（src/draw/Feedback.h の runTestRound）。
		// outActive が立って戻ったら、殻はパレットを開いて周を回し始める
		// （src/Extensions/ExtTestMenu.cpp）。
		bool active = false;
		(void)draw::runTestRound(allowDialogs != 0, active);
		if (outActive != nullptr)
			*outActive = active ? 1 : 0;
		return kVwPayloadOk;
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_run_mcp_bridge()
{
	try
	{
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;
		// ブリッジは**止められるまで戻らない**（draw/McpBridge.h）。中で例外を受け、
		// 利用者にはダイアログで見せる。ここは境界の最後の砦。
		draw::runMcpBridge();
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
		// 修飾して引く（draw/ColumnMarkPio.h の注記と同じ理由）。
		*outEvent = VWFC::PluginSupport::kObjectEventNoErr;
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;

		// MCObjectHandle は境界を void* で渡る（src/PayloadAbi.h「SDK を include しない」）。
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
			// 殻のほうが新しく、こちらの知らない PIO を頼んできた。**描かずに正常
			// 終了として返す**（殻は kObjectEventNoErr を返し、既に描いてあるものを
			// 消さない）。
			return kVwPayloadErrUnknownId;
		}
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_loop_status(const char** out)
{
	try
	{
		if (out == nullptr)
			return kVwPayloadErrAbi;
		*out = nullptr;
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;
		// 記憶の形を知っているのは本体だけ（core::FeedbackSession）。殻には key=value の
		// 平たい行で渡す（draw/Feedback.h の feedbackLoopStatus）。
		gLoopStatusText = draw::feedbackLoopStatus();
		*out = gLoopStatusText.c_str();
		return kVwPayloadOk;
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT int vw_payload_loop_end(const char* reason, int notifyPr)
{
	try
	{
		if (!gPayloadReady || gSDK == nil)
			return kVwPayloadErrNotInit;
		draw::endFeedbackLoop(reason != nullptr ? std::string(reason) : std::string(),
							  notifyPr != 0);
		return kVwPayloadOk;
	}
	catch (...)
	{
		return kVwPayloadErrException;
	}
}

VW_PAYLOAD_EXPORT void vw_payload_shutdown()
{
	// 降ろす直前に殻が呼ぶ。**殻へ渡したものを手放す**のがここの仕事——このモジュールの
	// 番地を持たれたまま降ろすと、次に触った瞬間に落ちる。
	gPayloadReady = false;
	// 殻から借りたものを手放す（このモジュールの番地も、殻の番地も持ち越さない）。
	draw::clearHostServices();
	gHost.forget();
}
