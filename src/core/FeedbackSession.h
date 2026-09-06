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

		// 投稿したあと、新しいビルドを待って自動で取り込み直すか。false なら 1 周で終わる
		// （投稿だけしたい・手元で続きを見たい場合）。
		bool autoContinue = false;

		// 投稿する本文から、案件が分かるもの（ファイル名・パス・ユーザー名）を伏せるか。
		// **既定は伏せる**——PR コメントは公開されるので、既定が「出す」であってはならない。
		bool anonymize = true;

		// 済んだ周回数（1 周目の投稿で 1 になる）。コメントの見出しに出る。
		int round = 0;

		// 直近の周で動いていたビルドの短縮 sha。**新しいビルドかどうかの判定に使う**
		// （同じ sha のビルドを取り込み直しても意味が無い）。
		std::string lastCommit;

		// 直近の周の要素内訳（parse::formatTally の 1 行表現）。次の周のコメントで
		// **「前回からどう変わったか」**を出すために持つ——数字の羅列を 2 つ並べて
		// 読み比べさせるのでは、往復を減らした意味が薄い。
		std::string lastTally;
	};

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
