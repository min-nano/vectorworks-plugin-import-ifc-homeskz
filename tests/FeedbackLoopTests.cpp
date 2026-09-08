//
//	FeedbackLoopTests.cpp
//
//	モードレスの往復の駆動（src/FeedbackLoop.cpp）の単体テスト。UpdaterFlowTests と同じ
//	作法で、副作用を記録する偽の host を差し込み、**1 回の確認で何をどの順に呼ぶか**と
//	**止まる条件**を無 SDK で確かめる（docs/DEV-NOTES.md M24）。
//

#include "TestFramework.h"
#include "FeedbackLoop.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;

namespace
{
	struct FakeLoopHost : IFeedbackLoopHost
	{
		// --- 答え -----------------------------------------------------------
		bool memoryReadable = true;
		FeedbackLoopMemory memory;
		bool scriptStarts = true;
		std::string loopControlOut = "state=open\ncontrol=none\nok\n";
		FeedbackLoopBuildResult build;
		bool roundStarts = true;
		bool roundPosted = true;

		// --- 記録 -----------------------------------------------------------
		std::vector<std::vector<std::string>> scriptCalls;
		std::vector<std::string> polledBranches;
		int roundCount = 0;
		std::vector<std::string> endReasons;
		std::vector<bool> endNotified;

		bool QueryMemory(FeedbackLoopMemory& out, std::string& error) override
		{
			if (!memoryReadable)
			{
				error = "本体が見つかりません";
				return false;
			}
			out = memory;
			return true;
		}
		bool RunFeedbackScript(const std::vector<std::string>& args, std::string& out) override
		{
			scriptCalls.push_back(args);
			out = loopControlOut;
			return scriptStarts;
		}
		FeedbackLoopBuildResult PollBuild(const std::string& branch) override
		{
			polledBranches.push_back(branch);
			return build;
		}
		bool RunRound(bool& posted, std::string& error) override
		{
			++roundCount;
			posted = roundPosted;
			if (!roundStarts)
			{
				error = "本体を読み込めませんでした";
				return false;
			}
			// 投稿できた周は記憶が進む（本体がそうする）。
			if (posted)
			{
				++memory.round;
				memory.lastCommit = build.commit;
				memory.lastPostedAt = "2026-09-07T02:00:00Z";
			}
			return true;
		}
		void EndLoop(const std::string& reason, bool notifyPr) override
		{
			endReasons.push_back(reason);
			endNotified.push_back(notifyPr);
			memory.active = false;
		}
	};

	FakeLoopHost activeHost()
	{
		FakeLoopHost h;
		h.memory.active = true;
		h.memory.repo = "o/r";
		h.memory.pullRequest = 123;
		h.memory.branch = "feature/x";
		h.memory.round = 2;
		h.memory.lastCommit = "aaa1111";
		h.memory.lastPostedAt = "2026-09-07T01:02:03Z";
		return h;
	}
} // namespace

// ---------------------------------------------------------------------------
// 回っていないとき
// ---------------------------------------------------------------------------

TEST(loop_is_idle_without_an_active_memory)
{
	FakeLoopHost h;
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Idle);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.polledBranches.size()), static_cast<std::size_t>(0));
	CHECK_EQ(h.roundCount, 0);
}

TEST(loop_is_idle_and_says_why_when_the_payload_cannot_be_read)
{
	FakeLoopHost h;
	h.memoryReadable = false;
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Idle);
	CHECK(view.message.find("本体を読めません") != std::string::npos);
}

// ---------------------------------------------------------------------------
// **人が始めるまで回さない**（M25）。パレットの JS タイマーは Vectorworks が生きている
// あいだ回り続け、「閉じる」で隠しても止まらない——記憶が active であることを理由に回すと、
// **誰も押していないのに取り込みが走り出す**（実機で起きた）。

TEST(loop_does_not_run_until_a_menu_round_arms_it)
{
	FakeLoopHost h = activeHost(); // 記憶は「続きがある」と言っている
	FeedbackLoopDriver driver(60);
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Idle);
	CHECK(view.message.find("実機テストを実行") != std::string::npos);
	// 本体にも GitHub にも触らない（記憶すら読みに行かない）。
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.polledBranches.size()), static_cast<std::size_t>(0));
	CHECK_EQ(h.roundCount, 0);

	// メニューが押されたら（Arm / BeginRound）そこから回り出す。
	driver.Arm();
	(void)driver.Tick(h, 1001);
	CHECK(h.scriptCalls.size() > 0);
}

