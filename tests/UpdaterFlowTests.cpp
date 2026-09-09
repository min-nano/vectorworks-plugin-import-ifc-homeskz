//
//	UpdaterFlowTests.cpp
//
//	Tests for the update FLOWS (src/UpdaterFlow.cpp): RunStableUpdateCheckWith
//	and RunDevUpdateCheckWith. They are driven through a FAKE IUpdaterHost that
//	records every call and returns canned answers, so the entire flow — each
//	branch and the exact dialog wording — is exercised WITHOUT the Vectorworks
//	SDK. This is still a unit test: the flow is the unit, the fake host is a test
//	double. (An end-to-end test would run the real plug-in inside Vectorworks
//	against the live GitHub API.)
//

#include "TestFramework.h"
#include "UpdaterHost.h"

#include <string>
#include <vector>

using namespace HomeskzIfcImport;

namespace
{
	// A programmable, recording IUpdaterHost.
	struct FakeHost : IUpdaterHost
	{
		// --- Programmable answers -------------------------------------------
		// stdout returned by RunScript, keyed by the mode (args[0]).
		std::string qStableOut;
		std::string qDevOut;
		std::string doInstallOut;
		// Whether RunScript "starts" for a given mode (false -> could not start).
		bool qStableStarts = true;
		bool qDevStarts = true;
		bool doInstallStarts = true;
		bool askAnswer = true; // what Ask returns once askAnswers runs out
		// Answers for the first N Ask calls, in order (a flow can ask twice:
		// "install?" and then "restart?"). Anything beyond falls back to
		// askAnswer.
		std::vector<bool> askAnswers;
		int pickAnswer = 0;		   // what PickBuild returns
		bool restartAnswer = true; // what Restart returns (false -> could not be arranged)
		// 本体（ペイロード）を降ろせたか。false は「まだ走っているので降ろせなかった」
		// ＝反映は次の起動、という分岐（src/UpdaterHost.h の DropLoadedPayload）。
		bool dropAnswer = true;

		// --- Recorded interactions ------------------------------------------
		std::vector<std::vector<std::string>> scriptCalls;
		std::vector<std::vector<std::string>> informs; // {text, advice}
		// {text, advice, okText, cancelText} of every Ask, in order.
		std::vector<std::vector<std::string>> asks;
		int askCount = 0;
		int pickCount = 0;
		int restartCount = 0;
		int dropCount = 0;
		std::vector<std::string> lastPickItems;

		bool RunScript(const std::vector<std::string>& args, std::string& out) override
		{
			scriptCalls.push_back(args);
			const std::string mode = args.empty() ? "" : args[0];
			out.clear();
			if (mode == "q-stable")
			{
				if (!qStableStarts)
					return false;
				out = qStableOut;
				return true;
			}
			if (mode == "q-dev")
			{
				if (!qDevStarts)
					return false;
				out = qDevOut;
				return true;
			}
			if (mode == "do-install")
			{
				if (!doInstallStarts)
					return false;
				out = doInstallOut;
				return true;
			}
			return true;
		}

		void Inform(const std::string& text, const std::string& advice) override
		{
			informs.push_back({text, advice});
		}

		bool Ask(const std::string& text, const std::string& advice, const std::string& okText,
				 const std::string& cancelText) override
		{
			const std::size_t i = asks.size();
			asks.push_back({text, advice, okText, cancelText});
			++askCount;
			return i < askAnswers.size() ? askAnswers[i] : askAnswer;
		}

		int PickBuild(const std::vector<std::string>& items, int) override
		{
			++pickCount;
			lastPickItems = items;
			return pickAnswer;
		}

		bool Restart() override
		{
			++restartCount;
			return restartAnswer;
		}

		bool DropLoadedPayload() override
		{
			++dropCount;
			return dropAnswer;
		}

		// Convenience: how many times a given mode was invoked.
		int CountScript(const std::string& mode) const
		{
			int n = 0;
			for (const auto& c : scriptCalls)
				if (!c.empty() && c[0] == mode)
					++n;
			return n;
		}
		// The args of the (first) do-install call, or empty if none.
		std::vector<std::string> DoInstallArgs() const
		{
			for (const auto& c : scriptCalls)
				if (!c.empty() && c[0] == "do-install")
					return c;
			return {};
		}
	};
	// いま動いている殻の ID（実物ではコンパイル時に焼かれる VW_SHELL_ID）。既定の
	// do-install 出力は "installed-shell=" の行を持たないので、下のほとんどのテストは
	// 「殻の新旧を判断できない＝安全側の再起動」を通る（src/UpdaterParse.h の
	// NeedsRestartAfterInstall）。ホットリロードの分岐だけ、その行を足して確かめる。
	constexpr const char* kRunningShell = "aaaa1111bbbb";
} // namespace

