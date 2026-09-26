//
//	Extensions/ExtMcpPalette.cpp
//
//	MCP ブリッジのパレットの登録と取り次ぎ（意図は ExtMcpPalette.h）。**ここに受け付けの
//	中身は無い**——JS の時計から届いた呼び出しを本体の vw_payload_mcp_serve へ渡し、返って
//	きた見え方（JSON）で Promise を解決するだけ。
//
//	使う SDK API は往復のパレット（Extensions/ExtFeedbackPalette.cpp）と同じもので、
//	実機で確かめてある（docs/DEV-NOTES.md M24「実機で確かめられたこと（round 1）」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMcpPalette.h"
#include "PayloadSession.h"

#include <chrono>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// JS 側の名前空間と関数名。HTML（resources/common.vwr/html/mcp.html）はこの綴りで呼ぶ。
		constexpr const char* kJsObject = "vwmcp";
		constexpr const char* kJsServe = "vwmcp.serve";

		// パレットの大きさ（ピクセル）。状態 1 行と数字 2 行が収まる程度。
		constexpr ViewCoord kInitialWidth = 360;
		constexpr ViewCoord kInitialHeight = 150;
		constexpr ViewCoord kMinimalWidth = 240;
		constexpr ViewCoord kMinimalHeight = 100;

		// 本体を読み込めなかったとき、次に試すまで（秒）。**時計は数百 ms ごとに来る**ので、
		// 読めない本体を毎回複製して読みに行かない（PayloadHost.h「必ず複製してから読む」）。
		constexpr long long kLoadRetrySeconds = 10;

		long long NowSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
					   std::chrono::steady_clock::now().time_since_epoch())
				.count();
		}

		// 殻で組む見え方（本体へ届かなかったとき）。**nlohmann::json で組む**——文字列を
		// 手で継ぐと、理由の文に引用符や改行が混じったときに JSON が壊れる。
		std::string ShellView(const char* phase, const std::string& message)
		{
			nlohmann::json view = nlohmann::json::object();
			view["phase"] = phase;
			view["message"] = message;
			try
			{
				return view.dump();
			}
			catch (...)
			{
				// 理由の文が UTF-8 として壊れていた。見え方だけは返す。
				return std::string(R"({"phase":")") + phase + R"("})";
			}
		}

		// 時計 1 刻みぶん。本体へ届けて見え方を返す。
		std::string ServeOnce()
		{
			// **取り込みの最中は本体へ入り直さない**（ExtMcpPalette.h「取り込みの最中は
			// 見送る」）。
			if (PayloadInUse())
				return ShellView("paused", "取り込みなどの最中なので、終わるまで受け付けを"
										   "見送っています。");

			static long long sRetryAt = 0;
			static std::string sFailure;
			const long long now = NowSeconds();
			if (now < sRetryAt)
				return sFailure;

			const PayloadUse use;
			std::string out;
			std::string error;
			if (use.ok() && use->mcpServe(out, error))
				return out;

			sRetryAt = now + kLoadRetrySeconds;
			sFailure = ShellView("error", use.ok() ? error : use.error());
			return sFailure;
		}
	} // namespace

	void ShowMcpPalette()
	{
		if (gSDK == nullptr)
			return;
		gSDK->SetWebPaletteVisibility(CExtMcpPalette::_GetIID(), true);
	}
} // namespace HomeskzIfcImport

// ---------------------------------------------------------------------------
// JS の受け口。

CMcpPaletteJS::CMcpPaletteJS(IVWUnknown* parent) : VWExtensionPaletteJSProvider(parent) {}

CMcpPaletteJS::~CMcpPaletteJS() = default;

