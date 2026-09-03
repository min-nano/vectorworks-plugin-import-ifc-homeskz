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

using namespace HomeskzIfcImport::UpdaterParse;

namespace HomeskzIfcImport
{
	namespace
	{
		// Run the bundled installer for one plug-in via the host. Returns true on
		// success; fills errorOut with the script's message (or a fallback) on
		// failure. The "could not start" wording is kept here, next to the flow,
		// rather than in the host.
		bool Install(IUpdaterHost& host, const std::string& url, const std::string& name,
					 std::string& installedShellIdOut, std::string& errorOut)
		{
			std::string out;
			installedShellIdOut.clear();
			if (!host.RunScript({"do-install", url, name}, out))
			{
				errorOut = "アップデータを起動できませんでした。";
				return false;
			}
			if (InstallReportedOk(out))
			{
				// 入れたビルドの殻の ID。**「再起動が要るか」はこれで決まる**
				// （UpdaterParse.h）。古いスクリプトはこの行を出さないので空になり、
				// そのときは安全側＝再起動を尋ねる側へ倒れる。
				installedShellIdOut = InstalledShellId(out);
				return true;
			}

			errorOut = InstallErrorText(out, "インストールに失敗しました。");
			return false;
		}

		// **入れ替えが済んだあとの結末。** プラグインは 2 つに割れていて（src/PayloadAbi.h）、
		// Vectorworks が起動時にしか読み込めないのは**殻**だけ。中身（解析・描画・PIO の
		// 作図）は殻が自分で読み込む**本体**にあるので、
		//
		//   * 本体だけが新しくなった（殻の ID が同じ）… 載っている本体を降ろすだけで、
		//     次の取り込み・次の PIO リセットから新しいコードが動く。**再起動を尋ねない。**
		//   * 殻まで変わった …………………………………… 読み込めるのは次の起動だけなので、
		//     従来どおり再起動を尋ねる（OfferRestart）。
		//
		// 判断できないとき（スクリプトが ID を出さない古い版など）は「要る」へ倒れる。
		void OfferRestart(IUpdaterHost& host, const std::string& text, const std::string& detail);

		void FinishInstall(IUpdaterHost& host, const std::string& text, const std::string& detail,
						   const std::string& runningShellId, const std::string& installedShellId)
		{
			if (!NeedsRestartAfterInstall(runningShellId, installedShellId))
			{
				// **ここでホットリロードが効く。** 降ろしておけば、次に本体を使うときに
				// 新しいファイルが読み直される（src/PayloadSession.h）。降ろせなかった
				// ——本体のコードがまだ走っている——ときだけ、次回の起動へ回す。
				std::string advice = detail;
				if (!advice.empty())
					advice += "\n\n";
				if (host.DropLoadedPayload())
					advice += "Vectorworks の再起動は要りません。\n"
							  "次の取り込みから新しいビルドが動きます。";
				else
					advice +=
						"反映は次に Vectorworks を起動したときです。\n"
						"（いま動いている処理があるため、その場では入れ替えられませんでした）";
				host.Inform(text, advice);
				return;
			}

			OfferRestart(host, text, detail);
		}

