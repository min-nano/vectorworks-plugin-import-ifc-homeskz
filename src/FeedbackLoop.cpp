//
//	FeedbackLoop.cpp
//
//	モードレスの往復の駆動（意図は FeedbackLoop.h 参照）。**SDK のヘッダを include しない**
//	——UpdaterFlow.cpp と同じく、殻とテストの両方へそのままリンクされる。
//

#include "FeedbackLoop.h"
#include "UpdaterParse.h"

#include <string>
#include <vector>

using namespace HomeskzIfcImport::UpdaterParse;

namespace HomeskzIfcImport
{
	namespace
	{
		// 走っている間だけ true にする番人（例外で抜けても戻す）。
		class BusyGuard
		{
		public:
			explicit BusyGuard(bool& flag) : fFlag(flag)
			{
				fFlag = true;
			}
			~BusyGuard()
			{
				fFlag = false;
			}
			BusyGuard(const BusyGuard&) = delete;
			BusyGuard& operator=(const BusyGuard&) = delete;

		private:
			bool& fFlag;
		};

		// 武装していないあいだパレットに出しておく 1 行。
		const char* const kNotRunning =
			"往復は回っていません。メニューの「実機テストを実行…」で始まります。";

		// **CopyMemory とは名付けない**——Windows.h（PluginPrefix.h 経由）が同名のマクロを
		// 定義していて、殻のビルドでここが memcpy に化ける（build-windows で実際に落ちた）。
		void ApplyMemory(const FeedbackLoopMemory& memory, FeedbackLoopView& view)
		{
			view.repo = memory.repo;
			view.pullRequest = memory.pullRequest;
			view.branch = memory.branch;
			view.round = memory.round;
			view.build = memory.lastCommit;
		}
	} // namespace

	FeedbackLoopDriver::FeedbackLoopDriver(long long checkIntervalSeconds)
		: fInterval(checkIntervalSeconds < 1 ? 1 : checkIntervalSeconds)
	{
		fView.message = kNotRunning;
	}

	void FeedbackLoopDriver::Arm()
	{
		fArmed = true;
		fForceCheck = true;
		// 止まったあとにメニューからもう一度往復へ入った——見え方も Waiting へ戻す
		// （Stopped の文言が残ったままだと、回り出したのに止まって見える）。
		fView.phase = FeedbackLoopPhase::Waiting;
		fView.message = "往復に入りました。新しいビルドを確認します。";
	}

	void FeedbackLoopDriver::CheckNow()
	{
		fForceCheck = true;
	}

	void FeedbackLoopDriver::BeginRound()
	{
		// **ここでは武装しない。** 押した周は途中で止まりうる（ファイル選択のキャンセル・
		// 「送らない」）ので、武装するのは**投稿できて往復が成立したとき**＝ Arm だけに
		// する。見え方だけ「走っています」にしておく。
		fView.phase = FeedbackLoopPhase::Working;
		fView.message = "実機テストを実行しています…（終わったら結果を PR へ投稿します）";
		fView.nextCheckAt = -1;
		// 周が終わったら間隔を待たずに見る（新しいビルドがもう出ているかもしれない）。
		fForceCheck = true;
	}

	FeedbackLoopView FeedbackLoopDriver::Tick(IFeedbackLoopHost& host, long long now)
	{
		// **走っている間は何もしない**（FeedbackLoop.h「再入」）。fExternalBusy は
		// メニューの実機テストが取り込みを回している間で、そこを素通しすると駆動が
		// 2 周目を始めようとして本体を降ろしにいく（FeedbackLoop.h「外から走っている間」）。
		if (fBusy || fExternalBusy)
			return fView;

		// **人が始めていない往復は回さない**（FeedbackLoop.h「回り出す条件」）。パレットの
		// 時計は「閉じる」で隠しても止まらないので、ここを素通しすると**誰も押していない
		// のに取り込みが走り出す**（実機で起きた）。記憶が active でも同じ——記憶は
		// 「続きがある」であって「いま回っている」ではない。
		if (!fArmed)
		{
			// 押した周が途中で止まったとき（BeginRound だけ済んで Arm へ来なかった）、
			// 「実行しています…」で固まらせない。
			if (fView.phase == FeedbackLoopPhase::Working)
			{
				fView.phase = FeedbackLoopPhase::Idle;
				fView.message = kNotRunning;
			}
			return fView;
		}

		const bool due = fForceCheck || fLastCheck < 0 || now - fLastCheck >= fInterval;
		if (!due)
		{
			fView.nextCheckAt = fLastCheck + fInterval;
			return fView;
		}
		fForceCheck = false;

		const BusyGuard busy(fBusy);
		Check(host, now);
		return fView;
	}

	FeedbackLoopView FeedbackLoopDriver::Stop(IFeedbackLoopHost& host, const std::string& reason)
	{
		if (fBusy)
			return fView; // 取り込みの最中。終わってから押し直してもらう
		const BusyGuard busy(fBusy);
		// 記憶が無いのに止めても意味は無いが、害も無い（本体は loop を下ろすだけで、
		// 既に下りていれば PR にも投稿しない。draw/Feedback.cpp の endFeedbackLoop）。
		Stopped(host, reason, /*notifyPr*/ true);
		return fView;
	}

