//
//	UpdaterFlow.cpp
//
//	The two startup update flows, written against IUpdaterHost (UpdaterHost.h)
//	instead of the SDK. This file includes NO Vectorworks header, so it compiles
//	and runs on a plain toolchain and is linked into both the plug-in and the
//	unit tests. All decisions delegate to the pure helpers in UpdaterParse.h; all
//	side effects go through the injected host. Behaviour is identical to the
//	original inline code in Updater.cpp — only the SDK calls became host calls.
//

#include "UpdaterHost.h"
#include "UpdaterParse.h"

#include <string>
#include <vector>

using namespace SamplePlugin::UpdaterParse;

namespace SamplePlugin
{
	namespace
	{
		// Run the bundled installer for one plug-in via the host. Returns true on
		// success; fills errorOut with the script's message (or a fallback) on
		// failure. The "could not start" wording is kept here, next to the flow,
		// rather than in the host.
		bool Install(IUpdaterHost& host, const std::string& url, const std::string& name,
					 std::string& errorOut)
		{
			std::string out;
			if (!host.RunScript({ "do-install", url, name }, out))
			{
				errorOut = "アップデータを起動できませんでした。";
				return false;
			}
			if (InstallReportedOk(out))
				return true;

			errorOut = InstallErrorText(out, "インストールに失敗しました。");
			return false;
		}
	}

	void RunStableStartupCheckWith(IUpdaterHost& host)
	{
		std::string out;
		if (!host.RunScript({ "q-stable" }, out))
			return;								// script missing -> stay silent

		// Offline / incomplete / already-current all come back as
		// offerUpdate == false (see EvaluateStable).
		StableStatus st = EvaluateStable(out);
		if (!st.offerUpdate)
			return;

		std::string shownInstalled = st.installed.empty() ? "none" : st.installed;
		if (!host.Ask("新しい安定版ビルドがあります。今すぐインストールしますか？",
					  "インストール済み: " + shownInstalled + "\n最新: " + st.latest,
					  "インストール", "後で"))
			return;

		std::string err;
		if (Install(host, st.url, "SamplePlugin", err))
			host.Inform("SamplePlugin を更新しました。",
						"反映するには Vectorworks を再起動してください。");
		else
			host.Inform("更新に失敗しました。", err);
	}

	void RunDevStartupCheckWith(IUpdaterHost& host,
								const std::string& runningBranch,
								const std::string& runningCommit)
	{
		std::string out;
		if (!host.RunScript({ "q-dev" }, out) || !ValueOf(out, "error").empty())
			// Offline / transient: don't block start-up — carry on with the
			// currently loaded build.
			return;

		// Candidates to switch TO: every prerelease except the running build.
		std::vector<DevBuild> others = DevSwitchCandidates(out, runningCommit);

		// One drop-down listing everything: entry 0 is the installed build,
		// entries 1.. are the other branches' prereleases.
		std::vector<std::string> items;
		items.push_back("現在: " + runningBranch + " (" + runningCommit + ") ― インストール済み");
		for (const DevBuild& b : others)
			items.push_back(b.name + "  (" + b.commit + ")");

		int sel = host.PickBuild(items, /*initialSel*/ 0);
		if (sel < 0)
			return;								// cancelled -> keep the loaded build

		// Map the selection back to a candidate (entry 0 or an out-of-range value
		// both mean "keep the installed build"). See ResolveDevSelection.
		int idx = ResolveDevSelection(static_cast<short>(sel), others.size());
		if (idx < 0)
			return;
		const DevBuild& pick = others[static_cast<std::size_t>(idx)];

		// A different build was chosen: install it (restart to load).
		std::string err;
		if (Install(host, pick.url, "SamplePluginDev", err))
			host.Inform("開発版ビルドをインストールしました。",
						"反映するには Vectorworks を再起動してください。\n"
						"branch: " + pick.name + "\ncommit: " + pick.commit);
		else
			host.Inform("インストールに失敗しました。", err);
	}
}
