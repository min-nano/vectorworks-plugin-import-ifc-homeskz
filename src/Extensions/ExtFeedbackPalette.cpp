//
//	Extensions/ExtFeedbackPalette.cpp
//
//	モードレスの往復パレットの登録と取り次ぎ（意図は ExtFeedbackPalette.h 参照）。
//	**ここに往復の中身は無い**——JS から届いた呼び出しを殻の駆動（src/FeedbackLoopHost.h）へ
//	渡し、返ってきた見え方を JSON にして Promise を解決するだけ。
//
//	使う SDK API はすべて SDK リファレンスの Findings「モードレス（非モーダル）なパレット」
//	から写し取ったもの（実機未確認。ExtFeedbackPalette.h 冒頭）:
//	  VWExtensionWebPalette / VWExtensionPaletteJSProvider（VWFC/PluginSupport/
//	  VWExtensionWebPalette.h）、IWebJavaScriptProvider::IInitContext::AddExecute /
//	  AddFunctionPromiseSync（Interfaces/VectorWorks/Extension/IExtensionWebPalette.h）、
//	  IJSFunctionCallbackContext::Resolve（Interfaces/VectorWorks/UI/IWebBrowserDlg.h）、
//	  DEFINE_VWPaletteExtension / IMPLEMENT_VWPaletteExtension（VWFC/PluginSupport/
//	  VWExtensions.h）。ディスパッチ表のマクロ（BEGIN_/ADD_WebPalette_…）は使わない
//	（ExtFeedbackPalette.h の OnFunctionCall）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtFeedbackPalette.h"
#include "FeedbackLoop.h"
#include "FeedbackLoopHost.h"

#include <chrono>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// JS 側の名前空間と関数名。HTML（resources/<vwr>/html/index.html）はこの綴りで呼ぶ。
		constexpr const char* kJsObject = "homeskz";
		constexpr const char* kJsTick = "homeskz.tick";
		constexpr const char* kJsStop = "homeskz.stop";
		constexpr const char* kJsCheckNow = "homeskz.checkNow";
		constexpr const char* kJsHide = "homeskz.hide";

		// パレットの大きさ（ピクセル）。本文 3〜4 行とボタン 2 つが収まる程度。
		constexpr ViewCoord kInitialWidth = 420;
		constexpr ViewCoord kInitialHeight = 240;
		constexpr ViewCoord kMinimalWidth = 300;
		constexpr ViewCoord kMinimalHeight = 160;

		long long NowSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
					   std::chrono::steady_clock::now().time_since_epoch())
				.count();
		}

		// 見え方を Promise で JS へ返す。**JSON 文字列のまま渡し、JS 側で parse する**
		// ——nlohmann::json のオブジェクトを組む作法を SDK のヘッダ越しに当てるより、
		// 文字列 1 つのほうが確実に通る。
		void ResolveView(VectorWorks::UI::IJSFunctionCallbackContext* context,
						 const FeedbackLoopView& view)
		{
			if (context == nullptr)
				return;
			const std::string json = FeedbackLoopViewJson(view, NowSeconds());
			context->Resolve(nlohmann::json(json));
		}
	} // namespace
} // namespace HomeskzIfcImport

// ---------------------------------------------------------------------------
// JS の受け口。

CFeedbackPaletteJS::CFeedbackPaletteJS(IVWUnknown* parent) : VWExtensionPaletteJSProvider(parent) {}

CFeedbackPaletteJS::~CFeedbackPaletteJS() = default;

void CFeedbackPaletteJS::OnInit(
	VectorWorks::Extension::IWebJavaScriptProvider::IInitContext* context)
{
	// 基底が Web フレーム（fWebFrame）を控える。
	VWExtensionPaletteJSProvider::OnInit(context);
	if (context == nullptr)
		return;

	// 名前空間のオブジェクトを先に作る（SDK の IWebBrowserDlg.h の例と同じ作法:
	// "API = {};" を pre-code で置いてから "API.getData" を登録する）。
	context->AddExecute(
		TXString((std::string("var ") + kJsObject + " = window." + kJsObject + " || {};").c_str()));
	// **Sync**（Vectorworks のメインスレッドで呼ばれる＝SDK と本体を安全に触れる）。
	// Promise で返すので、JS 側は homeskz.tick().then(view => …) と書く。
	context->AddFunctionPromiseSync(kJsTick);
	context->AddFunctionPromiseSync(kJsStop);
	context->AddFunctionPromiseSync(kJsCheckNow);
	context->AddFunctionPromiseSync(kJsHide);
}

void CFeedbackPaletteJS::OnFunction(const TXString& name, const std::vector<nlohmann::json>& args,
									VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	// "homeskz.tick" → objName "homeskz" / functionName "tick"。"." が無ければ全部を関数名に。
	const std::string full = static_cast<const char*>(name);
	const std::string::size_type dot = full.rfind('.');
	const TXString objName(
		(dot == std::string::npos ? std::string() : full.substr(0, dot)).c_str());
	const TXString functionName((dot == std::string::npos ? full : full.substr(dot + 1)).c_str());
	this->OnFunctionCall(objName, functionName, args, context);
}