// ---------------------------------------------------------------------------
// Stable flow
// ---------------------------------------------------------------------------

TEST(stable_silent_check_says_nothing_when_script_cannot_start)
{
	FakeHost h;
	h.qStableStarts = false;
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);
	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(stable_silent_check_says_nothing_when_already_current)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=abc1234\n"
				   "url=https://ex.com/x.zip\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);
	CHECK_EQ(h.askCount, 0); // no dialog when up to date
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(stable_silent_check_says_nothing_on_error_line)
{
	FakeHost h;
	h.qStableOut = "error=offline\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);
	CHECK_EQ(h.askCount, 0);
}

TEST(stable_declined_does_not_install)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/x.zip\n";
	h.askAnswer = false; // user chose "後で"
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);
	CHECK_EQ(h.askCount, 1);				  // was asked
	CHECK_EQ(h.CountScript("do-install"), 0); // but nothing installed
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(stable_accepted_and_install_succeeds)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/min-nano_structure.zip\n";
	h.askAnswers = {true, false}; // install: yes, restart: later
	h.doInstallOut = "ok";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	CHECK_EQ(h.CountScript("do-install"), 1);
	// Installed the right asset under the stable name.
	std::vector<std::string> args = h.DoInstallArgs();
	CHECK_EQ(static_cast<std::size_t>(args.size()), static_cast<std::size_t>(3));
	if (args.size() == 3)
	{
		CHECK_EQ(args[1], "https://ex.com/min-nano_structure.zip");
		CHECK_EQ(args[2], "min-nano_structure");
	}
	// Success is reported by the restart QUESTION (a plain notice would leave the
	// user to work out that a restart is needed), not by an Inform.
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.asks.size()), static_cast<std::size_t>(2));
	if (h.asks.size() == 2)
	{
		CHECK_EQ(h.asks[1][0], "みんなの構造設計支援を更新しました。");
		CHECK_EQ(h.asks[1][1], "build: def5678\n\n"
							   "反映するには Vectorworks の再起動が必要です。\n"
							   "今すぐ再起動しますか？（開いているファイルは保存を確認します）");
		CHECK_EQ(h.asks[1][2], "再起動");
		CHECK_EQ(h.asks[1][3], "後で");
	}
	// The user picked 後で, so nothing was restarted.
	CHECK_EQ(h.restartCount, 0);
}

TEST(stable_restart_button_restarts_vectorworks)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/min-nano_structure.zip\n";
	h.askAnswer = true; // says yes to both questions: install, then restart
	h.doInstallOut = "ok";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	CHECK_EQ(h.CountScript("do-install"), 1);
	CHECK_EQ(h.restartCount, 1);
	// It worked, so the user is not told anything further.
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(stable_restart_that_cannot_be_arranged_is_reported)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/min-nano_structure.zip\n";
	h.askAnswer = true;
	h.doInstallOut = "ok";
	h.restartAnswer = false; // e.g. the relaunch helper would not start
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	CHECK_EQ(h.restartCount, 1);
	// Pressing 再起動 must not look like it did nothing.
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
		CHECK_EQ(h.informs[0][0], "再起動できませんでした。");
}

TEST(stable_accepted_but_install_reports_error)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/x.zip\n";
	h.askAnswer = true;
	h.doInstallOut = "error=ダウンロードに失敗しました。\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "更新に失敗しました。");
		// The script's own error message is surfaced as the advice line.
		CHECK_EQ(h.informs[0][1], "ダウンロードに失敗しました。");
	}
	// Nothing was installed, so no restart is offered (the single Ask was the
	// install question).
	CHECK_EQ(h.askCount, 1);
	CHECK_EQ(h.restartCount, 0);
}

TEST(stable_accepted_but_installer_cannot_start)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/x.zip\n";
	h.askAnswer = true;
	h.doInstallStarts = false; // installer could not be launched
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "更新に失敗しました。");
		CHECK_EQ(h.informs[0][1], "アップデータを起動できませんでした。");
	}
}

// ---------------------------------------------------------------------------
// Dev flow
// ---------------------------------------------------------------------------