		// 殻まで変わったときの結末。コンパイル済みの殻は起動時にしか読み込まれないので、
		// 新しいビルドは Vectorworks を再起動するまで動かない——だからこれは通知ではなく
		// **再起動ボタンを持つ質問**にしてある。「後で」を選んだら何もしない: いま閉じた
		// ダイアログが既に再起動の必要を告げているので、追い討ちの通知は小言にしかならない。
		// チャンネルごとの詳細（build / branch+commit）は `detail` で受け取り、共通の
		// 再起動の文言の上に出す。
		void OfferRestart(IUpdaterHost& host, const std::string& text, const std::string& detail)
		{
			std::string advice = detail;
			if (!advice.empty())
				advice += "\n\n";
			// The restart is requested from outside and arrives once Vectorworks
			// has finished starting up (see IUpdaterHost::Restart), so say that —
			// otherwise pressing 再起動 looks like it did nothing for a moment.
			advice += "反映するには Vectorworks の再起動が必要です。\n"
					  "今すぐ再起動しますか？（起動の完了後に終了し、自動で起動し直します。\n"
					  "開いているファイルは保存を確認します）";

			if (!host.Ask(text, advice, "再起動", "後で"))
				return;

			// The restart could not even be set up (the application to relaunch
			// could not be found, or the helper that does it would not start).
			// Nothing was lost — the new build is installed and will load at the
			// next start-up — but say so, otherwise pressing 再起動 looks like it
			// did nothing at all.
			if (!host.Restart())
				host.Inform("再起動できませんでした。",
							"お手数ですが、手動で Vectorworks を再起動してください。\n"
							"（更新自体は完了しているので、次回の起動で反映されます）");
		}
	} // namespace

	void RunStableStartupCheckWith(IUpdaterHost& host, const std::string& runningShellId)
	{
		std::string out;
		if (!host.RunScript({"q-stable"}, out))
			return; // script missing -> stay silent

		// Offline / incomplete / already-current all come back as
		// offerUpdate == false (see EvaluateStable).
		StableStatus const st = EvaluateStable(out);
		if (!st.offerUpdate)
			return;

		std::string const shownInstalled = st.installed.empty() ? "none" : st.installed;
		if (!host.Ask("新しい安定版ビルドがあります。今すぐインストールしますか？",
					  "インストール済み: " + shownInstalled + "\n最新: " + st.latest,
					  "インストール", "後で"))
			return;

		std::string err;
		std::string installedShellId;
		if (Install(host, st.url, "HomeskzIfcImport", installedShellId, err))
			FinishInstall(host, "HomeskzIfcImport を更新しました。", "build: " + st.latest,
						  runningShellId, installedShellId);
		else
			host.Inform("更新に失敗しました。", err);
	}

	void RunDevStartupCheckWith(IUpdaterHost& host, const std::string& runningBranch,
								const std::string& runningCommit, const std::string& runningShellId)
	{
		std::string out;
		if (!host.RunScript({"q-dev"}, out) || !ValueOf(out, "error").empty())
			// Offline / transient: don't block start-up — carry on with the
			// currently loaded build.
			return;

		// Candidates to switch TO: every prerelease except the running build.
		std::vector<DevBuild> others = DevSwitchCandidates(out, runningCommit);

		// Nothing to choose between: no prereleases exist, or the only ones are
		// the running build itself. The picker would show a single "keep current"
		// row and nothing else, so don't bother the user — carry on with the
		// loaded build.
		if (others.empty())
			return;

		// One drop-down listing everything: entry 0 is the installed build,
		// entries 1.. are the other branches' prereleases.
		std::vector<std::string> items;
		items.push_back("現在: " + runningBranch + " (" + runningCommit + ") ― インストール済み");
		for (const DevBuild& b : others)
			items.push_back(b.name + "  (" + b.commit + ")");

		int const sel = host.PickBuild(items, /*initialSel*/ 0);
		if (sel < 0)
			return; // cancelled -> keep the loaded build

		// Map the selection back to a candidate (entry 0 or an out-of-range value
		// both mean "keep the installed build"). See ResolveDevSelection.
		int const idx = ResolveDevSelection(static_cast<short>(sel), others.size());
		if (idx < 0)
			return;
		const DevBuild& pick = others[static_cast<std::size_t>(idx)];

		// A different build was chosen: install it, then offer the restart that
		// actually loads it.
		std::string err;
		std::string installedShellId;
		if (Install(host, pick.url, "HomeskzIfcImportDev", installedShellId, err))
			FinishInstall(host, "開発版ビルドをインストールしました。",
						  "branch: " + pick.name + "\ncommit: " + pick.commit, runningShellId,
						  installedShellId);
		else
			host.Inform("インストールに失敗しました。", err);
	}
} // namespace HomeskzIfcImport