	void FeedbackLoopDriver::Stopped(IFeedbackLoopHost& host, const std::string& reason,
									 bool notifyPr)
	{
		// **止まったら武装も解く。** ここを落とすと、隠れたパレットの時計が次の Tick で
		// もう一度回し始める。
		fArmed = false;
		host.EndLoop(reason, notifyPr);
		fView.phase = FeedbackLoopPhase::Stopped;
		fView.message = "往復を終えました（" + reason +
						"）。続きは、メニューの「実機テストを実行…」を"
						"もう一度実行すると同じ条件で走ります。";
		fView.nextCheckAt = -1;
	}

	void FeedbackLoopDriver::Check(IFeedbackLoopHost& host, long long now)
	{
		fLastCheck = now;
		fView.lastCheckAt = now;
		fView.nextCheckAt = -1;

		// 0. 記憶。本体を読めなければ Idle（パレットにはその理由を出す）。
		FeedbackLoopMemory memory;
		std::string error;
		if (!host.QueryMemory(memory, error))
		{
			fView.phase = FeedbackLoopPhase::Idle;
			fView.message = "本体を読めません: " + error;
			return;
		}
		ApplyMemory(memory, fView);
		if (!memory.active)
		{
			fView.phase = FeedbackLoopPhase::Idle;
			fView.message = "往復は回っていません。メニューの「実機テストを実行…」で"
							"選ぶと始まります。";
			return;
		}
		if (memory.pullRequest <= 0)
		{
			Stopped(host, "投稿先の PR が分かりません", /*notifyPr*/ false);
			return;
		}

		// 1. 合図。**確認できなかっただけなら止めない**（オフラインは次の周期に持ち越す）。
		fView.phase = FeedbackLoopPhase::Working;
		std::string out;
		const bool ran = host.RunFeedbackScript(
			{"loop-control", memory.repo, std::to_string(memory.pullRequest), memory.lastPostedAt},
			out);
		if (!ran || !ValueOf(out, "error").empty())
		{
			fView.phase = FeedbackLoopPhase::Waiting;
			fView.message =
				"PR を確認できませんでした（" +
				(ran ? ValueOf(out, "error") : std::string("スクリプトを起動できません")) +
				"）。次の周期にもう一度見ます。";
			fView.nextCheckAt = now + fInterval;
			return;
		}
		const std::string state = ValueOf(out, "state");
		if (state == "merged" || state == "closed")
		{
			Stopped(host,
					"PR #" + std::to_string(memory.pullRequest) +
						(state == "merged" ? " がマージされました" : " が閉じました"),
					/*notifyPr*/ false);
			return;
		}
		if (ValueOf(out, "control") == "stop")
		{
			Stopped(host, "Claude が「もう要らない」と合図しました", /*notifyPr*/ false);
			return;
		}

		// 2. 新しいビルド。
		const FeedbackLoopBuildResult build = host.PollBuild(memory.branch);
		switch (build.outcome)
		{
		case FeedbackLoopBuild::NoNewBuild:
			fView.phase = FeedbackLoopPhase::Waiting;
			fView.message = "`" + memory.branch + "` の新しいビルドを待っています（round " +
							std::to_string(memory.round) + " は " + memory.lastCommit + "）。";
			fView.nextCheckAt = now + fInterval;
			return;
		case FeedbackLoopBuild::CheckFailed:
			fView.phase = FeedbackLoopPhase::Waiting;
			fView.message =
				"ビルドを確認できませんでした（" + build.message + "）。次の周期にもう一度見ます。";
			fView.nextCheckAt = now + fInterval;
			return;
		case FeedbackLoopBuild::NeedsRestart:
			Stopped(host,
					"新しいビルド " + build.commit + " を入れましたが、" + build.message +
						" 再起動してから「実機テストを実行…」で続きから走ります",
					/*notifyPr*/ true);
			return;
		case FeedbackLoopBuild::Failed:
			Stopped(host, "新しいビルドを入れられませんでした: " + build.message,
					/*notifyPr*/ true);
			return;
		case FeedbackLoopBuild::Installed:
			break;
		}

		// 3. 取り込み（本体）。新しい本体は次の PayloadUse で読み直される。
		fView.message = "新しいビルド " + build.commit + " を入れました。取り込んでいます…";
		bool posted = false;
		if (!host.RunRound(posted, error))
		{
			Stopped(host, "取り込みを開始できませんでした: " + error, /*notifyPr*/ true);
			return;
		}
		if (!posted)
		{
			// 本体が結果ダイアログで理由を出し終えている（投稿の失敗は結果へ添えられる）。
			Stopped(host,
					"round " + std::to_string(memory.round + 1) +
						" を投稿できませんでした（結果ダイアログを参照）",
					/*notifyPr*/ false);
			return;
		}

		// 投稿できた。記憶を読み直して見え方を更新する（round が進んでいる）。
		FeedbackLoopMemory after;
		if (host.QueryMemory(after, error))
			ApplyMemory(after, fView);
		fView.phase = FeedbackLoopPhase::Waiting;
		fView.message = "round " + std::to_string(fView.round) + "（" + fView.build +
						"）を投稿しました。次のビルドを待っています。";
		fView.nextCheckAt = now + fInterval;
	}
} // namespace HomeskzIfcImport