// ---------------------------------------------------------------------------
// 1 回の確認の順序と、間隔
// ---------------------------------------------------------------------------

TEST(loop_checks_the_signal_before_the_build_and_waits_when_nothing_is_new)
{
	FakeLoopHost h = activeHost();
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);

	CHECK(view.phase == FeedbackLoopPhase::Waiting);
	CHECK_EQ(view.round, 2);
	CHECK_EQ(view.build, "aaa1111");
	CHECK_EQ(view.pullRequest, 123);
	CHECK_EQ(view.lastCheckAt, 1000LL);
	CHECK_EQ(view.nextCheckAt, 1060LL);
	// 合図は「自分の投稿より後」だけを読む（since = 直近の投稿の時刻）。
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(1));
	if (!h.scriptCalls.empty())
	{
		const std::vector<std::string>& args = h.scriptCalls[0];
		CHECK_EQ(static_cast<std::size_t>(args.size()), static_cast<std::size_t>(4));
		if (args.size() == 4)
		{
			CHECK_EQ(args[0], "loop-control");
			CHECK_EQ(args[1], "o/r");
			CHECK_EQ(args[2], "123");
			CHECK_EQ(args[3], "2026-09-07T01:02:03Z");
		}
	}
	CHECK_EQ(static_cast<std::size_t>(h.polledBranches.size()), static_cast<std::size_t>(1));
	if (!h.polledBranches.empty())
		CHECK_EQ(h.polledBranches[0], "feature/x");
	CHECK_EQ(h.roundCount, 0);
}

TEST(loop_does_not_hit_github_before_the_interval_has_passed)
{
	FakeLoopHost h = activeHost();
	FeedbackLoopDriver driver(60);
	driver.Arm();
	driver.Tick(h, 1000);
	driver.Tick(h, 1010);
	driver.Tick(h, 1059);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(1));
	driver.Tick(h, 1060);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(2));
}

TEST(loop_arm_and_check_now_skip_the_interval)
{
	FakeLoopHost h = activeHost();
	FeedbackLoopDriver driver(60);
	driver.Arm();
	driver.Tick(h, 1000);
	driver.CheckNow();
	driver.Tick(h, 1001);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(2));
	driver.Arm();
	CHECK(driver.View().phase == FeedbackLoopPhase::Waiting);
	driver.Tick(h, 1002);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(3));
}

// ---------------------------------------------------------------------------
// 新しいビルドが出たら、入れて取り込んで投稿する
// ---------------------------------------------------------------------------

TEST(loop_installs_a_new_build_runs_the_round_and_keeps_going)
{
	FakeLoopHost h = activeHost();
	h.build.outcome = FeedbackLoopBuild::Installed;
	h.build.commit = "bbb2222";
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);

	CHECK_EQ(h.roundCount, 1);
	CHECK(view.phase == FeedbackLoopPhase::Waiting); // まだ回っている
	CHECK_EQ(view.round, 3);						 // 記憶が進んだ
	CHECK_EQ(view.build, "bbb2222");
	CHECK(view.message.find("round 3") != std::string::npos);
	CHECK_EQ(static_cast<std::size_t>(h.endReasons.size()), static_cast<std::size_t>(0));
	// 次の周は新しい投稿より後の合図を読む。
	h.build.outcome = FeedbackLoopBuild::NoNewBuild;
	driver.Tick(h, 1100);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(2));
	if (h.scriptCalls.size() == 2 && h.scriptCalls[1].size() == 4)
		CHECK_EQ(h.scriptCalls[1][3], "2026-09-07T02:00:00Z");
}

TEST(loop_stops_when_the_round_could_not_be_posted)
{
	// 投稿できていない周を重ねると、同じビルドを毎周取り込み直すことになる。
	FakeLoopHost h = activeHost();
	h.build.outcome = FeedbackLoopBuild::Installed;
	h.build.commit = "bbb2222";
	h.roundPosted = false;
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Stopped);
	CHECK_EQ(static_cast<std::size_t>(h.endReasons.size()), static_cast<std::size_t>(1));
	if (!h.endNotified.empty())
		CHECK(!h.endNotified[0]); // 本体が結果ダイアログで伝えている。PR へは重ねない
}