TEST(dev_manual_check_reports_when_script_cannot_start)
{
	FakeHost h;
	h.qDevStarts = false;
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);
	CHECK_EQ(h.pickCount, 0);
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(dev_manual_check_reports_an_error_line)
{
	FakeHost h;
	h.qDevOut = "error=リリース一覧を取得できませんでした。\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);
	CHECK_EQ(h.pickCount, 0);
}

TEST(dev_manual_check_says_so_when_no_prereleases_exist)
{
	FakeHost h;
	// The script returned no build rows at all.
	h.qDevOut = "installed=run1234\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);
	CHECK_EQ(h.pickCount, 0); // nothing to choose -> no picker
	CHECK_EQ(h.CountScript("do-install"), 0);
	// **黙って終わらない。** 手で押したコマンドなので、選べるものが無いことを言う。
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "ほかに選べる開発版ビルドはありません。");
		CHECK_EQ(h.informs[0][1], "現在: main (run1234)");
	}
}

TEST(dev_manual_check_says_so_when_only_prerelease_is_the_running_build)
{
	FakeHost h;
	// The only prerelease is the build already loaded (same commit).
	h.qDevOut = "installed=run1234\n"
				"build\trun1234\tmain\thttps://ex.com/main.zip\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);
	CHECK_EQ(h.pickCount, 0); // no alternative build -> no picker
	CHECK_EQ(h.CountScript("do-install"), 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
		CHECK_EQ(h.informs[0][0], "ほかに選べる開発版ビルドはありません。");
}

TEST(dev_picker_lists_current_first_then_other_builds)
{
	FakeHost h;
	// The running build (run1234) plus two other branches.
	h.qDevOut = "installed=run1234\n"
				"build\trun1234\tmain\thttps://ex.com/main.zip\n"
				"build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n"
				"build\tbbb2222\tfeature/y\thttps://ex.com/y.zip\n";
	h.pickAnswer = 0; // keep current
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);

	CHECK_EQ(h.pickCount, 1);
	// Entry 0 is the running build; the running build is NOT repeated among the
	// candidates, so 3 entries total (current + 2 others).
	CHECK_EQ(static_cast<std::size_t>(h.lastPickItems.size()), static_cast<std::size_t>(3));
	if (h.lastPickItems.size() == 3)
	{
		CHECK_EQ(h.lastPickItems[0], "現在: main (run1234) ― インストール済み");
		CHECK_EQ(h.lastPickItems[1], "feature/x  (aaa1111)");
		CHECK_EQ(h.lastPickItems[2], "feature/y  (bbb2222)");
	}
	// Kept current -> nothing installed.
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(dev_cancelled_does_not_install)
{
	FakeHost h;
	h.qDevOut = "build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	h.pickAnswer = -1; // cancelled the dialog
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);
	CHECK_EQ(h.CountScript("do-install"), 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_selecting_a_build_installs_it)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n"
				"build\tbbb2222\tfeature/y\thttps://ex.com/y.zip\n";
	h.pickAnswer = 2;		// entry 2 -> candidate index 1 (feature/y)
	h.askAnswers = {false}; // restart: later
	h.doInstallOut = "ok";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);

	CHECK_EQ(h.CountScript("do-install"), 1);
	std::vector<std::string> args = h.DoInstallArgs();
	CHECK_EQ(static_cast<std::size_t>(args.size()), static_cast<std::size_t>(3));
	if (args.size() == 3)
	{
		CHECK_EQ(args[1], "https://ex.com/y.zip"); // the SECOND candidate
		CHECK_EQ(args[2], "min-nano_structureDev");
	}
	// Like the stable channel, success is reported by the restart question.
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.asks.size()), static_cast<std::size_t>(1));
	if (h.asks.size() == 1)
	{
		CHECK_EQ(h.asks[0][0], "開発版ビルドをインストールしました。");
		CHECK_EQ(h.asks[0][1], "branch: feature/y\ncommit: bbb2222\n\n"
							   "反映するには Vectorworks の再起動が必要です。\n"
							   "今すぐ再起動しますか？（開いているファイルは保存を確認します）");
		CHECK_EQ(h.asks[0][2], "再起動");
		CHECK_EQ(h.asks[0][3], "後で");
	}
	CHECK_EQ(h.restartCount, 0);
}

