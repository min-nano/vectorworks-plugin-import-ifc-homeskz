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
		// **実機フィードバックの往復の 2 周目以降**（docs/DEV-NOTES.md M23）。Silent と
		// 同じく「いま動いているのと同じブランチの新しいビルド」だけを拾うが、
		// **尋ねずに入れる**。往復はもともと「新しいビルドが出たら試す」ためのもので、
		// 周ごとに「インストールしますか？」を挟むのは、この仕組みが無くそうとしている
		// 手間そのものだから。入れ替えたことも黙っている——モーダルのダイアログは
		// Vectorworks を止めるので、絵を見ている人の前に立ちはだかる。
		//
		// **口を開くのは輪が止まるときだけ**（殻まで変わった・入れられなかった）。
		// そのときは false が返り、呼び出し側は自動の周回をやめる。
		Auto,
	};

	// The SDK-independent update flows, parameterized by the host above. These
	// hold NO state, so tests can drive them repeatedly. runningBranch/runningCommit
	// identify the build currently loaded (compiled-in at run time; injected in
	// tests).
	// runningShellId は**いま動いている殻の ID**（コンパイル時に焼かれた VW_SHELL_ID。
	// テストでは注入する）。入れたビルドの殻が同じなら、本体を読み直すだけで反映される
	// ＝**再起動を尋ねない**（src/UpdaterParse.h の NeedsRestartAfterInstall）。
	//
	// 戻り値は「**この実行のまま自動で続けてよいか**」——殻まで変わった・入れられ
	// なかった・降ろせなかったときに false。**見るのは Auto の呼び出し側だけ**で
	// （実機フィードバックの往復。src/Extensions/ExtMenu.cpp）、Manual・Silent の
	// 呼び出し側は捨ててよい: そちらの結末はその場のダイアログで伝え終えている。
	bool RunStableUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
								  const std::string& runningShellId);

	// 開発版。**kind で挙動が大きく変わる唯一の流れ**:
	//   Manual … ビルドの選択ダイアログを出す（どのブランチのビルドを使うかを選ぶ）。
	//   Silent … ダイアログは出さず、**いま動いているのと同じブランチ**の新しいビルド
	//            だけを拾って尋ねる。取り込みのたびにブランチ選択が出ては邪魔になる。
	//   Auto … Silent と同じものを拾い、**尋ねずに入れて黙って続ける**。
	bool RunDevUpdateCheckWith(IUpdaterHost& host, UpdateCheckKind kind,
							   const std::string& runningBranch, const std::string& runningCommit,
							   const std::string& runningShellId);
} // namespace HomeskzIfcImport
