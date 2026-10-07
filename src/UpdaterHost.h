//
//	UpdaterHost.h
//
//	The seam that lets the update FLOWS be tested without the Vectorworks SDK.
//
//	RunStableUpdateCheck / RunDevUpdateCheck are two small state machines:
//	"ask the script, decide, maybe show a dialog, maybe install, report". The
//	decisions are already pure (UpdaterParse.h); what remained SDK-bound was the
//	side-effecting operations those flows perform:
//	  * run the bundled updater script and capture its stdout,
//	  * show an informational dialog,
//	  * ask a yes/no question,
//	  * show the build picker and return the chosen index,
//	  * restart Vectorworks (so a freshly installed build is loaded).
//	Those are gathered behind IUpdaterHost. The flows (UpdaterFlow.cpp)
//	depend ONLY on this interface, so they compile and run on any toolchain.
//
//	At run time Updater.cpp supplies the real implementation (gSDK dialogs +
//	popen'd script + the VWFC picker). In tests a fake implementation records the
//	calls and returns canned answers, so the whole flow — every branch and the
//	exact dialog wording — is exercised without the SDK (tests/UpdaterFlowTests.cpp).
//

#pragma once

#include <string>
#include <vector>

namespace HomeskzIfcImport
{
	// The side effects the update flows perform. One method per operation that
	// would otherwise touch the SDK / the OS.
	struct IUpdaterHost
	{
		virtual ~IUpdaterHost() = default;

		// Run the bundled updater script with the given args and capture its
		// stdout into `out`. Returns false if the script could not be started
		// (missing/unresolved) — the flows treat that as "stay silent".
		virtual bool RunScript(const std::vector<std::string>& args, std::string& out) = 0;

		// Show a modal informational dialog (text + a secondary advice line).
		virtual void Inform(const std::string& text, const std::string& advice) = 0;

		// Ask a yes/no question. Returns true if the user chose the affirmative
		// (okText) button.
		virtual bool Ask(const std::string& text, const std::string& advice,
						 const std::string& okText, const std::string& cancelText) = 0;

		// Show the build picker listing `items` (entry 0 is the installed build),
		// preselecting `initialSel`. Returns the chosen 0-based index, or a
		// negative value if the user cancelled.
		virtual int PickBuild(const std::vector<std::string>& items, int initialSel) = 0;

		// 読み込まれている本体（ペイロード）をアンロードする。**インストールした本体をこの
		// 実行のまま反映させるための最後の処理**で、次に本体を使うとき（取り込み・PIO の
		// リセット）に新しいファイルが再読み込みされる（src/PayloadSession.h）。アンロード
		// できなかった——本体のコードがまだ実行中の——ときだけ false。
		//
		// 更新の確認は本体を確保する**前**に実行されるので（src/Extensions/ExtMenu.cpp）、
		// ここが呼ばれる時点で本体はスタックに載っていない——アンロードできるのが通常である。
		// アンロードできなかったときだけ false になり、反映は次の起動へ回る。
		virtual bool DropLoadedPayload() = 0;

		// Quit Vectorworks and start it again, so the build just installed is
		// actually loaded (a compiled plug-in is only ever picked up at start-up).
		// Returns false if the restart could not even be REQUESTED — Vectorworks is
		// then left running untouched and the flow tells the user to restart by
		// hand. A true return only means "the quit was requested": open documents
		// still get the usual save prompt, and backing out there simply leaves the
		// old build running until the next start-up.
		virtual bool Restart() = 0;
	};

	// **更新の確認をどこから実行したか。** 異なるのは「新しいビルドが無かった」
	// ときの振る舞いだけで、更新があるときの流れ（尋ねてインストールし、要るなら再起動）は
	// 同じである。
	enum class UpdateCheckKind
	{
		// メニューコマンド「アップデータを確認」から。**必ず結果を伝える**——
		// 実行したのに何も起きないのでは、確認できたのか、そもそも動いていないのかが
		// 分からない。取得に失敗した（オフライン等）ときもその旨を表示する。
		Manual,
		// 取り込みコマンドに付随する確認。**更新があるときだけ表示する**——取り込みたい人の
		// 前に「最新です」を挟まない。取得に失敗しても何も表示せずに取り込みへ進む。
		Silent,
	};