TEST(dev_restart_button_restarts_vectorworks)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	h.pickAnswer = 1;	// the only candidate
	h.askAnswer = true; // presses 再起動
	h.doInstallOut = "ok";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);

	CHECK_EQ(h.CountScript("do-install"), 1);
	CHECK_EQ(h.restartCount, 1);
}

TEST(dev_out_of_range_selection_keeps_current)
{
	FakeHost h;
	h.qDevOut = "build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	h.pickAnswer = 5; // past the last candidate
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);
	CHECK_EQ(h.CountScript("do-install"), 0); // safeguard -> no install
}

TEST(dev_install_failure_is_reported)
{
	FakeHost h;
	h.qDevOut = "build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	h.pickAnswer = 1;	// the only candidate
	h.askAnswer = true; // would press 再起動 if it were ever offered...
	h.doInstallOut = "error=アーカイブの展開に失敗しました。\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);

	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "インストールに失敗しました。");
		CHECK_EQ(h.informs[0][1], "アーカイブの展開に失敗しました。");
	}
	// ...but the install failed, so it never is.
	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(h.restartCount, 0);
}

// ---------------------------------------------------------------------------
// 手で押したとき（UpdateCheckKind::Manual）は必ず結末を出す
//
// 起動時に自動で走っていた頃は「黙っている」が正しかった——起動の邪魔をしないため。
// いまはメニューコマンドから呼ばれるので、**押したのに何も起きない**のでは、最新
// だったのか、そもそも動いていないのかが区別できない（src/Updater.h）。
// ---------------------------------------------------------------------------

TEST(stable_manual_check_reports_being_up_to_date)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=abc1234\n"
				   "url=https://ex.com/x.zip\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Manual, kRunningShell);

	CHECK_EQ(h.askCount, 0);				  // 入れるものが無いので尋ねない
	CHECK_EQ(h.CountScript("do-install"), 0); // 何も入れない
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "みんなの構造設計支援は最新です。");
		CHECK_EQ(h.informs[0][1], "build: abc1234");
	}
}

TEST(stable_manual_check_reports_an_offline_error)
{
	FakeHost h;
	h.qStableOut = "error=stable リリースを取得できませんでした。\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Manual, kRunningShell);

	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "更新を確認できませんでした。");
		// スクリプトの言い分を先に、続けて「繋がっていないのでは」を添える。
		CHECK_EQ(h.informs[0][1],
				 "stable リリースを取得できませんでした。\n\n"
				 "ネットワークに繋がっていないか、リリースを取得できませんでした。\n"
				 "しばらく待ってからもう一度お試しください。");
	}
}

TEST(stable_manual_check_reports_when_the_script_cannot_start)
{
	FakeHost h;
	h.qStableStarts = false;
	RunStableUpdateCheckWith(h, UpdateCheckKind::Manual, kRunningShell);

	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "更新を確認できませんでした。");
		CHECK(h.informs[0][1].find("アップデータを起動できませんでした。") == 0u);
	}
}

TEST(stable_manual_check_reports_an_incomplete_release)
{
	// error= は無いが url が無い（配布 zip の付いていないリリース）。**最新だとは
	// 言えない**——確認できていないので、そう伝える。
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Manual, kRunningShell);

	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "更新を確認できませんでした。");
		CHECK(h.informs[0][1].find("リリースの情報が不完全です。") == 0u);
	}
}

TEST(stable_silent_check_says_nothing_about_an_incomplete_release)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\nlatest=def5678\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// 取り込みのついで（UpdateCheckKind::Silent）の開発版
//
// **ブランチ選択のダイアログを出さない。** 取り込みたいだけの人の前に「どのブランチを
// 使いますか」を挟むのは邪魔でしかないので、拾うのは**いま動いているのと同じブランチの
// 新しいビルド**だけにする（src/UpdaterFlow.cpp）。
// ---------------------------------------------------------------------------

