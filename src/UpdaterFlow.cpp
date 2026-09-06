//
//	UpdaterFlow.cpp
//
//	The two update flows, written against IUpdaterHost (UpdaterHost.h) instead of
//	the SDK. This file includes NO Vectorworks header, so it compiles and runs on
//	a plain toolchain and is linked into both the plug-in and the unit tests. All
//	decisions delegate to the pure helpers in UpdaterParse.h; all side effects go
//	through the injected host.
//
//	**いつ走るか。** 以前は Vectorworks の起動時（プラグインの読み込み中）に 1 度きり
//	だったが、いまは
//	  * メニューコマンド「アップデータを確認」……… UpdateCheckKind::Manual
//	  * 取り込みコマンドのついで ………………………… UpdateCheckKind::Silent
//	の 2 つの入口から呼ばれる。分かれるのは**「新しいビルドが無かった」ときに口を開くか
//	どうか**だけで（UpdaterHost.h の UpdateCheckKind）、更新があるときの流れ——尋ねて、
//	入れて、要るなら再起動——は同じである。
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
		// 配布物の名前（＝インストール先のフォルダ名・アセット名）と、人に見せる名前。
		// **この 2 つは別物**——前者はファイル名なので ASCII、後者はプラグインの名前。
		constexpr const char* kStablePluginName = "min-nano_structure";
		constexpr const char* kDevPluginName = "min-nano_structureDev";
		constexpr const char* kStableDisplayName = "みんなの構造設計支援";

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

		// ビルドを人に見せるときの名前。**ブランチ名があればそれ**——リリースの表示名
		// （name）は "Dev: <branch> (<sha>)" なので、コミットを並べると同じものが 2 度
		// 出る。branch はこの列を出さない古い同梱スクリプトでは空になるので、そのときは
		// 表示名で代用する（UpdaterParse.h の DevBuild）。
		std::string DevBuildLabel(const DevBuild& b)
		{
			return b.branch.empty() ? b.name : b.branch;
		}

		// **確認そのものができなかった。** オフライン・GitHub の一時的な不調・同梱
		// スクリプトを起動できない、のいずれか。
		//
		// Manual（メニューコマンド）のときは**必ず伝える**——押したのに何も起きないと、
		// 「最新だった」のか「そもそも動いていない」のかが利用者には区別できない。
		// Silent（取り込みのついで）では黙って進む: 取り込みたいだけの人に、繋がらない
		// ことを毎回知らせても仕方がない。
		void ReportCheckFailed(IUpdaterHost& host, UpdateCheckKind kind, const std::string& reason)
		{
			if (kind != UpdateCheckKind::Manual)
				return;

			std::string advice = reason;
			if (!advice.empty())
				advice += "\n\n";
			advice += "ネットワークに繋がっていないか、リリースを取得できませんでした。\n"
					  "しばらく待ってからもう一度お試しください。";
			host.Inform("更新を確認できませんでした。", advice);
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
			// 再起動は Vectorworks 自身に頼む（SDK の CloseAllFilesAndQuitVectorworks。
			// src/Updater.cpp）。開いている文書は通常どおり保存を確認してから閉じられ、
			// そこで取り消せば Vectorworks は落ちない——だからそう書く。
			advice += "反映するには Vectorworks の再起動が必要です。\n"
					  "今すぐ再起動しますか？（開いているファイルは保存を確認します）";

			if (!host.Ask(text, advice, "再起動", "後で"))
				return;

			// The restart could not even be requested. Nothing was lost — the new
			// build is installed and will load at the next start-up — but say so,
			// otherwise pressing 再起動 looks like it did nothing at all.
			if (!host.Restart())
				host.Inform("再起動できませんでした。",
							"お手数ですが、手動で Vectorworks を再起動してください。\n"
							"（更新自体は完了しているので、次回の起動で反映されます）");
		}
	} // namespace

	void RunStableUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
								  const std::string& runningShellId)
	{
		std::string out;
		if (!host.RunScript({"q-stable"}, out))
		{
			// 同梱スクリプトが見つからない／起動できない。
			ReportCheckFailed(host, kind, "アップデータを起動できませんでした。");
			return;
		}

		// オフライン・GitHub の不調はスクリプトが error= で返す。
		const std::string scriptError = ValueOf(out, "error");
		if (!scriptError.empty())
		{
			ReportCheckFailed(host, kind, scriptError);
			return;
		}

		StableStatus const st = EvaluateStable(out);
		if (!st.offerUpdate)
		{
			// 出力が足りない（リリースに配布 zip が無い等）のと、既に最新なのとは
			// 意味が違う——前者は**確認できていない**ので、そう伝える。
			if (st.latest.empty() || st.url.empty())
				ReportCheckFailed(host, kind, "リリースの情報が不完全です。");
			else if (kind == UpdateCheckKind::Manual)
				host.Inform(std::string(kStableDisplayName) + "は最新です。",
							"build: " + st.latest);
			return;
		}

		std::string const shownInstalled = st.installed.empty() ? "none" : st.installed;
		if (!host.Ask("新しい安定版ビルドがあります。今すぐインストールしますか？",
					  "インストール済み: " + shownInstalled + "\n最新: " + st.latest,
					  "インストール", "後で"))
			return;

		std::string err;
		std::string installedShellId;
		if (Install(host, st.url, kStablePluginName, installedShellId, err))
			FinishInstall(host, std::string(kStableDisplayName) + "を更新しました。",
						  "build: " + st.latest, runningShellId, installedShellId);
		else
			host.Inform("更新に失敗しました。", err);
	}

	void RunDevUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
							   const std::string& runningBranch, const std::string& runningCommit,
							   const std::string& runningShellId)
	{
		std::string out;
		if (!host.RunScript({"q-dev"}, out))
		{
			ReportCheckFailed(host, kind, "アップデータを起動できませんでした。");
			return;
		}

		const std::string scriptError = ValueOf(out, "error");
		if (!scriptError.empty())
		{
			ReportCheckFailed(host, kind, scriptError);
			return;
		}

		// Candidates to switch TO: every prerelease except the running build.
		std::vector<DevBuild> const others = DevSwitchCandidates(out, runningCommit);

		// どのビルドを入れるか。Manual は選ばせ、Silent は同じブランチのものだけを拾う。
		DevBuild pick;
		if (kind == UpdateCheckKind::Silent)
		{
			// **取り込みのついでにブランチ選択を出さない。** ここで拾うのは「いま動いて
			// いるのと同じブランチの、別のコミット」だけ——それだけが「自分のビルドが
			// 新しくなった」に当たる。ブランチが分からない（列を出さない古い同梱
			// スクリプト）ときは何も拾わない＝黙って取り込みへ進む。
			bool found = false;
			for (const DevBuild& b : others)
			{
				if (!b.branch.empty() && b.branch == runningBranch)
				{
					pick = b;
					found = true;
					break;
				}
			}
			if (!found)
				return;

			if (!host.Ask("同じブランチの新しい開発版ビルドがあります。"
						  "今すぐインストールしますか？",
						  "branch: " + runningBranch + "\nインストール済み: " + runningCommit +
							  "\n新しいビルド: " + pick.commit,
						  "インストール", "後で"))
				return;
		}
		else
		{
			// Nothing to choose between: no prereleases exist, or the only one is
			// the running build itself. **メニューから明示的に呼ばれている**ので、
			// 起動時に自動で走っていた頃と違って黙ってはいられない。
			if (others.empty())
			{
				host.Inform("ほかに選べる開発版ビルドはありません。",
							"現在: " + runningBranch + " (" + runningCommit + ")");
				return;
			}

			// One drop-down listing everything: entry 0 is the installed build,
			// entries 1.. are the other branches' prereleases.
			std::vector<std::string> items;
			items.push_back("現在: " + runningBranch + " (" + runningCommit +
							") ― インストール済み");
			for (const DevBuild& b : others)
				items.push_back(DevBuildLabel(b) + "  (" + b.commit + ")");

			int const sel = host.PickBuild(items, /*initialSel*/ 0);
			if (sel < 0)
				return; // cancelled -> keep the loaded build

			// Map the selection back to a candidate (entry 0 or an out-of-range
			// value both mean "keep the installed build"). See ResolveDevSelection.
			int const idx = ResolveDevSelection(static_cast<short>(sel), others.size());
			if (idx < 0)
				return;
			pick = others[static_cast<std::size_t>(idx)];
		}

		// A different build was chosen: install it, then offer the restart that
		// actually loads it (or hot-reload the payload, if only that changed).
		std::string err;
		std::string installedShellId;
		if (Install(host, pick.url, kDevPluginName, installedShellId, err))
			FinishInstall(host, "開発版ビルドをインストールしました。",
						  "branch: " + DevBuildLabel(pick) + "\ncommit: " + pick.commit,
						  runningShellId, installedShellId);
		else
			host.Inform("インストールに失敗しました。", err);
	}
} // namespace HomeskzIfcImport
