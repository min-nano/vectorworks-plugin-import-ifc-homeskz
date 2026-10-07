//
//	Extensions/ExtMcpPalette.cpp
//
//	MCP ブリッジのパレットの登録と取り次ぎ（意図は ExtMcpPalette.h）。**ここに受け付けの
//	中身は無い**——JS の時計から届いた呼び出しを本体の vw_payload_mcp_serve へ渡し、返って
//	きた表示状態（JSON）で Promise を解決するだけ。殻に要求されたこと（更新・再起動）も
//	ここで実行する（ExtMcpPalette.h「殻に要求されること」）。
//
//	使う SDK API は M24 の往復のパレット（M38 で削除した）と同じもので、実機で確かめてある
//	（docs/DEV-NOTES.md M24「実機で確かめられたこと（round 1）」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMcpPalette.h"
#include "PayloadSession.h"
#include "Updater.h"
#include "UpdaterHost.h"

#include <chrono>
#include <string>
#include <vector>

#if defined(__APPLE__)
// 殻の時計（StartMcpBridgeClock）。
#	include <CoreFoundation/CoreFoundation.h>
#endif

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
		// 読み込めない本体を毎回複製して読み込まない（PayloadHost.h「必ず複製してから読む」）。
		constexpr long long kLoadRetrySeconds = 10;

		long long NowSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
					   std::chrono::steady_clock::now().time_since_epoch())
				.count();
		}

		// 殻で組み立てる表示状態（本体へ届かなかったとき）。**nlohmann::json で組み立てる**
		// ——文字列を手で連結すると、理由の文に引用符や改行が混じったときに JSON が壊れる。
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
				// 理由の文が UTF-8 として壊れていた。表示状態だけは返す。
				return std::string(R"({"phase":")") + phase + R"("})";
			}
		}

		// **殻が実行した要求の結末**（本体へ渡して応答として書いてもらう。PayloadAbi.h の
		// VwPayloadMcpServeFn）。本体が `reportDone` を返すまで渡し直す——入れ替えの直後は
		// 新しい本体の用意（スプールの準備）が 1 回で済まないことがある。渡し直しは上限つき
		// （上限に達するころには Python 側がすでにタイムアウトしている）。
		struct PendingReport
		{
			std::string json;
			long long since = 0;
		};
		constexpr long long kReportGiveUpSeconds = 120;

		PendingReport& Pending()
		{
			static PendingReport sPending;
			return sPending;
		}

		// 本体の受け付けを 1 回呼ぶ。呼べなければ false と理由。
		bool CallServe(const std::string& report, std::string& out, std::string& error)
		{
			const PayloadUse use;
			if (!use.ok())
			{
				error = use.error();
				return false;
			}
			return use->mcpServe(report, out, error);
		}

		// 表示状態を読んで、結末を渡し終えたかを確認する。
		void SettleReport(const nlohmann::json& view, long long now)
		{
			PendingReport& pending = Pending();
			if (pending.json.empty())
				return;
			const bool done = view.is_object() && view.value("reportDone", false);
			if (done || now - pending.since > kReportGiveUpSeconds)
				pending = PendingReport{};
		}

		nlohmann::json ParseView(const std::string& text)
		{
			try
			{
				return nlohmann::json::parse(text);
			}
			catch (...)
			{
				return nlohmann::json::object(); // 読めない表示状態は「要求無し」とみなす
			}
		}

		const char* RemoteUpdateWord(RemoteUpdateOutcome outcome)
		{
			switch (outcome)
			{
			case RemoteUpdateOutcome::NoNewBuild:
				return "no_new_build";
			case RemoteUpdateOutcome::Installed:
				return "installed";
			case RemoteUpdateOutcome::NeedsRestart:
				return "needs_restart";
			case RemoteUpdateOutcome::Failed:
				return "failed";
			case RemoteUpdateOutcome::CheckFailed:
				return "check_failed";
			}
			return "failed";
		}

		// **殻に要求されたことを実行する**（M38）。結末の JSON を返し、restartAfter には
		// 「応答を書いてから再起動する」かが入る（再起動してからでは応答を書く者がいない）。
		//
		// **判断はここに持たせない。** 入れ替えの流れは src/UpdaterFlow.cpp の
		// RemoteDevUpdateWith、再起動は src/Updater.cpp の RequestRestart にあり、ここは
		// 引数を渡して結末を JSON に詰めるだけ（CLAUDE.md「インストールの経路は 1 本だけ」）。
		std::string RunShellAction(const nlohmann::json& action, bool& restartAfter)
		{
			restartAfter = false;
			nlohmann::json report = nlohmann::json::object();
			report["id"] = action.value("id", std::string());
			const std::string tool = action.value("tool", std::string());
			const nlohmann::json args = action.contains("args") && action["args"].is_object()
											? action["args"]
											: nlohmann::json::object();
			nlohmann::json result = nlohmann::json::object();
			if (tool == "vw_update")
			{
				const std::string branch = args.contains("branch") && args["branch"].is_string()
											   ? args["branch"].get<std::string>()
											   : std::string();
				const bool restartIfNeeded = args.contains("restart_if_needed") &&
											 args["restart_if_needed"].is_boolean() &&
											 args["restart_if_needed"].get<bool>();
				const RemoteUpdateResult update = RemoteDevUpdate(branch);
				result["outcome"] = RemoteUpdateWord(update.outcome);
				result["branch"] = update.branch;
				result["previous"] = update.previous;
				result["commit"] = update.commit;
				result["message"] = update.message;
				const bool needsRestart = update.outcome == RemoteUpdateOutcome::NeedsRestart;
				result["restart_required"] = needsRestart;
				restartAfter = needsRestart && restartIfNeeded;
				result["restarting"] = restartAfter;
				const bool ok = update.outcome == RemoteUpdateOutcome::Installed ||
								update.outcome == RemoteUpdateOutcome::NoNewBuild ||
								update.outcome == RemoteUpdateOutcome::NeedsRestart;
				report["ok"] = ok;
				if (!ok)
					report["error"] = update.message.empty() ? std::string("更新できませんでした。")
															 : update.message;
			}
			else if (tool == "vw_restart")
			{
				result["requested"] = true;
				report["ok"] = true;
				restartAfter = true;
			}
			else
			{
				report["ok"] = false;
				report["error"] = "殻の知らない頼みごとです: " + tool;
			}
			report["result"] = result;
			try
			{
				return report.dump();
			}
			catch (...)
			{
				// 理由の文が UTF-8 として壊れていた。id だけは返す（Python を待たせ続けない）。
				nlohmann::json fallback = nlohmann::json::object();
				fallback["id"] = report["id"];
				fallback["ok"] = false;
				fallback["error"] = "結末を JSON にできませんでした。";
				return fallback.dump();
			}
		}

		// 時計 1 刻みぶん。本体へ届けて見え方を返す。**呼ぶのは 2 つの時計**——殻の OS の
		// タイマー（StartMcpBridgeClock。受け付けの本線）と、パレットの JS タイマー（見え方を
		// 描き直すため。パレットが出ている間だけ）。どちらもメインスレッドから来る。
		std::string ServeOnce()
		{
			// **取り込みの最中は本体へ入り直さない**（ExtMcpPalette.h「取り込みの最中は
			// 見送る」）。
			if (PayloadInUse())
				return ShellView("paused", "取り込みなどの最中なので、終わるまで受け付けを"
										   "見送っています。");

			// **入れ子で入らない。** 殻に頼まれた更新（RunShellAction）は本体の外で走るので
			// PayloadInUse では止まらず、その途中で Vectorworks がイベントを回せば、もう一方の
			// 時計の刻みがここへ入り直しうる。
			static bool sServing = false;
			if (sServing)
				return ShellView("paused", "前の刻みの受け付けが終わるまで見送っています。");
			sServing = true;
			struct ServingGuard
			{
				ServingGuard(const ServingGuard&) = delete;
				ServingGuard& operator=(const ServingGuard&) = delete;
				ServingGuard(ServingGuard&&) = delete;
				ServingGuard& operator=(ServingGuard&&) = delete;
				ServingGuard() = default;
				~ServingGuard()
				{
					sServing = false;
				}
			} const guard;

			static long long sRetryAt = 0;
			static std::string sFailure;
			const long long now = NowSeconds();
			if (now < sRetryAt)
				return sFailure;

			std::string out;
			std::string error;
			if (!CallServe(Pending().json, out, error))
			{
				sRetryAt = now + kLoadRetrySeconds;
				sFailure = ShellView("error", error);
				return sFailure;
			}
			nlohmann::json view = ParseView(out);
			SettleReport(view, now);

			// **本体が殻に要求してきた**（vw_update / vw_restart）。いまは本体がスタックに無い
			// のでアンロードして入れ替えられる（src/PayloadSession.h）。結末をすぐ渡して応答を
			// 書いてもらうと、その回で次の要求を受け取ってくることがあるので、数回まで続けて
			// 実行する（上限は無限ループの防止）。
			constexpr int kMaxActionsPerTick = 3;
			for (int i = 0; i < kMaxActionsPerTick; ++i)
			{
				if (!view.is_object() || !view.contains("action") || !view["action"].is_object())
					break;
				bool restartAfter = false;
				Pending().json = RunShellAction(view["action"], restartAfter);
				Pending().since = now;

				// **すぐ渡す**（入れ替えたなら新しい本体が書く）。渡せなければ次の回で渡し直す。
				std::string replied;
				if (!CallServe(Pending().json, replied, error))
					replied.clear();
				view = ParseView(replied);
				SettleReport(view, now);
				if (!replied.empty())
					out = replied;

				// **再起動は応答を書いてから。** 先に要求すると、応答する者がいなくなる。
				// 開いている文書の保存確認は Vectorworks が通常どおり出す（src/Updater.h）。
				if (restartAfter)
				{
					(void)RequestRestart();
					break;
				}
			}
			return out;
		}

		// --- 殻の時計（OS のタイマー）---------------------------------------
		//
		// **受け付けの本線はパレットの JS タイマーではなく、ここ。** JS タイマーは埋め込み
		// ブラウザ（CEF）が、パレットを隠す・Vectorworks が裏に回ると 60 秒に 1 回まで間引き、
		// 図面が 1 枚も開いていない間はパレットそのものが出ない——ローカルの Claude Code から
		// 実機確認を回すとき、Vectorworks はたいてい裏にいて、再起動の直後は図面が無い
		// （docs/dev-notes/milestones/m41-bridge-os-timer.md）。OS のタイマーはどちらでも間引かれずに刻み、
		// その刻みから gSDK を読み書きできる（[SDK リファレンス「Timers and Notifications」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Timers%20and%20Notifications.md)）。
		//
		// 決めごと（どれも同 Findings の実測から）:
		//   * **既定のモード（kCFRunLoopDefaultMode）にだけ載せる。** 共通モードに載せると
		//     Vectorworks のモーダルダイアログの最中にも刻み、そのとき開いている undo の記録へ
		//     書き込みが混ざる（利用者の 1 回の取り消しが、無関係な取り込みごと持っていく）。
		//   * **undo の記録が開いている刻みは見送る**（IsCurrentlyBuildingAnUndoEvent）。
		//     信用できるのは「開いていない」の側だけなので、開いている間は何もしない。開いた
		//     ままの置き土産で見送り続けることはありうるが、そのときもパレットが出ていれば
		//     JS の時計が従来どおり受け付ける。
		//   * **間隔は当てにしない**（平均 250 ms 強・数秒空くことがある）。受け付けの待ち時間は
		//     Python 側が時刻で測っている。
		//   * **起動の直後は待つ。** 起動の最中に本体を読み込みに行かない（パレットの最初の
		//     刻みを遅らせていたのと同じ理由。resources/common.vwr/html/mcp.html の FIRST_TICK_MS）。
		constexpr long long kClockFirstTickSeconds = 10;
		constexpr unsigned kClockIntervalMs = 250;

		long long& ClockStartedAt()
		{
			static long long sStartedAt = 0;
			return sStartedAt;
		}

		// NOLINTBEGIN(bugprone-empty-catch): 黙って次の刻みを待つのが**この場所では正しい**
		// 振る舞い（OS のタイマーのコールバックへ例外を漏らさない。受け付けの失敗は本体が
		// 応答と診断に載せる）。Extensions/ExtMenu.cpp と同じ扱い。
		void ClockTick()
		{
			try
			{
				if (gSDK == nullptr || NowSeconds() < ClockStartedAt() + kClockFirstTickSeconds)
					return;
				if (gSDK->IsCurrentlyBuildingAnUndoEvent())
					return;
				(void)ServeOnce();
			}
			catch (...)
			{
				// OS のタイマーのコールバックへ例外を漏らさない（SDK のコールバックと同じ扱い）。
			}
		}
		// NOLINTEND(bugprone-empty-catch)

