//
//	FeedbackLoop.h
//
//	**モードレスの往復（M24）の駆動。** 実機フィードバックの往復（docs/DEV-NOTES.md M23）は
//	「投稿したら戻り、次の周は人がメニューを 1 回押して始める」形だった——プラグインが
//	自分で待つとモーダルのダイアログが図面を塞ぐためである。M24 はその待機を**モードレスの
//	パレット**（src/Extensions/ExtFeedbackPalette.h）に載せ替え、パレットの JS タイマーが
//	周期的にここを叩く。人がするのは 1 周目のメニュー 1 回だけで、以後は
//
//	    合図を見る → 新しいビルドを見る → 入れて本体を降ろす → 取り込み（本体）→ 投稿
//
//	が **Claude が「もう要らない」と合図するまで**（または人がパレットで止めるまで）回る。
//
//	【なぜ殻にあるか】入れ替えは殻でしか起きない（本体のコードがスタックに載っている間は
//	降ろせない。src/PayloadSession.h）。この駆動は「自動アップデートの、往復のための顔」
//	であり、殻に残る唯一の実処理である自動アップデート（CLAUDE.md「殻と本体」）の一部と
//	して置く。**取り込みそのものは相変わらず本体**（draw::runTestRound。M25 で本番の
//	取り込みコマンドから分けた）が行う。
//
//	【SDK 非依存】UpdaterFlow.cpp と同じ作法で、副作用は IFeedbackLoopHost の後ろへ出す。
//	このファイルと FeedbackLoop.cpp は SDK のヘッダを 1 つも include せず、無 SDK で
//	テストできる（tests/FeedbackLoopTests.cpp）。実物の host は
//	src/FeedbackLoopHost.cpp（SDK 依存）。
//
//	【止まる条件】次のどれか。**どれも記憶（core::FeedbackSession）は消さない**——人が
//	メニューから取り込みを実行すれば続きの周として走り、そこでまた回り出す。
//	  * 人がパレットで「往復を止める」を押した
//	  * PR に Claude の合図（`<!-- homeskz-ifc-feedback v1 control=stop -->`）が付いた
//	  * PR が閉じた／マージされた
//	  * 入れられなかった・殻まで変わった・取り込めなかった・投稿できなかった
//	確認そのものができなかった（オフライン等）ときは止めず、次の周期でもう一度見る。
//
//	【再入】取り込みの最中は進捗ダイアログが VW にイベントを処理させる（DoYield）ので、
//	その間に JS のタイマーがもう一度ここを叩きうる。**走っている間の Tick は何もしない**。
//

#pragma once

#include <string>
#include <vector>

namespace HomeskzIfcImport
{
	// 本体（ペイロード）が持つ往復の記憶のうち、駆動に要るもの（src/PayloadAbi.h の
	// vw_payload_loop_status を解いたもの）。
	struct FeedbackLoopMemory
	{
		bool active = false; // 自動の往復が回っているか（send かつ round>0 かつ loop）
		std::string repo;		  // 投稿先 owner/repo
		int pullRequest = 0;	  // 投稿先 PR
		std::string branch;		  // 追いかけているブランチ
		int round = 0;			  // 済んだ周回数
		std::string lastCommit;	  // 直近の周のビルド
		std::string lastPostedAt; // 直近の投稿の時刻（合図を探す since）
	};

	// 新しいビルドの確認の結末（UpdaterHost.h の PollDevBuildWith を host が包む）。
	enum class FeedbackLoopBuild
	{
		NoNewBuild,
		Installed,
		NeedsRestart,
		Failed,
		CheckFailed,
	};
	struct FeedbackLoopBuildResult
	{
		FeedbackLoopBuild outcome = FeedbackLoopBuild::NoNewBuild;
		std::string commit;
		std::string message;
	};

	// 駆動が行う副作用。1 操作 1 メソッド。
	struct IFeedbackLoopHost
	{
		virtual ~IFeedbackLoopHost() = default;

		// 本体から記憶を読む。本体を読めなければ false（error に理由）。
		virtual bool QueryMemory(FeedbackLoopMemory& out, std::string& error) = 0;

		// 同梱スクリプト vw-feedback を走らせて標準出力を受け取る（loop-control）。
		virtual bool RunFeedbackScript(const std::vector<std::string>& args, std::string& out) = 0;

		// 同じブランチの新しいビルドを探し、あれば入れて本体を降ろす（尋ねない・報せない）。
		virtual FeedbackLoopBuildResult PollBuild(const std::string& branch) = 0;

		// 本体の取り込みを 1 周走らせる。posted は投稿できたか（＝続きの周が成立したか）。
		// 呼べなかったときだけ false（error に理由）。
		virtual bool RunRound(bool& posted, std::string& error) = 0;

		// 本体に「自動の往復を止めた」と伝える（記憶の loop を下ろす。notifyPr なら PR へ
		// 終えたことを 1 通投稿する）。
		virtual void EndLoop(const std::string& reason, bool notifyPr) = 0;
	};

	// パレットに見せる状態。JS 側はこれをそのまま並べる。
	enum class FeedbackLoopPhase
	{
		Idle,	 // 往復は回っていない（記憶が無い・loop が下りている）
		Waiting, // 回っている。新しいビルドを待っている
		Working, // 入れている／取り込んでいる（この間の Tick は何もしない）
		Stopped, // 止まった（message に理由）
	};
	struct FeedbackLoopView
	{
		FeedbackLoopPhase phase = FeedbackLoopPhase::Idle;
		std::string message; // 人に見せる 1 行
		std::string repo;	 // 以下、記憶の写し（Idle でも最後に読めたもの）
		int pullRequest = 0; //
		std::string branch;	 //
		int round = 0;		 //
		std::string build;	 // 直近の周のビルド
		long long lastCheckAt = -1; // 最後に GitHub を見た時刻（秒。未確認なら -1）
		long long nextCheckAt = -1; // 次に見る時刻（秒。Waiting のときだけ）
	};

	// -----------------------------------------------------------------------
	// 駆動そのもの。**状態はこれ 1 つが持つ**（殻の中で 1 つだけ作られる。
	// src/FeedbackLoopHost.cpp）。
	class FeedbackLoopDriver
	{
	public:
		// checkIntervalSeconds: GitHub を見に行く間隔。JS のタイマーはもっと細かく叩いて
		// よく、間隔に満たない Tick は何もせず現在の見え方を返す。
		explicit FeedbackLoopDriver(long long checkIntervalSeconds = 60);

		// メニューから往復に入った（投稿できた）。**次の Tick で間隔を待たずに見る。**
		void Arm();

		// JS のタイマーから。now は単調な秒（起点は問わない）。
		FeedbackLoopView Tick(IFeedbackLoopHost& host, long long now);

		// パレットの「往復を止める」。PR へ終えたことを投稿する。
		FeedbackLoopView Stop(IFeedbackLoopHost& host);

		// 「今すぐ確認」（間隔を待たずに次の Tick で見る）。
		void CheckNow();

		const FeedbackLoopView& View() const
		{
			return fView;
		}

	private:
		// 1 回ぶんの確認（合図 → ビルド → 取り込み）。fView を書き換える。
		void Check(IFeedbackLoopHost& host, long long now);
		void Stopped(IFeedbackLoopHost& host, const std::string& reason, bool notifyPr);

		long long fInterval;
		long long fLastCheck = -1;
		bool fForceCheck = false;
		bool fBusy = false;
		FeedbackLoopView fView;
	};
} // namespace HomeskzIfcImport
