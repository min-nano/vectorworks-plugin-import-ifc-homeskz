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
//	  * 実機フィードバックの往復の 2 周目以降 ……… UpdateCheckKind::Auto
//	の 3 つの入口から呼ばれる。分かれるのは**どこで口を開くか**だけで（UpdaterHost.h の
//	UpdateCheckKind）、更新があるときの流れ——入れて、要るなら再起動——は同じである。
//	Auto だけは尋ねも報せもせず、**輪が止まるときだけ**口を開いて false を返す。
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

		// **何も入れずに戻るときの答え。** Auto にとって「入らなかった」は「何も変わって
		// いない」——同じ周をもう一度回しても同じ結果が出るだけで、しかも本体は毎周
		// PR へコメントを投げるので、放っておくと同じ報告が並ぶ。だから輪を止める。
		// Manual / Silent の呼び出し側は戻り値を見ないので true でよい。
		bool NothingInstalled(UpdateCheckKind kind)
		{
			return kind != UpdateCheckKind::Auto;
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

		bool FinishInstall(IUpdaterHost& host, UpdateCheckKind kind, const std::string& text,
						   const std::string& detail, const std::string& runningShellId,
						   const std::string& installedShellId)
		{
			if (!NeedsRestartAfterInstall(runningShellId, installedShellId))
			{
				// **Auto は黙って入れ替える。** 往復の 1 周ごとにモーダルのダイアログを
				// 出しては、絵を見ている人の前に立ちはだかるだけになる。降ろせなかった
				// ときだけ輪を止める——古い本体のまま次の周を回しても、同じ結果が出る
				// だけで意味が無い。
				if (kind == UpdateCheckKind::Auto)
					return host.DropLoadedPayload();

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
				return true;
			}

			// **殻まで変わった。** Auto では再起動を仕掛けない——利用者は図面を開いた
			// まま輪を回しているので、勝手に終了させるわけにいかない。伝えて輪を止め、
			// 再起動するかどうかはその人に委ねる。
			if (kind == UpdateCheckKind::Auto)
			{
				host.Inform(text, detail + "\n\n殻（プラグインのモジュール）まで変わったため、"
										   "この実行では入れ替えられません。\n"
										   "Vectorworks を再起動してから、もう一度取り込みを実行"
										   "してください（同じ条件で続きから走ります）。");
				return false;
			}

			OfferRestart(host, text, detail);
			return true;
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

	// **安定版に Auto は無い。** 往復するのは PR のビルドであって main の配布物では
	// ないので、Auto で呼ばれることはそもそも無い。万一呼ばれても Silent と同じ
	// ——尋ねずに入れる相手ではない——として扱い、輪は止めない。
	bool RunStableUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
								  const std::string& runningShellId)
	{
		if (kind == UpdateCheckKind::Auto)
			kind = UpdateCheckKind::Silent;

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
			// 出力が足りない（リリースに配布 zip が無い等）のと、既に最新なのとは
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
			return FinishInstall(host, kind, std::string(kStableDisplayName) + "を更新しました。",
								 "build: " + st.latest, runningShellId, installedShellId);

		host.Inform("更新に失敗しました。", err);
		return false;
	}

	bool RunDevUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
							   const std::string& runningBranch, const std::string& runningCommit,
							   const std::string& runningShellId)
	{
		std::string out;
		if (!host.RunScript({"q-dev"}, out))
		{
			ReportCheckFailed(host, kind, "アップデータを起動できませんでした。");
			return NothingInstalled(kind);
		}

		const std::string scriptError = ValueOf(out, "error");
		if (!scriptError.empty())
		{
			ReportCheckFailed(host, kind, scriptError);
			return NothingInstalled(kind);
		}

		// Candidates to switch TO: every prerelease except the running build.
		std::vector<DevBuild> const others = DevSwitchCandidates(out, runningCommit);

		// どのビルドを入れるか。Manual は選ばせ、Silent は同じブランチのものだけを拾う。
		DevBuild pick;
		if (kind != UpdateCheckKind::Manual)
		{
			// **取り込みのついでにブランチ選択を出さない。** ここで拾うのは「いま動いて
			// いるのと同じブランチの、別のコミット」だけ——それだけが「自分のビルドが
			// 新しくなった」に当たる（UpdaterParse.h の FindDevBuildForBranch）。
			// ブランチが分からないときは何も拾わない＝黙って取り込みへ進む。
			int const idx = FindDevBuildForBranch(others, runningBranch);
			if (idx < 0)
				return NothingInstalled(kind);
			pick = others[static_cast<std::size_t>(idx)];

			// **Auto は尋ねない。** 往復は「新しいビルドが出たら試す」ためのもので、
			// 周ごとに確認を挟むのはこの仕組みが無くそうとしている手間そのもの
			// （UpdaterHost.h の UpdateCheckKind::Auto）。
			if (kind == UpdateCheckKind::Silent &&
				!host.Ask("同じブランチの新しい開発版ビルドがあります。"
						  "今すぐインストールしますか？",
						  "branch: " + runningBranch + "\nインストール済み: " + runningCommit +
							  "\n新しいビルド: " + pick.commit,
						  "インストール", "後で"))
				return true;
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
				return true;
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
				return true; // cancelled -> keep the loaded build

			// Map the selection back to a candidate (entry 0 or an out-of-range
			// value both mean "keep the installed build"). See ResolveDevSelection.
			int const idx = ResolveDevSelection(static_cast<short>(sel), others.size());
			if (idx < 0)
				return true;
			pick = others[static_cast<std::size_t>(idx)];
		}

		// A different build was chosen: install it, then offer the restart that
		// actually loads it (or hot-reload the payload, if only that changed).
		std::string err;
		std::string installedShellId;
		if (Install(host, pick.url, kDevPluginName, installedShellId, err))
			return FinishInstall(host, kind, "開発版ビルドをインストールしました。",
								 "branch: " + DevBuildLabel(pick) + "\ncommit: " + pick.commit,
								 runningShellId, installedShellId);

		// **入れられなかった。** Auto でもここは黙らない——輪が止まる理由を伝えないと、
		// 待っていた人には「同じ結果がもう一度出た」ようにしか見えない。
		host.Inform("インストールに失敗しました。", err);
		return false;
	}
} // namespace HomeskzIfcImport
