//
//	FeedbackLoopHost.cpp
//
//	FeedbackLoopHost.h の実装（意図はそちら）。駆動（src/FeedbackLoop.cpp）が呼ぶ副作用を、
//	殻の道具で 1 つずつ実装する:
//
//	  QueryMemory       … 本体の vw_payload_loop_status（PayloadUse で確保して呼ぶ）
//	  RunFeedbackScript … 同梱スクリプト vw-feedback（src/Updater.h の RunBundledScriptNamed）
//	  PollBuild         … 尋ねない自動アップデート（src/Updater.h の PollDevBuild）
//	  RunRound          … 本体の実機テスト 1 周（Extensions/ExtTestMenu.cpp と同じ呼び方）
//	  EndLoop           … 本体の vw_payload_loop_end
//
//	**本体を使う区間（PayloadUse）は 1 操作ごとに閉じる。** PollBuild は入れたあとに本体を
//	降ろすので（src/UpdaterFlow.cpp）、そのとき本体のコードがスタックに載っていてはならない
//	——QueryMemory の PayloadUse をまたいで PollBuild を呼ぶと、降ろせずに Failed になる。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "FeedbackLoopHost.h"

#include "Extensions/ExtFeedbackPalette.h"
#include "FeedbackLoop.h"
#include "PayloadSession.h"
#include "Updater.h"
#include "UpdaterHost.h"
#include "UpdaterParse.h"

#include <chrono>
#include <string>
#include <vector>

using namespace HomeskzIfcImport::UpdaterParse;

namespace HomeskzIfcImport
{
	namespace
	{
		// GitHub を見に行く間隔（秒）。CI の dev ビルドは push から十数分かかるので、
		// 1 分ごとで十分に早い。短くしすぎると GitHub の API 制限と、同梱スクリプトを
		// 起動するあいだ（1〜2 秒）メインスレッドが止まる回数が増える。
		constexpr long long kCheckIntervalSeconds = 60;

