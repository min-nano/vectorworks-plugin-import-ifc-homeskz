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

		// 載っている本体（ペイロード）を降ろす。**インストールした本体をこの実行のまま
		// 効かせるための最後の一押し**で、次に本体を使うとき（取り込み・PIO のリセット）に
		// 新しいファイルが読み直される（src/PayloadSession.h）。降ろせなかった——本体の
		// コードがまだ走っている——ときだけ false。
		//
		// 更新の確認は本体を確保する**前**に走るので（src/Extensions/ExtMenu.cpp）、
		// ここが呼ばれる時点で本体はスタックに載っていない——降ろせるのが常態である。
		// 降ろせなかったときだけ false になり、反映は次の起動へ回る。
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

	// **更新の確認をどこから起こしたか。** 分かれるのは「新しいビルドが無かった」
	// ときの振る舞いだけで、更新があるときの流れ（尋ねて入れて、要るなら再起動）は
	// 同じである。
	enum class UpdateCheckKind
	{
		// メニューコマンド「アップデータを確認」から。**必ず結果を伝える**——
		// 押したのに何も起きないのでは、確認できたのか、そもそも動いていないのかが
		// 分からない。取得に失敗した（オフライン等）ときもその旨を出す。
		Manual,
		// 取り込みコマンドのついで。**更新があるときだけ口を開く**——取り込みたい人の
		// 前に「最新です」を挟まない。取得に失敗しても黙って取り込みへ進む。
		Silent,
	};

	// The SDK-independent update flows, parameterized by the host above. These
	// hold NO state, so tests can drive them repeatedly. shellBranch/shellCommit は
	// **殻にコンパイルされた**ブランチと sha（実行時は VW_BUILD_BRANCH /
	// VW_BUILD_VERSION、テストでは注入する）。**「いま動いているビルド」そのものでは
	// ない**——本体だけを入れ替えたあとは前のブランチを名乗ったままなので、開発版の流れは
	// ディスク上のビルドを基準にし、分からないときだけこの 2 つへ落ちる
	// （src/UpdaterParse.h の ResolveCurrentDevBuild）。
	// runningShellId は**いま動いている殻の ID**（コンパイル時に焼かれた VW_SHELL_ID。
	// テストでは注入する）。入れたビルドの殻が同じなら、本体を読み直すだけで反映される
	// ＝**再起動を尋ねない**（src/UpdaterParse.h の NeedsRestartAfterInstall）。
	//
	// 戻り値は「**この実行のまま取り込みへ進んでよいか**」——入れると答えたのに入れられ
	// なかったときだけ false（呼び出し側は取り込みを見送る。src/Extensions/ExtMenu.cpp）。
	// 結末そのものはその場のダイアログで伝え終えている。
	bool RunStableUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
								  const std::string& runningShellId);

	// 開発版。**kind で挙動が大きく変わる唯一の流れ**:
	//   Manual … ビルドの選択ダイアログを出す（どのブランチのビルドを使うかを選ぶ）。
	//   Silent … ダイアログは出さず、**いま動いているのと同じブランチ**の新しいビルド
	//            だけを拾って尋ねる。取り込みのたびにブランチ選択が出ては邪魔になる。
	bool RunDevUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
							   const std::string& shellBranch, const std::string& shellCommit,
							   const std::string& runningShellId);

	// -----------------------------------------------------------------------
	// **MCP から頼まれた、尋ねも報せもしない開発版の入れ替え**（M38。道具 `vw_update`）。
	// Claude は自分で push したビルドを入れたくて頼んでいるので、「インストールしますか？」も
	// 「入れました」も出さず、**結末を値で返す**——ローカルの Claude Code がそれを読んで、
	// 再起動が要るか・取り込みへ進めるかを決める（src/Extensions/ExtMcpPalette.cpp）。
	// モーダルのダイアログを出すと、誰も見ていない Vectorworks が止まる。
	//
	// wantedBranch が空なら**いま入っているビルドのブランチ**の新しいビルドを拾う。名指し
	// すればそのブランチの最新を入れる（別の PR へ乗り換えるとき）。どちらも、いま入って
	// いるのと同じ sha は拾わない（UpdaterParse.h の DevSwitchCandidates）。
	//
	// **基準はディスク上に入っているビルド**（`q-dev` の `installed=` /
	// `installed-branch=`）。殻にコンパイルされた値は本体だけを入れ替えたあと古いまま
	// なので（殻は起動時にしか読み直されない）、sha を取り違えれば同じビルドを入れ直し、
	// **ブランチを取り違えれば乗り換えたはずのブランチへ戻してしまう**。分からないときだけ
	// shellBranch / shellCommit へ落ちる（src/UpdaterParse.h の ResolveCurrentDevBuild）。
	//
	// **インストールの経路はこのファイルの Install ただ 1 つ**で、手で押した確認と同じものを
	// 通る（CLAUDE.md「更新と配置の要点」）。
	enum class RemoteUpdateOutcome
	{
		NoNewBuild, // そのブランチに、いま入っているのと別のビルドは無い
		Installed, // 入れて本体を降ろした（次の呼び出しから新しい本体が動く）
		NeedsRestart, // 入れたが殻まで変わった（再起動するまで効かない）
		Failed,		  // 入れられなかった・降ろせなかった（message に理由）
		CheckFailed,  // 確認そのものができなかった（オフライン等）
	};
	struct RemoteUpdateResult
	{
		RemoteUpdateOutcome outcome = RemoteUpdateOutcome::NoNewBuild;
		std::string branch;	  // 探したブランチ
		std::string previous; // 入れる前に入っていたビルドの sha
		std::string commit;	  // Installed / NeedsRestart のとき、入れたビルドの sha
		std::string message;  // 人に見せる 1 行（Failed / CheckFailed / NeedsRestart）
	};
	RemoteUpdateResult RemoteDevUpdateWith(IUpdaterHost& host, const std::string& shellBranch,
										   const std::string& shellCommit,
										   const std::string& runningShellId,
										   const std::string& wantedBranch);
} // namespace HomeskzIfcImport