#if defined(__APPLE__)
		void OnClock(CFRunLoopTimerRef /*timer*/, void* /*info*/)
		{
			ClockTick();
		}
#elif defined(_WIN32)
		void CALLBACK OnClock(HWND /*hwnd*/, UINT /*msg*/, UINT_PTR /*id*/, DWORD /*time*/)
		{
			ClockTick();
		}
#endif
	} // namespace

	void StartMcpBridgeClock()
	{
		static bool sStarted = false;
		if (sStarted)
			return;
		sStarted = true;
		ClockStartedAt() = NowSeconds();
#if defined(__APPLE__)
		// Vectorworks が終わるまで持ち続ける（解放しない。殻は起動中ずっと読み込まれている）。
		static CFRunLoopTimerRef sTimer = nullptr;
		sTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(),
									  kClockIntervalMs / 1000.0, 0, 0, &OnClock, nullptr);
		if (sTimer != nullptr)
			CFRunLoopAddTimer(CFRunLoopGetMain(), sTimer, kCFRunLoopDefaultMode);
#elif defined(_WIN32)
		// ウィンドウを持たないタイマー。WM_TIMER は Vectorworks のメッセージポンプが配る
		// （同 Findings）。
		(void)SetTimer(nullptr, 0, kClockIntervalMs, &OnClock);
#endif
	}

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
	// **Sync**（Vectorworks のメインスレッドで呼ばれる＝SDK と本体へ安全にアクセスできる）。
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
		context->Reject("unknown function"); // 知らない名前は無視せず JS へエラーを返す
		return;
	}
	// 例外を JS のブリッジへ漏らさない（SDK のコールバックと同じ扱い）。
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

// 中身は .vwr の html/mcp.html（resources/common.vwr から梱包する。CMakeLists.txt）。
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
