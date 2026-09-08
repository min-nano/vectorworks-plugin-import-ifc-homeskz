//
//	core/FeedbackSession.h
//
//	**実機フィードバックの往復 1 セッションぶんの記憶。** 開発版（dev）ビルドで取り込みを
//	走らせたとき、その結果を PR へ投稿し、Claude が直した次のビルドを待って**同じ条件で
//	取り込み直す**——この繰り返しを回すために覚えておく値をまとめたもの（docs/DEV-NOTES.md
//	M23）。
//
//	【なぜ覚えるのか】1 周目は人が決める（どの IFC を・どのシンボルで・どの PR へ）。
//	2 周目からは**人の操作を 1 つも挟まずに**同じ条件で走らせたい——そうでないと
//	「ファイルを選び直し、設定を選び直し、ログを貼り直す」という往復が残り、自動化の
//	意味が無くなる。だから 1 周目の選択をそのままディスクへ置き、2 周目以降はここから読む。
//
//	【なぜ core/ に置くか】ImportOptions とまったく同じ立ち位置である:
//	  * 決めるのは描画側（draw/SettingsDialog・draw/Feedback）——PR 番号も IFC のパスも
//	    ダイアログでしか決まらない。
//	  * 使うのは解析側と描画側の両方（解析は options を、描画は残り全部を読む）。
//	SDK も STEP も知らない値なので core/ が置き場所になる（CLAUDE.md「依存の向き」）。
//
//	【書式】**key=value を 1 行 1 つ**（更新スクリプトの機械可読出力と同じ流儀）。JSON に
//	しないのは、読み書きするのがこのファイルと単体テストだけで、値がすべて平たいから——
//	パーサを持ち込むより、`=` の左右で切るほうが小さく確実に済む。値に改行は入れない
//	（入っていたら捨てる。パスに改行が含まれることは実際上ない）。
//
//	【SDK 非依存】core/ は VectorWorks SDK を include しない。ファイルの読み書きも
//	標準ライブラリだけで完結するので、無 SDK で単体テストできる（core/Trace と同じ）。
//

#pragma once

