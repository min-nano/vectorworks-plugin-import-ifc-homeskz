//
//	UpdaterFlow.cpp
//
//	The two update flows, written against IUpdaterHost (UpdaterHost.h) instead of
//	the SDK. This file includes NO Vectorworks header, so it compiles and runs on
//	a plain toolchain and is linked into both the plug-in and the unit tests. All
//	decisions delegate to the pure helpers in UpdaterParse.h; all side effects go
//	through the injected host.
//
//	**いつ実行されるか。**
//	  * メニューコマンド「アップデータを確認」……… UpdateCheckKind::Manual
//	  * 取り込みコマンド・実機テストに付随して …… UpdateCheckKind::Silent
//	の入口から呼ばれる。異なるのは**どの場面で利用者へ表示するか**だけで（UpdaterHost.h の
//	UpdateCheckKind）、更新があるときの流れ——インストールして、要るなら再起動——は同じで
//	ある。もう 1 つ、MCP の `vw_update` から呼ばれる RemoteDevUpdateWith があり、こちらは
//	ダイアログを 1 枚も出さずに結末を値で返す（M38）。以前は Vectorworks の起動時
//	（プラグインの読み込み中）に 1 度きり実行していた。
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
		// 配布物の名前（＝インストール先のフォルダ名・アセット名）と、利用者に表示する名前。
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
				// インストールしたビルドの殻の ID。**「再起動が要るか」はこれで決まる**
				// （UpdaterParse.h）。古いスクリプトはこの行を出力しないので空になり、
				// そのときは安全側＝再起動を尋ねる側として扱う。
				installedShellIdOut = InstalledShellId(out);
				return true;
			}

			errorOut = InstallErrorText(out, "インストールに失敗しました。");
			return false;
		}

		// ビルドを利用者に表示するときの名前。**ブランチ名があればそれ**——リリースの表示名
		// （name）は "Dev: <branch> (<sha>)" なので、コミットを並べると同じものが 2 度
		// 表示される。branch はこの列を出力しない古い同梱スクリプトでは空になるので、そのときは
		// 表示名で代用する（UpdaterParse.h の DevBuild）。
		std::string DevBuildLabel(const DevBuild& b)
		{
			return b.branch.empty() ? b.name : b.branch;
		}

		// **いまインストールされているビルドの表記は 1 か所で定める**（開発版の流れは 3 か所で
		// これを表示する——「ほかに選べるビルドはありません」「そのままです」、そして選択
		// ダイアログの先頭）。どれも同じ 1 つのビルドを指しているので、表記が揺れると同じ状態が
		// 別物に見える。
		std::string CurrentDevLabel(const CurrentDevBuild& current)
		{
			return "現在: " + current.branch + " (" + current.commit + ")";
		}

		// **確認そのものができなかった。** オフライン・GitHub の一時的な不調・同梱
		// スクリプトを起動できない、のいずれか。
		//
		// Manual（メニューコマンド）のときは**必ず伝える**——実行したのに何も起きないと、
		// 「最新だった」のか「そもそも動いていない」のかが利用者には区別できない。
		// Silent（取り込みに付随する確認）では何も表示せずに進む: 取り込みたいだけの人に、
		// 接続できないことを毎回知らせても意味が無い。
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

		// **入れ替えが済んだあとの結末。**
		//
		//   * 本体だけが新しくなった（殻の ID が同じ）… 読み込まれている本体をアンロード
		//     するだけで、次の取り込み・次の PIO リセットから新しいコードが動く。
		//     **再起動を尋ねない。**
		//   * 殻まで変わった …………………………………… 読み込めるのは次の起動だけなので、
		//     従来どおり再起動を尋ねる（OfferRestart）。
		//
		// 判断できないとき（スクリプトが ID を出力しない古い版など）は「要る」として扱う。
		//
		// プラグインは 2 つのモジュールに分かれていて（src/PayloadAbi.h）、Vectorworks が
		// 起動時にしか読み込めないのは**殻**だけ。中身（解析・描画・PIO の作図）は殻が
		// 自分で読み込む**本体**にあるので、この 2 通りになる。
		void OfferRestart(IUpdaterHost& host, const std::string& text, const std::string& detail);

		bool FinishInstall(IUpdaterHost& host, const std::string& text, const std::string& detail,
						   const std::string& runningShellId, const std::string& installedShellId)
		{
			if (!NeedsRestartAfterInstall(runningShellId, installedShellId))
			{
				// **ここでホットリロードが機能する。** アンロードしておけば、次に本体を使う
				// ときに新しいファイルが再読み込みされる（src/PayloadSession.h）。アンロード
				// できなかった——本体のコードがまだ実行中の——ときだけ、次回の起動へ回す。
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
				return true;
			}

			OfferRestart(host, text, detail);
			return true;
		}

		// 殻まで変わったときの結末。コンパイル済みの殻は起動時にしか読み込まれないので、
		// 新しいビルドは Vectorworks を再起動するまで動かない——だからこれは通知ではなく
		// **再起動ボタンを持つ質問**にしてある。「後で」を選んだら何もしない: いま閉じた
		// ダイアログが既に再起動の必要を告げているので、重ねて通知しても煩わしいだけである。
		// チャンネルごとの詳細（build / branch+commit）は `detail` で受け取り、共通の
		// 再起動の文言の上に表示する。
		void OfferRestart(IUpdaterHost& host, const std::string& text, const std::string& detail)
		{
			std::string advice = detail;
			if (!advice.empty())
				advice += "\n\n";
			// 再起動は Vectorworks 自身に要求する（SDK の CloseAllFilesAndQuitVectorworks。
			// src/Updater.cpp）。開いている文書は通常どおり保存を確認してから閉じられ、
			// そこで取り消せば Vectorworks は終了しない——だからそう表示する。
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

	bool RunStableUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
								  const std::string& runningShellId)
	{
		std::string out;
		if (!host.RunScript({"q-stable"}, out))
		{
			// 同梱スクリプトが見つからない／起動できない。
			ReportCheckFailed(host, kind, "アップデータを起動できませんでした。");
			return true;
		}

		// オフライン・GitHub の不調はスクリプトが error= で返す。
		const std::string scriptError = ValueOf(out, "error");
		if (!scriptError.empty())
		{
			ReportCheckFailed(host, kind, scriptError);
			return true;
		}

		StableStatus const st = EvaluateStable(out);
		if (!st.offerUpdate)
		{
			// 出力が不足している（リリースに配布 zip が無い等）のと、既に最新なのとは
			// 意味が違う——前者は**確認できていない**ので、そう伝える。
			if (st.latest.empty() || st.url.empty())
				ReportCheckFailed(host, kind, "リリースの情報が不完全です。");
			else if (kind == UpdateCheckKind::Manual)
				host.Inform(std::string(kStableDisplayName) + "は最新です。",
							"build: " + st.latest);
			return true;
		}

		std::string const shownInstalled = st.installed.empty() ? "none" : st.installed;
		if (!host.Ask("新しい安定版ビルドがあります。今すぐインストールしますか？",
					  "インストール済み: " + shownInstalled + "\n最新: " + st.latest,
					  "インストール", "後で"))
			return true;

		std::string err;
		std::string installedShellId;
		if (Install(host, st.url, kStablePluginName, installedShellId, err))
			return FinishInstall(host, std::string(kStableDisplayName) + "を更新しました。",
								 "build: " + st.latest, runningShellId, installedShellId);

		host.Inform("更新に失敗しました。", err);
		return false;
	}

	bool RunDevUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
							   const std::string& shellBranch, const std::string& shellCommit,
							   const std::string& runningShellId)
	{
		std::string out;
		if (!host.RunScript({"q-dev"}, out))
		{
			ReportCheckFailed(host, kind, "アップデータを起動できませんでした。");
			return true;
		}

		const std::string scriptError = ValueOf(out, "error");
		if (!scriptError.empty())
		{
			ReportCheckFailed(host, kind, scriptError);
			return true;
		}

		// **「いま」はディスク上にインストールされているビルドである**（UpdaterParse.h の
		// ResolveCurrentDevBuild）。殻にコンパイルされたブランチと sha は、本体だけを
		// 入れ替えたあとでは前のブランチを名乗ったままなので、それを基準にすると
		// 切り替えたはずのブランチへ切り替わらない（docs/DEV-NOTES.md M26）。
		CurrentDevBuild const current = ResolveCurrentDevBuild(out, shellBranch, shellCommit);

		// Candidates to switch TO: every prerelease except the installed build.
		std::vector<DevBuild> const others = DevSwitchCandidates(out, current.commit);

		// どのビルドをインストールするか。Manual は選ばせ、Silent は同じブランチのものだけを選ぶ。
		DevBuild pick;
		if (kind != UpdateCheckKind::Manual)
		{
			// **取り込みに付随する確認ではブランチ選択を出さない。** ここで選ぶのは「いま
			// 動いているのと同じブランチの、別のコミット」だけ——それだけが「自分のビルドが
			// 新しくなった」に該当する（UpdaterParse.h の FindDevBuildForBranch）。
			// ブランチが分からないときは何も選ばない＝何も表示せずに取り込みへ進む。
			int const idx = FindDevBuildForBranch(others, current.branch);
			if (idx < 0)
				return true;
			pick = others[static_cast<std::size_t>(idx)];

			if (!host.Ask("同じブランチの新しい開発版ビルドがあります。"
						  "今すぐインストールしますか？",
						  "branch: " + current.branch + "\nインストール済み: " + current.commit +
							  "\n新しいビルド: " + pick.commit,
						  "インストール", "後で"))
				return true;
		}
		else
		{
			// Nothing to choose between: no prereleases exist, or the only one is
			// the running build itself. **メニューから明示的に呼ばれている**ので、
			// 起動時に自動で実行していた頃と違って何も表示せずに終えることはできない。
			if (others.empty())
			{
				host.Inform("ほかに選べる開発版ビルドはありません。", CurrentDevLabel(current));
				return true;
			}

			// One drop-down listing everything: entry 0 is the installed build,
			// entries 1.. are the other branches' prereleases.
			std::vector<std::string> items;
			items.push_back(CurrentDevLabel(current) + " ― インストール済み");
			for (const DevBuild& b : others)
				items.push_back(DevBuildLabel(b) + "  (" + b.commit + ")");

			int const sel = host.PickBuild(items, /*initialSel*/ 0);
			if (sel < 0)
				return true; // cancelled -> keep the loaded build

			// Map the selection back to a candidate (entry 0 or an out-of-range
			// value both mean "keep the installed build"). See ResolveDevSelection.
			int const idx = ResolveDevSelection(static_cast<short>(sel), others.size());
			if (idx < 0)
			{
				// **選んだ結末は必ず返す。** 「現在のまま」を選んだときに何も表示せずに閉じると、
				// 別のものを選んだつもりの人には「選んだのに切り替わらない」と映り、
				// 選び間違えたのか何も起きなかったのかを区別できない（取り消したときだけは
				// 何も表示しなくてよい——それは「何もしない」という意思表示だから）。
				host.Inform("開発版ビルドはそのままです。", CurrentDevLabel(current));
				return true;
			}
			pick = others[static_cast<std::size_t>(idx)];
		}

		// A different build was chosen: install it, then offer the restart that
		// actually loads it (or hot-reload the payload, if only that changed).
		std::string err;
		std::string installedShellId;
		if (Install(host, pick.url, kDevPluginName, installedShellId, err))
			return FinishInstall(host, "開発版ビルドをインストールしました。",
								 "branch: " + DevBuildLabel(pick) + "\ncommit: " + pick.commit,
								 runningShellId, installedShellId);

		// **インストールできなかった。** 取り込みへは進ませない（インストールすると答えた人は
		// 新しいビルドで取り込むつもりでいる）。
		host.Inform("インストールに失敗しました。", err);
		return false;
	}

	// -----------------------------------------------------------------------
	// MCP の `vw_update` 向け（M38）。意図は UpdaterHost.h の RemoteDevUpdateWith 参照。
	// **host の Inform / Ask / PickBuild / Restart は呼ばない**——結末はすべて値で返す
	// （再起動するかは要求した側が決める。src/Extensions/ExtMcpPalette.cpp）。
	RemoteUpdateResult RemoteDevUpdateWith(IUpdaterHost& host, const std::string& shellBranch,
										   const std::string& shellCommit,
										   const std::string& runningShellId,
										   const std::string& wantedBranch)
	{
		RemoteUpdateResult result;
		std::string out;
		if (!host.RunScript({"q-dev"}, out))
		{
			result.outcome = RemoteUpdateOutcome::CheckFailed;
			result.message = "アップデータを起動できませんでした。";
			return result;
		}
		const std::string scriptError = ValueOf(out, "error");
		if (!scriptError.empty())
		{
			result.outcome = RemoteUpdateOutcome::CheckFailed;
			result.message = scriptError;
			return result;
		}

		// いまインストールされている版（ディスク上）。**ブランチも sha もディスクから読む**
		// （UpdaterHost.h）。
		CurrentDevBuild const current = ResolveCurrentDevBuild(out, shellBranch, shellCommit);
		result.previous = current.commit;
		result.branch = wantedBranch.empty() ? current.branch : wantedBranch;

		// そのブランチのビルドが 1 つも無ければ、名指しの誤り（UpdaterHost.h）。いま
		// インストールされているビルドも数える——それしか無いのは「新しいビルドが無い」である。
		std::vector<DevBuild> const all = ParseDevBuilds(out);
		if (FindDevBuildForBranch(all, result.branch) < 0)
		{
			result.outcome = RemoteUpdateOutcome::NoSuchBranch;
			result.message = NoSuchDevBranchMessage(all, result.branch);
			return result;
		}

		std::vector<DevBuild> const others = DevSwitchCandidates(out, current.commit);
		int const idx = FindDevBuildForBranch(others, result.branch);
		if (idx < 0)
		{
			result.outcome = RemoteUpdateOutcome::NoNewBuild;
			return result;
		}
		const DevBuild& pick = others[static_cast<std::size_t>(idx)];
		result.commit = pick.commit;

		std::string err;
		std::string installedShellId;
		if (!Install(host, pick.url, kDevPluginName, installedShellId, err))
		{
			result.outcome = RemoteUpdateOutcome::Failed;
			result.message = err;
			return result;
		}

		if (NeedsRestartAfterInstall(runningShellId, installedShellId))
		{
			// 殻まで変わった。インストールはされたが、この実行では反映できない。
			result.outcome = RemoteUpdateOutcome::NeedsRestart;
			result.message = "殻（プラグインのモジュール）まで変わったため、Vectorworks を"
							 "再起動するまで新しいビルドは動きません。";
			return result;
		}
		if (!host.DropLoadedPayload())
		{
			result.outcome = RemoteUpdateOutcome::Failed;
			result.message = "本体を降ろせませんでした（いま動いている処理があります）。";
			return result;
		}
		result.outcome = RemoteUpdateOutcome::Installed;
		return result;
	}
} // namespace HomeskzIfcImport