void CMcpPaletteJS::OnInit(VectorWorks::Extension::IWebJavaScriptProvider::IInitContext* context)
{
	VWExtensionPaletteJSProvider::OnInit(context);
	if (context == nullptr)
		return;
	context->AddExecute(
		TXString((std::string("var ") + kJsObject + " = window." + kJsObject + " || {};").c_str()));
	// **Sync**（Vectorworks のメインスレッドで呼ばれる＝SDK と本体を安全に触れる）。
	context->AddFunctionPromiseSync(kJsServe);
}

void CMcpPaletteJS::OnFunction(const TXString& name, const std::vector<nlohmann::json>& args,
							   VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	const std::string full = static_cast<const char*>(name);
	const std::string::size_type dot = full.rfind('.');
	const TXString objName(
		(dot == std::string::npos ? std::string() : full.substr(0, dot)).c_str());
	const TXString functionName((dot == std::string::npos ? full : full.substr(dot + 1)).c_str());
	this->OnFunctionCall(objName, functionName, args, context);
}

void CMcpPaletteJS::OnFunctionCall(const TXString& /*objName*/, const TXString& functionName,
								   const std::vector<nlohmann::json>& /*args*/,
								   VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	if (context == nullptr)
		return;
	if (functionName != "serve")
	{
		context->Reject("unknown function"); // 知らない名前は黙って落とさず JS へ返す
		return;
	}
	// 例外を JS の橋へ漏らさない（SDK のコールバックと同じ扱い）。
	try
	{
		// JSON 文字列のまま渡し、JS 側で parse する（ExtFeedbackPalette.cpp の ResolveView と
		// 同じ作法）。
		context->Resolve(nlohmann::json(ServeOnce()));
	}
	catch (...)
	{
		context->Reject("serve failed");
	}
}

// ---------------------------------------------------------------------------
// パレットそのもの。安定版と開発版は同時に読み込まれうるので UUID とユニバーサル名は別。
//
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: 1d0d6a0c-2e00-4e6c-897d-4784be3a6a28  (dev build)
IMPLEMENT_VWPaletteExtension(
	/*Extension class*/ CExtMcpPalette,
	/*Universal name*/ PLUGIN_MCP_PALETTE_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x1d0d6a0c, 0x2e00, 0x4e6c, 0x89, 0x7d, 0x47, 0x84, 0xbe, 0x3a, 0x6a, 0x28);
#else
// UUID: 3e57e0ef-0fc4-4e06-9372-9813f4725575  (stable build)
IMPLEMENT_VWPaletteExtension(
	/*Extension class*/ CExtMcpPalette,
	/*Universal name*/ PLUGIN_MCP_PALETTE_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x3e57e0ef, 0x0fc4, 0x4e06, 0x93, 0x72, 0x98, 0x13, 0xf4, 0x72, 0x55, 0x75);
#endif
// NOLINTEND(misc-const-correctness)

// 中身は .vwr の html/mcp.html（resources/common.vwr から包む。CMakeLists.txt）。
CExtMcpPalette::CExtMcpPalette(CallBackPtr /*cbp*/) : VWExtensionWebPalette("html", "mcp.html") {}

CExtMcpPalette::~CExtMcpPalette() = default;

void CExtMcpPalette::DefineSinks()
{
	this->DefineSink<CMcpPaletteJS>(VectorWorks::Extension::IID_WebJavaScriptProvider);
}

TXString VCOM_CALLTYPE CExtMcpPalette::GetTitle()
{
#ifdef VW_DEV_BUILD
	return {"MCP ブリッジ (みんなの構造設計支援Dev)"};
#else
	return {"MCP ブリッジ (みんなの構造設計支援)"};
#endif
}

bool VCOM_CALLTYPE CExtMcpPalette::GetInitialSize(ViewCoord& outCX, ViewCoord& outCY)
{
	outCX = kInitialWidth;
	outCY = kInitialHeight;
	return true;
}

bool VCOM_CALLTYPE CExtMcpPalette::GetMinimalSize(ViewCoord& outCX, ViewCoord& outCY)
{
	outCX = kMinimalWidth;
	outCY = kMinimalHeight;
	return true;
}