TEST(loop_stops_when_the_round_could_not_start)
{
	FakeLoopHost h = activeHost();
	h.build.outcome = FeedbackLoopBuild::Installed;
	h.build.commit = "bbb2222";
	h.roundStarts = false;
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Stopped);
	CHECK(view.message.find("本体を読み込めませんでした") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 止まる条件
// ---------------------------------------------------------------------------

TEST(loop_stops_on_claudes_signal_without_polling_a_build)
{
	FakeLoopHost h = activeHost();
	h.loopControlOut = "state=open\ncontrol=stop\nok\n";
	h.build.outcome = FeedbackLoopBuild::Installed; // 合図が先。ビルドは見ない
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Stopped);
	CHECK(view.message.find("Claude") != std::string::npos);
	CHECK_EQ(static_cast<std::size_t>(h.polledBranches.size()), static_cast<std::size_t>(0));
	CHECK_EQ(h.roundCount, 0);
	CHECK_EQ(static_cast<std::size_t>(h.endReasons.size()), static_cast<std::size_t>(1));
	if (!h.endNotified.empty())
		CHECK(!h.endNotified[0]); // Claude の合図に返事は要らない
}

TEST(loop_stops_when_the_pr_is_merged_or_closed)
{
	FakeLoopHost merged = activeHost();
	merged.loopControlOut = "state=merged\ncontrol=none\nok\n";
	FeedbackLoopDriver d1(60);
	d1.Arm();
	CHECK(d1.Tick(merged, 1000).phase == FeedbackLoopPhase::Stopped);
	CHECK(d1.View().message.find("マージ") != std::string::npos);

	FakeLoopHost closed = activeHost();
	closed.loopControlOut = "state=closed\ncontrol=none\nok\n";
	FeedbackLoopDriver d2(60);
	d2.Arm();
	CHECK(d2.Tick(closed, 1000).phase == FeedbackLoopPhase::Stopped);
	CHECK(d2.View().message.find("閉じ") != std::string::npos);
}

TEST(loop_keeps_waiting_when_the_check_itself_failed)
{
	// オフライン・GitHub の不調は止める理由ではない。次の周期にもう一度見る。
	FakeLoopHost h = activeHost();
	h.loopControlOut = "error=PR の状態を取得できませんでした（ネットワークか権限）。\n";
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Waiting);
	CHECK_EQ(view.nextCheckAt, 1060LL);
	CHECK_EQ(static_cast<std::size_t>(h.endReasons.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.polledBranches.size()), static_cast<std::size_t>(0));

	FakeLoopHost h2 = activeHost();
	h2.build.outcome = FeedbackLoopBuild::CheckFailed;
	h2.build.message = "リリース一覧を取得できませんでした。";
	FeedbackLoopDriver d2(60);
	d2.Arm();
	CHECK(d2.Tick(h2, 1000).phase == FeedbackLoopPhase::Waiting);
	CHECK(d2.View().message.find("リリース一覧") != std::string::npos);

	FakeLoopHost h3 = activeHost();
	h3.scriptStarts = false;
	FeedbackLoopDriver d3(60);
	d3.Arm();
	CHECK(d3.Tick(h3, 1000).phase == FeedbackLoopPhase::Waiting);
}

TEST(loop_stops_when_the_shell_changed_or_the_install_failed)
{
	FakeLoopHost h = activeHost();
	h.build.outcome = FeedbackLoopBuild::NeedsRestart;
	h.build.commit = "bbb2222";
	h.build.message = "殻まで変わりました。";
	FeedbackLoopDriver driver(60);
	driver.Arm();
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Stopped);
	CHECK(view.message.find("bbb2222") != std::string::npos);
	CHECK(view.message.find("再起動") != std::string::npos);
	CHECK_EQ(h.roundCount, 0);
	if (!h.endNotified.empty())
		CHECK(h.endNotified[0]); // 読む側が待ち続けないよう PR へ伝える

	FakeLoopHost h2 = activeHost();
	h2.build.outcome = FeedbackLoopBuild::Failed;
	h2.build.message = "zip を展開できませんでした。";
	FeedbackLoopDriver d2(60);
	d2.Arm();
	CHECK(d2.Tick(h2, 1000).phase == FeedbackLoopPhase::Stopped);
	CHECK(d2.View().message.find("zip") != std::string::npos);
}