		// 単調な秒（起点は問わない）。
		long long NowSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
					   std::chrono::steady_clock::now().time_since_epoch())
				.count();
		}

		int ParseIntOr(const std::string& text, int fallback)
		{
			if (text.empty())
				return fallback;
			int value = 0;
			for (const char c : text)
			{
				if (c < '0' || c > '9')
					return fallback;
				if (value > 214748363)
					return fallback;
				value = value * 10 + (c - '0');
			}
			return value;
		}

		// JSON の文字列リテラル（引用符・バックスラッシュ・制御文字をエスケープ）。
		std::string JsonString(const std::string& text)
		{
			std::string out = "\"";
			for (const char c : text)
			{
				switch (c)
				{
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
					{
						static const char* const kHex = "0123456789abcdef";
						out += "\\u00";
						out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
						out += kHex[static_cast<unsigned char>(c) & 0xF];
					}
					else
						out += c;
					break;
				}
			}
			out += "\"";
			return out;
		}

		const char* PhaseName(FeedbackLoopPhase phase)
		{
			switch (phase)
			{
			case FeedbackLoopPhase::Idle:
				return "idle";
			case FeedbackLoopPhase::Waiting:
				return "waiting";
			case FeedbackLoopPhase::Working:
				return "working";
			case FeedbackLoopPhase::Stopped:
				return "stopped";
			}
			return "idle";
		}

		// -------------------------------------------------------------------
		// 殻の道具で駆動の副作用を実装する。
		class CShellFeedbackLoopHost : public IFeedbackLoopHost
		{
		public:
			bool QueryMemory(FeedbackLoopMemory& out, std::string& error) override
			{
				out = FeedbackLoopMemory{};
				const PayloadUse use;
				if (!use.ok())
				{
					error = use.error();
					return false;
				}
				std::string text;
				if (!use->loopStatus(text, error))
					return false;
				out.active = ValueOf(text, "active") == "1";
				out.repo = ValueOf(text, "repo");
				out.pullRequest = ParseIntOr(ValueOf(text, "pr"), 0);
				out.branch = ValueOf(text, "branch");
				out.round = ParseIntOr(ValueOf(text, "round"), 0);
				out.lastCommit = ValueOf(text, "build");
				out.lastPostedAt = ValueOf(text, "posted");
				return true;
			}

			bool RunFeedbackScript(const std::vector<std::string>& args, std::string& out) override
			{
				return RunBundledScriptNamed("vw-feedback", args, out);
			}

			FeedbackLoopBuildResult PollBuild(const std::string& /*branch*/) override
			{
				// ブランチは殻にコンパイルされたもの（VW_BUILD_BRANCH）を使う——記憶の
				// ブランチは本体がそれと同じときにしか active にしない（draw/Feedback.cpp
				// の loadFeedbackSession）ので、ここで引数を使う理由が無い。
				const DevBuildPollResult r = PollDevBuild();
				FeedbackLoopBuildResult result;
				result.commit = r.commit;
				result.message = r.message;
				switch (r.outcome)
				{
				case DevBuildPoll::NoNewBuild:
					result.outcome = FeedbackLoopBuild::NoNewBuild;
					break;
				case DevBuildPoll::Installed:
					result.outcome = FeedbackLoopBuild::Installed;
					break;
				case DevBuildPoll::NeedsRestart:
					result.outcome = FeedbackLoopBuild::NeedsRestart;
					break;
				case DevBuildPoll::Failed:
					result.outcome = FeedbackLoopBuild::Failed;
					break;
				case DevBuildPoll::CheckFailed:
					result.outcome = FeedbackLoopBuild::CheckFailed;
					break;
				}
				return result;
			}

			bool RunRound(bool& posted, std::string& error) override
			{
				posted = false;
				// **入れ替えはここで効く。** PollBuild が降ろしたあとなので、この PayloadUse が
				// 新しい本体を読み直す（src/PayloadSession.h）。
				const PayloadUse use;
				if (!use.ok())
				{
					error = use.error();
					return false;
				}
				// **往復を知っているのは本体のテストの周だけ**（M25。src/draw/Feedback.h）。
				// ダイアログは 1 枚も出さない。
				return use->runTest(/*allowDialogs*/ false, posted, error);
			}

			void EndLoop(const std::string& reason, bool notifyPr) override
			{
				const PayloadUse use;
				if (!use.ok())
					return;
				std::string error;
				(void)use->endLoop(reason, notifyPr, error);
			}
		};
	} // namespace

	FeedbackLoopDriver& TheFeedbackLoop()
	{
		// 関数ローカル static（静的初期化順序に依存しない。PayloadSession と同じ作法）。
		static FeedbackLoopDriver sDriver(kCheckIntervalSeconds);
		return sDriver;
	}

	void ArmFeedbackLoop()
	{
		TheFeedbackLoop().Arm();
		ShowFeedbackPalette(true);
	}

	void BeginFeedbackRound()
	{
		TheFeedbackLoop().BeginRound();
		ShowFeedbackPalette(true);
	}

	FeedbackLoopBusyScope::FeedbackLoopBusyScope()
	{
		TheFeedbackLoop().SetExternalBusy(true);
	}

	FeedbackLoopBusyScope::~FeedbackLoopBusyScope()
	{
		TheFeedbackLoop().SetExternalBusy(false);
	}

	FeedbackLoopView FeedbackLoopTick()
	{
		CShellFeedbackLoopHost host;
		return TheFeedbackLoop().Tick(host, NowSeconds());
	}

	FeedbackLoopView FeedbackLoopStop()
	{
		CShellFeedbackLoopHost host;
		return TheFeedbackLoop().Stop(host, "利用者がパレットで止めました");
	}

	FeedbackLoopView FeedbackLoopStopForClose()
	{
		CShellFeedbackLoopHost host;
		// **隠さない**（FeedbackLoopHost.h）。隠す順序は呼び出し側が握る。
		return TheFeedbackLoop().Stop(host, "利用者がパレットを閉じました");
	}

	FeedbackLoopView FeedbackLoopCheckNow()
	{
		TheFeedbackLoop().CheckNow();
		return FeedbackLoopTick();
	}

	void ShowFeedbackPalette(bool visible)
	{
#ifdef VW_DEV_BUILD
		if (gSDK == nullptr)
			return;
		// **実機未確認の呼び出し**（SDK リファレンス Findings「モードレス（非モーダル）な
		// パレット」。ヘッダ根拠のみ）。効かなければ何も出ないだけで、往復の手動の周
		// （メニュー）は従来どおり動く。
		gSDK->SetWebPaletteVisibility(CExtFeedbackPalette::_GetIID(), visible);
#else
		(void)visible;
#endif
	}

	std::string FeedbackLoopViewJson(const FeedbackLoopView& view, long long now)
	{
		const long long sinceCheck = view.lastCheckAt >= 0 ? now - view.lastCheckAt : -1;
		const long long untilNext = view.nextCheckAt >= 0 ? view.nextCheckAt - now : -1;
		std::string out = "{";
		out += "\"phase\":" + JsonString(PhaseName(view.phase));
		out += ",\"message\":" + JsonString(view.message);
		out += ",\"repo\":" + JsonString(view.repo);
		out += ",\"pr\":" + std::to_string(view.pullRequest);
		out += ",\"branch\":" + JsonString(view.branch);
		out += ",\"round\":" + std::to_string(view.round);
		out += ",\"build\":" + JsonString(view.build);
		out += ",\"secondsSinceCheck\":" + std::to_string(sinceCheck);
		out += ",\"secondsUntilNext\":" + std::to_string(untilNext < 0 ? -1 : untilNext);
		out += "}";
		return out;
	}
} // namespace HomeskzIfcImport