TEST(dev_silent_check_offers_the_same_branchs_newer_build)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n"
				"build\tbbb2222\tDev: other (bbb2222)\thttps://ex.com/y.zip\tother\n";
	h.askAnswers = {true, false}; // インストール: はい、再起動: 後で
	h.doInstallOut = "ok";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Silent, "feature/x", "run1234", kRunningShell);

	CHECK_EQ(h.pickCount, 0); // 選択ダイアログは出さない
	CHECK_EQ(h.CountScript("do-install"), 1);
	std::vector<std::string> args = h.DoInstallArgs();
	if (args.size() == 3)
	{
		CHECK_EQ(args[1], "https://ex.com/x.zip"); // 同じブランチのほう
		CHECK_EQ(args[2], "min-nano_structureDev");
	}
	// 最初に出るのは「入れますか？」で、勝手には入れない。
	if (!h.asks.empty())
	{
		CHECK_EQ(h.asks[0][0], "同じブランチの新しい開発版ビルドがあります。"
							   "今すぐインストールしますか？");
		CHECK_EQ(h.asks[0][1], "branch: feature/x\nインストール済み: run1234\n"
							   "新しいビルド: aaa1111");
	}
	// 結末（再起動を尋ねる）には素のブランチ名が出る（表示名ではなく）。
	if (h.asks.size() == 2)
		CHECK_EQ(h.asks[1][1].find("branch: feature/x\ncommit: aaa1111"),
				 static_cast<std::size_t>(0));
}

TEST(dev_silent_check_says_nothing_when_no_build_matches_the_branch)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\tbbb2222\tDev: other (bbb2222)\thttps://ex.com/y.zip\tother\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Silent, "feature/x", "run1234", kRunningShell);

	CHECK_EQ(h.pickCount, 0);
	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(h.CountScript("do-install"), 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_silent_check_says_nothing_when_the_branch_cannot_be_told)
{
	// 4 列しか出さない**古い同梱スクリプト**でも、題が CI の形なら照合できる
	// （UpdaterParse.h の ParseDevBuilds）。ここはその形ですらない場合——照合の
	// しようが無いので**何もしない**。別のブランチのビルドを勝手に入れるよりよい。
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Silent, "feature/x", "run1234", kRunningShell);

	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(h.CountScript("do-install"), 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_silent_check_reads_the_branch_from_the_title_when_the_column_is_missing)
{
	// **古いスクリプトでも往復が回る。** 5 列目が無くても、CI の題
	// "Dev: <branch> (<sha>)" からブランチが読めるので同じブランチを拾える。
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\n";
	h.askAnswer = false; // 「後で」——ここで見たいのは「尋ねたかどうか」だけ
	RunDevUpdateCheckWith(h, UpdateCheckKind::Silent, "feature/x", "run1234", kRunningShell);

	CHECK_EQ(h.askCount, 1);
}

TEST(dev_silent_check_says_nothing_when_offline)
{
	FakeHost h;
	h.qDevOut = "error=リリース一覧を取得できませんでした。\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Silent, "feature/x", "run1234", kRunningShell);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_silent_check_declined_does_not_install)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n";
	h.askAnswer = false; // 「後で」
	RunDevUpdateCheckWith(h, UpdateCheckKind::Silent, "feature/x", "run1234", kRunningShell);

	CHECK_EQ(h.askCount, 1);
	CHECK_EQ(h.CountScript("do-install"), 0);
}

// ---------------------------------------------------------------------------
// 実機フィードバックの往復（UpdateCheckKind::Auto）
//
// **尋ねない・報せない・再起動しない。** 往復の最中に呼ばれるので、周ごとにダイアログを
// 挟むのはこの仕組みが無くそうとしている手間そのものになる（src/UpdaterHost.h の
// UpdateCheckKind::Auto）。口を開いて false（＝この実行では取り込みへ進むな）を返すのは、
// **入れたのに効かせられなかったとき**だけ。
// ---------------------------------------------------------------------------

TEST(dev_auto_check_installs_the_same_branchs_build_without_asking)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n"
				"build\tbbb2222\tDev: other (bbb2222)\thttps://ex.com/y.zip\tother\n";
	h.doInstallOut = std::string("installed-shell=") + kRunningShell + "\nok";
	const bool proceed =
		RunDevUpdateCheckWith(h, UpdateCheckKind::Auto, "feature/x", "run1234", kRunningShell);

	CHECK(proceed);			  // そのまま取り込みへ進む
	CHECK_EQ(h.askCount, 0);  // 尋ねない
	CHECK_EQ(h.pickCount, 0); // 選ばせない
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0)); // 報せない
	CHECK_EQ(h.restartCount, 0); // 再起動しない
	CHECK_EQ(h.CountScript("do-install"), 1);
	CHECK_EQ(h.dropCount, 1); // 本体は降ろす（＝次の周で読み直される）
	const std::vector<std::string> args = h.DoInstallArgs();
	if (args.size() == 3)
		CHECK_EQ(args[1], "https://ex.com/x.zip"); // 同じブランチのほう
}