#include "core/ImportOptions.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 往復 1 セッションぶんの記憶。**既定値は「何もしない」**——記憶が無い（＝1 周目の）
	// ときにそのまま使っても、従来どおりの手動の取り込みになる。
	struct FeedbackSession
	{
		// 結果を PR へ投稿するか。false ならこの仕組みは丸ごと動かない。
		bool send = false;

		// 投稿先。repo は "owner/repo"、pullRequest は PR 番号（0 なら投稿しない）。
		// **repo を持たせてあるのは、公開したくない往復を私有リポジトリへ逃がすため**
		// （docs/DEVELOPMENT.md「フィードバックの往復」）。既定は本リポジトリ。
		std::string repo;
		int pullRequest = 0;

		// 追いかけるブランチ（＝1 周目に動いていた dev ビルドのブランチ）。次の周回で
		// 取りに行くのは**このブランチの新しいビルドだけ**で、他のブランチの dev
		// プレリリースへ勝手に乗り換えない。
		std::string branch;

		// 1 周目に選んだ IFC のパスと取り込み設定。2 周目以降はこれをそのまま使う。
		std::string ifcPath;
		ImportOptions options;

		// 投稿する本文から、案件が分かるもの（ファイル名・パス・ユーザー名）を伏せるか。
		// **既定は伏せる**——PR コメントは公開されるので、既定が「出す」であってはならない。
		bool anonymize = true;

		// 済んだ周回数（1 周目の投稿で 1 になる）。コメントの見出しに出る。
		int round = 0;

		// 直近の周で動いていたビルドの短縮 sha。**新しいビルドかどうかの判定に使う**
		// （同じ sha のビルドを取り込み直しても意味が無い）。
		std::string lastCommit;

		// **1 周目の「取り込み前に在ったレイヤ」の顔ぶれ**（core::DrawCounts::existingLayers）。
		// 次の周でこれと引き比べ、図面が取り込み前へ戻してあるかを見る——テンプレートに
		// 「共通」等が最初から在ると真偽 1 つでは「戻し忘れ」と区別できないため、**基準は
		// 1 周目に採る**（docs/DEV-NOTES.md M23「基準は 1 周目に採る」）。
		//
		// baselineRecorded は「採ってあるか」。**空の基準（＝まっさらな図面で始めた 1 周目）と
		// 古い版が書いた記憶を区別する**ために要る——真偽が無いと、どちらも「空」に見える。
		bool baselineRecorded = false;
		std::vector<std::string> baselineLayers;

		// 直近の周の要素内訳（parse::formatTally の 1 行表現）。次の周のコメントで
		// **「前回からどう変わったか」**を出すために持つ——数字の羅列を 2 つ並べて
		// 読み比べさせるのでは、往復を減らした意味が薄い。
		std::string lastTally;

		// **直近の投稿の時刻**（GitHub が返した created_at。ISO 8601 / UTC）。モードレスの
		// 往復（M24）が「止めろ」の合図を探すとき、**この時刻より後のコメントだけ**を読む
		// ためのもの——読む範囲を切らないと、一度読んだ古い合図を毎回読み直すことになる。
		// 空なら「最近のコメント」から読む（同梱スクリプトの loop-control）。
		std::string lastPostedAt;

		// **自動の往復（M24）が回っているか。** 投稿できた周の終わりに立ち、止まったとき
		// （人がパレットで止めた・Claude が合図した・PR が閉じた・入れ替えに失敗した）に
		// 下りる。**下りても記憶は消さない**——人がメニューから取り込みを実行すれば
		// 続きの周として走り、そこでまた立つ（止めたことが「往復を最初からやり直す」に
		// ならないように。docs/DEV-NOTES.md M24）。
		bool loop = false;
	};

	// -----------------------------------------------------------------------
	// **実機テストの周がどれになるか**（M25）。`draw/Feedback` の `runTestRound` が最初に
	// 通す判断で、**SDK も IFC も知らない純粋な場合分け**なのでここに置き、無 SDK でテスト
	// する（CLAUDE.md「テスト方針」——描画側から切り離せる計算は core/ へ寄せる）。
	//
	// **判断そのものが M25 の要点である。** M24 まではここが「記憶があって同じビルドなら、
	// 新しい 1 周目として取り込み直す」で、パレットが開いている最中に人がメニューを押すと
	// 同じ round が二重に投稿された（実機で発生。docs/DEV-NOTES.md M25）。
	enum class FeedbackRoundKind
	{
		// 記憶が無い（別ブランチの記憶は呼び出し側が捨ててから渡す）。宛先と伏せ字を
		// 尋ねてから 1 周目を走らせる。
		FirstRound,
		// 記憶があり、**動いているビルドがそれと違う**。何も尋ねずに続きの周を走らせる。
		ContinueRound,
		// 記憶があり、**同じビルドが動いている**。取り込んでも前の周と同じ数字が並ぶだけ
		// なので**走らせず**、往復を回す（止まっていたら回し直す）だけにする。
		RearmOnly,
		// ダイアログを出せない場面（パレットの周）なのに、尋ねないと始められない。
		// 何もしない。
		Refuse,
	};

	// runningCommit は**いま動いている本体の短縮 sha**。allowDialogs はダイアログを出して
	// よいか（メニューから実行したとき true、パレットの周は false）。
	FeedbackRoundKind feedbackRoundKind(const FeedbackSession& session,
										const std::string& runningCommit, bool allowDialogs);

	// 記憶を key=value テキストへ（末尾は改行）。**行の順は固定**——差分を取ったときに
	// 中身の変化だけが見えるようにするため。
	std::string formatFeedbackSession(const FeedbackSession& session);

	// key=value テキストから記憶を復元する。**知らない行・壊れた行は黙って飛ばす**
	// （古い版が書いたファイルを読めなくして往復を止めない）。値の無い項目は既定のまま。
	FeedbackSession parseFeedbackSession(const std::string& text);

	// 記憶の置き場所。**一時ディレクトリには置かない**（消えると 2 周目が走らない）:
	//   macOS   … $HOME/Library/Application Support/HomeskzIfcImport/feedback.txt
	//   Windows … %LOCALAPPDATA%\HomeskzIfcImport\feedback.txt
	// どちらの環境変数も取れなければ空を返す（呼び出し側は記憶を諦めて 1 周で終わる）。
	// **環境変数 HOMESKZ_IFC_FEEDBACK_STATE が指定されていればそれを優先する**（試験用）。
	std::string defaultFeedbackSessionPath();

	// 読み書き。読めなければ false（＝記憶が無い＝1 周目）。書けなければ false
	// （往復は続けられるが 2 周目が走らないので、呼び出し側はログに残す）。
	bool readFeedbackSession(const std::string& path, FeedbackSession& out);
	bool writeFeedbackSession(const std::string& path, const FeedbackSession& session);

	// 記憶を消す（セッションを畳むとき）。無ければ何もしない。
	void clearFeedbackSession(const std::string& path);
} // namespace HomeskzIfcImport::core
