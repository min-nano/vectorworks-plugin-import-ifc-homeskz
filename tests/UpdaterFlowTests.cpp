//
//	UpdaterFlowTests.cpp
//
//	Tests for the update FLOWS (src/UpdaterFlow.cpp): RunStableStartupCheckWith
//	and RunDevStartupCheckWith. They are driven through a FAKE IUpdaterHost that
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

		// --- Recorded interactions ------------------------------------------
		std::vector<std::vector<std::string>> scriptCalls;
		std::vector<std::vector<std::string>> informs; // {text, advice}
		// {text, advice, okText, cancelText} of every Ask, in order.
		std::vector<std::vector<std::string>> asks;
		int askCount = 0;
		int pickCount = 0;
		int restartCount = 0;
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
} // namespace

// ---------------------------------------------------------------------------
// Stable flow
// ---------------------------------------------------------------------------

TEST(stable_stays_silent_when_script_cannot_start)
{
	FakeHost h;
	h.qStableStarts = false;
	RunStableStartupCheckWith(h);
	CHECK_EQ(h.askCount, 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(stable_stays_silent_when_already_current)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=abc1234\n"
				   "url=https://ex.com/x.zip\n";
	RunStableStartupCheckWith(h);
	CHECK_EQ(h.askCount, 0); // no dialog when up to date
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(stable_stays_silent_on_error_line)
{
	FakeHost h;
	h.qStableOut = "error=offline\n";
	RunStableStartupCheckWith(h);
	CHECK_EQ(h.askCount, 0);
}

TEST(stable_declined_does_not_install)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/x.zip\n";
	h.askAnswer = false; // user chose "後で"
	RunStableStartupCheckWith(h);
	CHECK_EQ(h.askCount, 1);				  // was asked
	CHECK_EQ(h.CountScript("do-install"), 0); // but nothing installed
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(stable_accepted_and_install_succeeds)
{
	FakeHost h;
	h.qStableOut = "installed=abc1234\n"
				   "latest=def5678\n"
				   "url=https://ex.com/HomeskzIfcImport.zip\n";
	h.askAnswers = {true, false}; // install: yes, restart: later
	h.doInstallOut = "ok";
	RunStableStartupCheckWith(h);

	CHECK_EQ(h.CountScript("do-install"), 1);
	// Installed the right asset under the stable name.
	std::vector<std::string> args = h.DoInstallArgs();
	CHECK_EQ(static_cast<std::size_t>(args.size()), static_cast<std::size_t>(3));
	if (args.size() == 3)
	{
		CHECK_EQ(args[1], "https://ex.com/HomeskzIfcImport.zip");
		CHECK_EQ(args[2], "HomeskzIfcImport");
	}
	// Success is reported by the restart QUESTION (a plain notice would leave the
	// user to work out that a restart is needed), not by an Inform.
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.asks.size()), static_cast<std::size_t>(2));
	if (h.asks.size() == 2)
	{
		CHECK_EQ(h.asks[1][0], "HomeskzIfcImport を更新しました。");
		CHECK_EQ(h.asks[1][1],
				 "build: def5678\n\n"
				 "反映するには Vectorworks の再起動が必要です。\n"
				 "今すぐ再起動しますか？（起動の完了後に終了し、自動で起動し直します。\n"
				 "開いているファイルは保存を確認します）");
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
				   "url=https://ex.com/HomeskzIfcImport.zip\n";
	h.askAnswer = true; // says yes to both questions: install, then restart
	h.doInstallOut = "ok";
	RunStableStartupCheckWith(h);

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
				   "url=https://ex.com/HomeskzIfcImport.zip\n";
	h.askAnswer = true;
	h.doInstallOut = "ok";
	h.restartAnswer = false; // e.g. the relaunch helper would not start
	RunStableStartupCheckWith(h);

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
	RunStableStartupCheckWith(h);

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
	RunStableStartupCheckWith(h);

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

TEST(dev_stays_silent_when_script_cannot_start)
{
	FakeHost h;
	h.qDevStarts = false;
	RunDevStartupCheckWith(h, "main", "run1234");
	CHECK_EQ(h.pickCount, 0);
	CHECK_EQ(h.CountScript("do-install"), 0);
}

TEST(dev_stays_silent_on_error_line)
{
	FakeHost h;
	h.qDevOut = "error=リリース一覧を取得できませんでした。\n";
	RunDevStartupCheckWith(h, "main", "run1234");
	CHECK_EQ(h.pickCount, 0);
}

TEST(dev_skips_picker_when_no_prereleases_exist)
{
	FakeHost h;
	// The script returned no build rows at all.
	h.qDevOut = "installed=run1234\n";
	RunDevStartupCheckWith(h, "main", "run1234");
	CHECK_EQ(h.pickCount, 0); // nothing to choose -> no dialog
	CHECK_EQ(h.CountScript("do-install"), 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
}

TEST(dev_skips_picker_when_only_prerelease_is_the_running_build)
{
	FakeHost h;
	// The only prerelease is the build already loaded (same commit).
	h.qDevOut = "installed=run1234\n"
				"build\trun1234\tmain\thttps://ex.com/main.zip\n";
	RunDevStartupCheckWith(h, "main", "run1234");
	CHECK_EQ(h.pickCount, 0); // no alternative build -> no dialog
	CHECK_EQ(h.CountScript("do-install"), 0);
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
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
	RunDevStartupCheckWith(h, "main", "run1234");

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
	RunDevStartupCheckWith(h, "main", "run1234");
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
	RunDevStartupCheckWith(h, "main", "run1234");

	CHECK_EQ(h.CountScript("do-install"), 1);
	std::vector<std::string> args = h.DoInstallArgs();
	CHECK_EQ(static_cast<std::size_t>(args.size()), static_cast<std::size_t>(3));
	if (args.size() == 3)
	{
		CHECK_EQ(args[1], "https://ex.com/y.zip"); // the SECOND candidate
		CHECK_EQ(args[2], "HomeskzIfcImportDev");
	}
	// Like the stable channel, success is reported by the restart question.
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(0));
	CHECK_EQ(static_cast<std::size_t>(h.asks.size()), static_cast<std::size_t>(1));
	if (h.asks.size() == 1)
	{
		CHECK_EQ(h.asks[0][0], "開発版ビルドをインストールしました。");
		CHECK_EQ(h.asks[0][1],
				 "branch: feature/y\ncommit: bbb2222\n\n"
				 "反映するには Vectorworks の再起動が必要です。\n"
				 "今すぐ再起動しますか？（起動の完了後に終了し、自動で起動し直します。\n"
				 "開いているファイルは保存を確認します）");
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
	RunDevStartupCheckWith(h, "main", "run1234");

	CHECK_EQ(h.CountScript("do-install"), 1);
	CHECK_EQ(h.restartCount, 1);
}

TEST(dev_out_of_range_selection_keeps_current)
{
	FakeHost h;
	h.qDevOut = "build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	h.pickAnswer = 5; // past the last candidate
	RunDevStartupCheckWith(h, "main", "run1234");
	CHECK_EQ(h.CountScript("do-install"), 0); // safeguard -> no install
}

TEST(dev_install_failure_is_reported)
{
	FakeHost h;
	h.qDevOut = "build\taaa1111\tfeature/x\thttps://ex.com/x.zip\n";
	h.pickAnswer = 1;	// the only candidate
	h.askAnswer = true; // would press 再起動 if it were ever offered...
	h.doInstallOut = "error=アーカイブの展開に失敗しました。\n";
	RunDevStartupCheckWith(h, "main", "run1234");

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

TEST_MAIN();