TEST(dev_auto_check_is_silent_and_proceeds_when_no_new_build_matches)
{
	// **まだ新しいビルドが出ていないだけ。** 人は自分の判断で取り込みを実行している
	// のだから、黙って通す（往復を回すかどうかはその人が決める）。
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\tbbb2222\tDev: other (bbb2222)\thttps://ex.com/y.zip\tother\n";
	const bool proceed =
		RunDevUpdateCheckWith(h, UpdateCheckKind::Auto, "feature/x", "run1234", kRunningShell);

	CHECK(proceed);
	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(h.CountScript("do-install"), 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_auto_check_is_silent_and_proceeds_when_offline)
{
	// 確認できなかっただけで、取り込みを止める理由にはならない（更新は付随でしかない）。
	FakeHost h;
	h.qDevOut = "error=リリース一覧を取得できませんでした。\n";
	const bool proceed =
		RunDevUpdateCheckWith(h, UpdateCheckKind::Auto, "feature/x", "run1234", kRunningShell);

	CHECK(proceed);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_auto_check_is_silent_and_proceeds_when_the_script_cannot_start)
{
	FakeHost h;
	h.qDevStarts = false;
	const bool proceed =
		RunDevUpdateCheckWith(h, UpdateCheckKind::Auto, "feature/x", "run1234", kRunningShell);

	CHECK(proceed);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_auto_check_skips_the_import_when_the_shell_changed)
{
	// **殻まで変わったら勝手に再起動しない。** 図面を開いたまま往復を回している人を
	// 落とすわけにいかないので、伝えて取り込みを見送り、判断はその人に委ねる。
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n";
	h.doInstallOut = "installed-shell=DIFFERENT\nok";
	const bool proceed =
		RunDevUpdateCheckWith(h, UpdateCheckKind::Auto, "feature/x", "run1234", kRunningShell);

	CHECK(!proceed);
	CHECK_EQ(h.askCount, 0); // 「再起動しますか？」は出さない
	CHECK_EQ(h.restartCount, 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
		CHECK(h.informs[0][1].find("再起動") != std::string::npos);
}

TEST(dev_auto_check_skips_the_import_when_the_payload_cannot_be_dropped)
{
	// 降ろせなければ古い本体のまま。前の周と同じ結果をもう一度出しても意味が無い。
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n";
	h.doInstallOut = std::string("installed-shell=") + kRunningShell + "\nok";
	h.dropAnswer = false;
	const bool proceed =
		RunDevUpdateCheckWith(h, UpdateCheckKind::Auto, "feature/x", "run1234", kRunningShell);

	CHECK(!proceed);
	CHECK_EQ(h.dropCount, 1);
}

TEST(dev_auto_check_reports_and_skips_the_import_when_the_install_fails)
{
	// **入れられなかったときは黙らない。** 尋ねずに入れる約束で呼ばれているのだから、
	// 入らなかったことは伝える。
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n";
	h.doInstallOut = "error=ダウンロードに失敗しました。\n";
	const bool proceed =
		RunDevUpdateCheckWith(h, UpdateCheckKind::Auto, "feature/x", "run1234", kRunningShell);

	CHECK(!proceed);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
		CHECK_EQ(h.informs[0][0], "インストールに失敗しました。");
}

TEST(stable_auto_check_never_installs_without_asking)
{
	// **安定版に Auto は無い。** 往復するのは PR のビルドであって main の配布物では
	// ないので、万一 Auto で呼ばれても Silent と同じ扱い＝必ず尋ねる。
	FakeHost h;
	h.qStableOut = "installed=old\nlatest=new\nurl=https://ex.com/s.zip\n";
	h.askAnswer = false; // 「後で」
	const bool proceed = RunStableUpdateCheckWith(h, UpdateCheckKind::Auto, kRunningShell);

	CHECK(proceed);
	CHECK_EQ(h.askCount, 1);
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(dev_manual_picker_shows_the_branch_when_the_script_reports_it)
{
	// 5 列目があるときは、表示名（"Dev: feature/x (aaa1111)"）ではなくブランチ名を出す
	// ——コミットは隣に並ぶので、表示名では同じものが 2 度出る。
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n";
	h.pickAnswer = 0;
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);

	CHECK_EQ(h.pickCount, 1);
	if (h.lastPickItems.size() == 2)
		CHECK_EQ(h.lastPickItems[1], "feature/x  (aaa1111)");
}

// ---------------------------------------------------------------------------
// ホットリロード（殻が同じなら再起動を尋ねない）
//
// プラグインは殻と本体に割れていて、Vectorworks が起動時にしか読み込めないのは殻だけ
// （src/PayloadAbi.h）。**入れたビルドの殻が同じなら、本体を降ろすだけで次の操作から
// 新しいコードが動く**ので、再起動を尋ねてはならない——それがこの分岐の全部である。
// ---------------------------------------------------------------------------

TEST(stable_same_shell_reloads_without_asking_to_restart)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/min-nano_structure.zip\n";
	h.askAnswer = true; // says yes to the install question
	// 入れた殻はいま動いているものと同じ＝本体だけが新しい。
	h.doInstallOut = std::string("installed-shell=") + kRunningShell + "\nok\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	CHECK_EQ(h.CountScript("do-install"), 1);
	// 尋ねたのは「インストールしますか？」の 1 回だけ（再起動は尋ねない）。
	CHECK_EQ(h.askCount, 1);
	CHECK_EQ(h.restartCount, 0);
	// 代わりに本体を降ろして、そう伝える。
	CHECK_EQ(h.dropCount, 1);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "みんなの構造設計支援を更新しました。");
		CHECK_EQ(h.informs[0][1], "build: def5678\n\n"
								  "Vectorworks の再起動は要りません。\n"
								  "次の取り込みから新しいビルドが動きます。");
	}
}

