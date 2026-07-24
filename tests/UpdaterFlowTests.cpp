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

using namespace SamplePlugin;

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
		bool askAnswer = true; // what Ask returns
		int pickAnswer = 0;	   // what PickBuild returns

		// --- Recorded interactions ------------------------------------------
		std::vector<std::vector<std::string>> scriptCalls;
		std::vector<std::vector<std::string>> informs; // {text, advice}
		int askCount = 0;
		int pickCount = 0;
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

		bool Ask(const std::string&, const std::string&, const std::string&,
				 const std::string&) override
		{
			++askCount;
			return askAnswer;
		}

		int PickBuild(const std::vector<std::string>& items, int) override
		{
			++pickCount;
			lastPickItems = items;
			return pickAnswer;
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
				   "url=https://ex.com/SamplePlugin.zip\n";
	h.askAnswer = true;
	h.doInstallOut = "ok";
	RunStableStartupCheckWith(h);

	CHECK_EQ(h.askCount, 1);
	CHECK_EQ(h.CountScript("do-install"), 1);
	// Installed the right asset under the stable name.
	std::vector<std::string> args = h.DoInstallArgs();
	CHECK_EQ(static_cast<std::size_t>(args.size()), static_cast<std::size_t>(3));
	if (args.size() == 3)
	{
		CHECK_EQ(args[1], "https://ex.com/SamplePlugin.zip");
		CHECK_EQ(args[2], "SamplePlugin");
	}
	// Reported success.
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
		CHECK_EQ(h.informs[0][0], "SamplePlugin を更新しました。");
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
	h.pickAnswer = 2; // entry 2 -> candidate index 1 (feature/y)
	h.doInstallOut = "ok";
	RunDevStartupCheckWith(h, "main", "run1234");

	CHECK_EQ(h.CountScript("do-install"), 1);
	std::vector<std::string> args = h.DoInstallArgs();
	CHECK_EQ(static_cast<std::size_t>(args.size()), static_cast<std::size_t>(3));
	if (args.size() == 3)
	{
		CHECK_EQ(args[1], "https://ex.com/y.zip"); // the SECOND candidate
		CHECK_EQ(args[2], "SamplePluginDev");
	}
	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
		CHECK_EQ(h.informs[0][0], "開発版ビルドをインストールしました。");
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
	h.pickAnswer = 1; // the only candidate
	h.doInstallOut = "error=アーカイブの展開に失敗しました。\n";
	RunDevStartupCheckWith(h, "main", "run1234");

	CHECK_EQ(static_cast<std::size_t>(h.informs.size()), static_cast<std::size_t>(1));
	if (!h.informs.empty())
	{
		CHECK_EQ(h.informs[0][0], "インストールに失敗しました。");
		CHECK_EQ(h.informs[0][1], "アーカイブの展開に失敗しました。");
	}
}

// ---------------------------------------------------------------------------

TEST_MAIN();