TEST(loop_stop_button_ends_the_loop_and_tells_the_pr)
{
	FakeLoopHost h = activeHost();
	FeedbackLoopDriver driver(60);
	driver.Arm();
	driver.Tick(h, 1000);
	const FeedbackLoopView view = driver.Stop(h, "利用者がパレットで止めました");
	CHECK(view.phase == FeedbackLoopPhase::Stopped);
	CHECK_EQ(static_cast<std::size_t>(h.endReasons.size()), static_cast<std::size_t>(1));
	if (!h.endNotified.empty())
		CHECK(h.endNotified[0]);
	// **止めたあとの Tick は本体にも GitHub にも触らない**（武装が解けている。M25）。
	// 見え方も Stopped のまま——止めた理由を、押した人が読めるうちは消さない。
	const std::size_t before = h.scriptCalls.size();
	driver.Tick(h, 2000);
	CHECK(driver.View().phase == FeedbackLoopPhase::Stopped);
	CHECK_EQ(h.scriptCalls.size(), before);
}

TEST(loop_stops_without_a_pull_request_number)
{
	FakeLoopHost h = activeHost();
	h.memory.pullRequest = 0;
	FeedbackLoopDriver driver(60);
	driver.Arm();
	CHECK(driver.Tick(h, 1000).phase == FeedbackLoopPhase::Stopped);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// **メニューの周が走っている間は何もしない**（M25）。パレットはコマンドを押した直後に
// 開くので、取り込みの最中も JS のタイマーが Tick を叩きうる——素通しすると駆動が
// 2 周目を始めようとして本体を降ろしにいく。

TEST(feedback_loop_does_nothing_while_a_menu_round_is_running)
{
	FakeLoopHost h;
	h.memory.active = true;
	h.memory.pullRequest = 110;
	h.memory.branch = "b";
	FeedbackLoopDriver driver(60);

	driver.BeginRound();
	driver.SetExternalBusy(true);
	// **間隔を待たない指定（BeginRound の中で立つ）でも動かない。**
	const FeedbackLoopView during = driver.Tick(h, 1000);
	CHECK(during.phase == FeedbackLoopPhase::Working);
	// 合図も見に行かない（スクリプトを 1 度も起動しない）＝本体を降ろしにも行かない。
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.polledBranches.size()), static_cast<std::size_t>(0));
	CHECK_EQ(h.roundCount, 0);

	// 番人が外れても、**投稿できるまでは回らない**（BeginRound は武装しない。M25）。
	driver.SetExternalBusy(false);
	(void)driver.Tick(h, 1001);
	CHECK_EQ(static_cast<std::size_t>(h.scriptCalls.size()), static_cast<std::size_t>(0));
	// 投稿できた周のあと（ExtTestMenu の ArmFeedbackLoop）から回り出す。
	driver.Arm();
	(void)driver.Tick(h, 1002);
	CHECK(h.scriptCalls.size() > 0);
}

TEST(feedback_loop_does_not_stay_working_when_the_menu_round_was_cancelled)
{
	// キャンセル・「送らない」で周が止まると Arm へ来ない。**「実行しています…」で
	// 固まらせない**——止まったのに動いているように見えるのが一番たちが悪い。
	FakeLoopHost h = activeHost();
	FeedbackLoopDriver driver(60);
	driver.BeginRound();
	driver.SetExternalBusy(true);
	driver.SetExternalBusy(false);
	const FeedbackLoopView view = driver.Tick(h, 1000);
	CHECK(view.phase == FeedbackLoopPhase::Idle);
	CHECK_EQ(h.roundCount, 0);
}

TEST(feedback_loop_begin_round_shows_that_it_is_running)
{
	// **開いた瞬間に「回っていません」と出さない。** コマンドを押した直後はまだ何も
	// 投稿できていないが、そこで「往復は回っていません」と出ると押した人が失敗したと読む。
	FeedbackLoopDriver driver(60);
	driver.BeginRound();
	CHECK(driver.View().phase == FeedbackLoopPhase::Working);
	CHECK(driver.View().message.find("実機テスト") != std::string::npos);
}

TEST_MAIN();