TEST(stable_same_shell_but_payload_still_in_use_defers_to_next_start)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/min-nano_structure.zip\n";
	h.askAnswer = true;
	h.doInstallOut = std::string("installed-shell=") + kRunningShell + "\nok\n";
	h.dropAnswer = false; // 本体のコードがまだ走っている
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	// それでも再起動は尋ねない（殻は同じなので、次の起動で確実に反映される）。
	CHECK_EQ(h.askCount, 1);
	CHECK_EQ(h.restartCount, 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
		CHECK_EQ(h.informs[0][1],
				 "build: def5678\n\n"
				 "反映は次に Vectorworks を起動したときです。\n"
				 "（いま動いている処理があるため、その場では入れ替えられませんでした）");
}

TEST(stable_different_shell_still_offers_restart)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/min-nano_structure.zip\n";
	h.askAnswers = {true, false}; // install: yes, restart: 後で
	h.doInstallOut = "installed-shell=cccc2222dddd\nok\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	// 殻まで変わったので、従来どおり再起動を尋ねる。本体は降ろさない
	// （どうせ次の起動で殻ごと入れ替わる）。
	CHECK_EQ(h.askCount, 2);
	CHECK_EQ(h.dropCount, 0);
	if (h.asks.size() == 2)
		CHECK_EQ(h.asks[1][2], "再起動");
}

TEST(dev_same_shell_reloads_without_asking_to_restart)
{
	FakeHost h;
	h.qDevOut = "build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	h.pickAnswer = 1; // the only candidate
	h.doInstallOut = std::string("installed-shell=") + kRunningShell + "\nok\n";
	RunDevUpdateCheckWith(h, UpdateCheckKind::Manual, "main", "run1234", kRunningShell);

	CHECK_EQ(h.CountScript("do-install"), 1);
	CHECK_EQ(h.askCount, 0); // 再起動を尋ねない
	CHECK_EQ(h.restartCount, 0);
	CHECK_EQ(h.dropCount, 1);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "開発版ビルドをインストールしました。");
		CHECK_EQ(h.informs[0][1], "branch: feature/x\ncommit: aaa1111\n\n"
								  "Vectorworks の再起動は要りません。\n"
								  "次の取り込みから新しいビルドが動きます。");
	}
}

TEST(install_without_a_shell_line_falls_back_to_restart)
{
	// 古い同梱スクリプト（installed-shell を出さない版）。**判断できないので安全側**
	// ＝再起動を尋ねる（src/UpdaterParse.h の NeedsRestartAfterInstall）。
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/min-nano_structure.zip\n";
	h.askAnswers = {true, false};
	h.doInstallOut = "ok\n";
	RunStableUpdateCheckWith(h, UpdateCheckKind::Silent, kRunningShell);

	CHECK_EQ(h.askCount, 2);
	CHECK_EQ(h.dropCount, 0);
}