void CFeedbackPaletteJS::OnFunctionCall(const TXString& objName, const TXString& functionName,
										const std::vector<nlohmann::json>& args,
										VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	if (functionName == "tick")
		this->OnTick(objName, functionName, args, context);
	else if (functionName == "stop")
		this->OnStop(objName, functionName, args, context);
	else if (functionName == "checkNow")
		this->OnCheckNow(objName, functionName, args, context);
	else if (functionName == "hide")
		this->OnHide(objName, functionName, args, context);
	else if (context != nullptr)
		context->Reject("unknown function"); // 知らない名前は黙って落とさず JS へ返す
}

void CFeedbackPaletteJS::OnTick(const TXString& /*objName*/, const TXString& /*functionName*/,
								const std::vector<nlohmann::json>& /*args*/,
								VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	// 例外を JS の橋へ漏らさない（SDK のコールバックと同じ扱い）。
	try
	{
		ResolveView(context, FeedbackLoopTick());
	}
	catch (...)
	{
		if (context != nullptr)
			context->Reject("tick failed");
	}
}

void CFeedbackPaletteJS::OnStop(const TXString& /*objName*/, const TXString& /*functionName*/,
								const std::vector<nlohmann::json>& /*args*/,
								VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	try
	{
		ResolveView(context, FeedbackLoopStop());
	}
	catch (...)
	{
		if (context != nullptr)
			context->Reject("stop failed");
	}
}

void CFeedbackPaletteJS::OnCheckNow(const TXString& /*objName*/, const TXString& /*functionName*/,
									const std::vector<nlohmann::json>& /*args*/,
									VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	try
	{
		ResolveView(context, FeedbackLoopCheckNow());
	}
	catch (...)
	{
		if (context != nullptr)
			context->Reject("checkNow failed");
	}
}

void CFeedbackPaletteJS::OnHide(const TXString& /*objName*/, const TXString& /*functionName*/,
								const std::vector<nlohmann::json>& /*args*/,
								VectorWorks::UI::IJSFunctionCallbackContext* context)
{
	try
	{
		ShowFeedbackPalette(false);
		ResolveView(context, TheFeedbackLoop().View());
	}
	catch (...)
	{
		if (context != nullptr)
			context->Reject("hide failed");
	}
}

// ---------------------------------------------------------------------------
// パレットそのもの。安定版と開発版は同時に読み込まれうるので UUID とユニバーサル名は別
// （登録するのは開発版だけだが、綴りの規則は他の拡張と揃える）。
//
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: 9ba75b62-8671-4800-8ca5-edf9108614bb  (dev build)
IMPLEMENT_VWPaletteExtension(
	/*Extension class*/ CExtFeedbackPalette,
	/*Universal name*/ PLUGIN_FEEDBACK_PALETTE_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x9ba75b62, 0x8671, 0x4800, 0x8c, 0xa5, 0xed, 0xf9, 0x10, 0x86, 0x14, 0xbb);
#else
// UUID: ea8bf52d-50ae-401c-8308-e6dc542fc592  (stable build)
IMPLEMENT_VWPaletteExtension(
	/*Extension class*/ CExtFeedbackPalette,
	/*Universal name*/ PLUGIN_FEEDBACK_PALETTE_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0xea8bf52d, 0x50ae, 0x401c, 0x83, 0x08, 0xe6, 0xdc, 0x54, 0x2f, 0xc5, 0x92);
#endif
// NOLINTEND(misc-const-correctness)

// 中身は .vwr の html/index.html（GetStandardURL の既定と同じ綴りを明示しておく）。
CExtFeedbackPalette::CExtFeedbackPalette(CallBackPtr /*cbp*/)
	: VWExtensionWebPalette("html", "index.html")
{
}

CExtFeedbackPalette::~CExtFeedbackPalette() = default;

void CExtFeedbackPalette::DefineSinks()
{
	// JS からの呼び出しを受ける側だけを登録する（IWebCallbacksProvider は要らない）。
	this->DefineSink<CFeedbackPaletteJS>(VectorWorks::Extension::IID_WebJavaScriptProvider);
}

TXString VCOM_CALLTYPE CExtFeedbackPalette::GetTitle()
{
	return TXString("実機フィードバックの往復 (みんなの構造設計支援Dev)");
}

bool VCOM_CALLTYPE CExtFeedbackPalette::GetInitialSize(ViewCoord& outCX, ViewCoord& outCY)
{
	outCX = kInitialWidth;
	outCY = kInitialHeight;
	return true;
}

bool VCOM_CALLTYPE CExtFeedbackPalette::GetMinimalSize(ViewCoord& outCX, ViewCoord& outCY)
{
	outCX = kMinimalWidth;
	outCY = kMinimalHeight;
	return true;
}