	// The SDK-independent update flows, parameterized by the host above. These
	// hold NO state, so tests can drive them repeatedly. shellBranch/shellCommit は
	// **殻にコンパイルされた**ブランチと sha（実行時は VW_BUILD_BRANCH /
	// VW_BUILD_VERSION、テストでは注入する）。**「いま動いているビルド」そのものでは
	// ない**——本体だけを入れ替えたあとは前のブランチを名乗ったままなので、開発版の流れは
	// ディスク上のビルドを基準にし、分からないときだけこの 2 つを使う
	// （src/UpdaterParse.h の ResolveCurrentDevBuild）。
	// runningShellId は**いま動いている殻の ID**（コンパイル時に埋め込まれた VW_SHELL_ID。
	// テストでは注入する）。インストールしたビルドの殻が同じなら、本体を再読み込みするだけで
	// 反映される＝**再起動を尋ねない**（src/UpdaterParse.h の NeedsRestartAfterInstall）。
	//
	// 戻り値は「**この実行のまま取り込みへ進んでよいか**」——インストールすると答えたのに
	// インストールできなかったときだけ false（呼び出し側は取り込みを見送る。
	// src/Extensions/ExtMenu.cpp）。
	// 結末そのものはその場のダイアログで伝え終えている。
	bool RunStableUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
								  const std::string& runningShellId);

	// 開発版。**kind で挙動が大きく変わる唯一の流れ**:
	//   Manual … ビルドの選択ダイアログを出す（どのブランチのビルドを使うかを選ぶ）。
	//   Silent … ダイアログは出さず、**いま動いているのと同じブランチ**の新しいビルド
	//            だけを選んで尋ねる。取り込みのたびにブランチ選択が出ては作業の妨げになる。
	bool RunDevUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
							   const std::string& shellBranch, const std::string& shellCommit,
							   const std::string& runningShellId);

	// -----------------------------------------------------------------------
	// **MCP から要求された、確認も通知もしない開発版の入れ替え**（M38。道具 `vw_update`）。
	// 「インストールしますか？」も「インストールしました」も表示せず、**結末を値で返す**
	// ——ローカルの Claude Code がそれを読んで、再起動が要るか・取り込みへ進めるかを決める
	// （src/Extensions/ExtMcpPalette.cpp）。Claude は自分で push したビルドをインストール
	// したくて要求しているので、確認は要らない。モーダルのダイアログを出すと、誰も見ていない
	// Vectorworks が止まる。
	//
	// wantedBranch が空なら**いまインストールされているビルドのブランチ**の新しいビルドを
	// 選ぶ。ブランチを指定すればそのブランチの最新をインストールする（別の PR へ切り替える
	// とき）。どちらも、いまインストールされているのと同じ sha は選ばない（UpdaterParse.h の
	// DevSwitchCandidates）。
	//
	// **「新しいビルドが無い」と「そのブランチのビルドが無い」は区別する**（NoNewBuild /
	// NoSuchBranch）。開発版は PR のブランチからしかビルドされない（main は安定版）ので、
	// main やタイプミスしたブランチを名指しされたときに「新しいビルドは無い」と返すと、
	// 待てばビルドされるかのように読めてしまう。
	//
	// **基準はディスク上にインストールされているビルド**（`q-dev` の `installed=` /
	// `installed-branch=`）。分からないときだけ shellBranch / shellCommit を使う
	// （src/UpdaterParse.h の ResolveCurrentDevBuild）。殻にコンパイルされた値は本体だけを
	// 入れ替えたあと古いままなので（殻は起動時にしか再読み込みされない）、sha を取り違えれば
	// 同じビルドを再インストールし、**ブランチを取り違えれば切り替えたはずのブランチへ戻して
	// しまう**。
	//
	// **インストールの経路はこのファイルの Install ただ 1 つ**で、手で実行した確認と同じ経路を
	// 通る（CLAUDE.md「更新と配置の要点」）。
	enum class RemoteUpdateOutcome
	{
		NoNewBuild, // そのブランチに、いまインストールされているのと別のビルドは無い
		NoSuchBranch, // そのブランチの開発版ビルドが 1 つも無い（名指しの誤り。message に理由）
		Installed, // インストールして本体をアンロードした（次の呼び出しから新しい本体が動く）
		NeedsRestart, // インストールしたが殻まで変わった（再起動するまで反映されない）
		Failed, // インストールできなかった・アンロードできなかった（message に理由）
		CheckFailed, // 確認そのものができなかった（オフライン等）
	};
	struct RemoteUpdateResult
	{
		RemoteUpdateOutcome outcome = RemoteUpdateOutcome::NoNewBuild;
		std::string branch; // 探したブランチ
		std::string previous; // インストール前にインストールされていたビルドの sha
		std::string commit; // Installed / NeedsRestart のとき、インストールしたビルドの sha
		std::string message; // 利用者に表示する 1 行（Failed / CheckFailed / NeedsRestart / NoSuchBranch）
	};
	RemoteUpdateResult RemoteDevUpdateWith(IUpdaterHost& host, const std::string& shellBranch,
										   const std::string& shellCommit,
										   const std::string& runningShellId,
										   const std::string& wantedBranch);
} // namespace HomeskzIfcImport