// ---------------------------------------------------------------------------
// モードレスの往復（M24）が周期的に呼ぶ確認（PollDevBuildWith）。
//
// **ダイアログを 1 枚も出さず、結末を値で返す。** パレットがその文言を出すので、ここで
// Inform / Ask を呼ぶとモーダルのダイアログが図面の前に立ちはだかる。
// ---------------------------------------------------------------------------

TEST(poll_dev_build_installs_the_same_branchs_new_build_silently)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n"
				"build\tbbb2222\tDev: other (bbb2222)\thttps://ex.com/y.zip\tother\n";
	h.doInstallOut = std::string("installed-shell=") + kRunningShell + "\nok";
	const DevBuildPollResult r = PollDevBuildWith(h, "feature/x", "run1234", kRunningShell);

	CHECK(r.outcome == DevBuildPoll::Installed);
	CHECK_EQ(r.commit, "aaa1111");
	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(h.pickCount, 0);
	CHECK_EQ(h.restartCount, 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
	CHECK_EQ(h.CountScript("do-install"), 1);
	CHECK_EQ(h.dropCount, 1);
	const std::vector<std::string> args = h.DoInstallArgs();
	if (args.size() == 3)
		CHECK_EQ(args[1], "https://ex.com/x.zip");
}

TEST(poll_dev_build_waits_when_nothing_new_matches)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\tbbb2222\tDev: other (bbb2222)\thttps://ex.com/y.zip\tother\n";
	const DevBuildPollResult r = PollDevBuildWith(h, "feature/x", "run1234", kRunningShell);
	CHECK(r.outcome == DevBuildPoll::NoNewBuild);
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(poll_dev_build_uses_the_installed_line_not_the_shells_sha)
{
	// **本体だけを入れ替えたあと。** 殻の sha（run1234）は古いままだが、ディスク上は
	// もう aaa1111 になっている。殻の sha を基準にすると同じビルドを毎周入れ直す。
	FakeHost h;
	h.qDevOut = "installed=aaa1111\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n";
	const DevBuildPollResult r = PollDevBuildWith(h, "feature/x", "run1234", kRunningShell);
	CHECK(r.outcome == DevBuildPoll::NoNewBuild);
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(poll_dev_build_reports_a_failed_check_without_a_dialog)
{
	FakeHost h;
	h.qDevOut = "error=リリース一覧を取得できませんでした。\n";
	const DevBuildPollResult r = PollDevBuildWith(h, "feature/x", "run1234", kRunningShell);
	CHECK(r.outcome == DevBuildPoll::CheckFailed);
	CHECK_EQ(r.message, "リリース一覧を取得できませんでした。");
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));

	FakeHost h2;
	h2.qDevStarts = false;
	CHECK(PollDevBuildWith(h2, "feature/x", "run1234", kRunningShell).outcome ==
		  DevBuildPoll::CheckFailed);
}

TEST(poll_dev_build_reports_install_failure_and_shell_change)
{
	FakeHost h;
	h.qDevOut = "installed=run1234\n"
				"build\taaa1111\tDev: feature/x (aaa1111)\thttps://ex.com/x.zip\tfeature/x\n";
	h.doInstallOut = "error=zip を展開できませんでした。\n";
	const DevBuildPollResult failed = PollDevBuildWith(h, "feature/x", "run1234", kRunningShell);
	CHECK(failed.outcome == DevBuildPoll::Failed);
	CHECK_EQ(failed.message, "zip を展開できませんでした。");
	CHECK_EQ(h.dropCount, 0);

	// 殻まで変わった。入ってはいるが、この実行では効かせられない＝降ろさない。
	FakeHost h2;
	h2.qDevOut = h.qDevOut;
	h2.doInstallOut = "installed-shell=other-shell\nok";
	const DevBuildPollResult restart = PollDevBuildWith(h2, "feature/x", "run1234", kRunningShell);
	CHECK(restart.outcome == DevBuildPoll::NeedsRestart);
	CHECK_EQ(restart.commit, "aaa1111");
	CHECK_EQ(h2.dropCount, 0);
	CHECK_EQ(h2.restartCount, 0);

	// 降ろせなかった（本体のコードがまだ走っている）。
	FakeHost h3;
	h3.qDevOut = h.qDevOut;
	h3.doInstallOut = std::string("installed-shell=") + kRunningShell + "\nok";
	h3.dropAnswer = false;
	CHECK(PollDevBuildWith(h3, "feature/x", "run1234", kRunningShell).outcome ==
		  DevBuildPoll::Failed);
}

// ---------------------------------------------------------------------------

TEST_MAIN();
