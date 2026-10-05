//
//	core/FeedbackSession.h
//
//	**実機テストの記憶。** 開発版（dev）ビルドの「実機テストを実行…」が、Claude が直した
//	次のビルドで**同じ条件で取り込み直す**ために覚えておく値をまとめたもの（docs/DEV-NOTES.md
//	M23 / M25 / M38）。
//
//	【なぜ覚えるのか】1 周目は人が決める（どの IFC を・どのシンボルで）。2 周目からは
//	**人の操作を 1 つも挟まずに**同じ条件で走らせたい——Claude が MCP の `vw_run_test` から
//	起こす周はダイアログを 1 枚も出せないので、1 周目の選択をそのままディスクへ置き、
//	2 周目以降はここから読む。M37 までは結果を PR へ投稿していたので宛先も覚えていたが、
//	いまは結果を手元に控えるだけになった（draw/Feedback.h）。
//
//	【なぜ core/ に置くか】ImportOptions とまったく同じ立ち位置である:
//	  * 決めるのは描画側（draw/SettingsDialog・draw/Feedback）——IFC のパスは
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
	// 実機テストの記憶。**既定値は「何もしない」**——記憶が無い（＝1 周目の）
	// ときにそのまま使っても、従来どおりの手動の取り込みになる。
	struct FeedbackSession
	{
		// **毎周の図面の元になるテンプレート（`.sta`）の絶対パス**（空なら 1 周目と同じく、
		// いま開いている図面から採る）。
		//
		// 1 周目に、そのとき開いている図面をこの場所へ**別名保存**し、以後の周は毎回これを
		// `OpenDocumentPath` で開く。`.sta` を渡すと**そのファイル自体ではなく、中身を
		// 写した無題の新規文書**が開く（SDK リファレンス Findings「Documents」）ので、
		// テンプレートには前の周の絵が 1 つも書き戻らず、どの周もまったく同じ初期状態から
		// 始まる。M38 までの「取り消し」「レイヤ削除」による戻しはやめた（M39。どちらも
		// 取り込み前から在ったレイヤへ描いた分や取り消しスタックの中身に左右された）。
		//
		// **これは実機テストの周だけの都合である。** 本番の取り込みは開いている図面へ描く
		// のが仕事なので、この値を見るのは draw/Feedback だけ。
		std::string templatePath;

		// **実機テストが自分で保存した図面のパス**（テンプレートと、各周の描き上がり）。
		//
		// 次の周の頭で、この中で**いま開いているものを保存せずに閉じる**（draw/Feedback の
		// closeOwnedDocuments）。各周の描き上がりは周の終わりに一時ファイルへ保存してある
		// ので、人が手を入れていなければ未保存の変更は無く、Vectorworks を再起動しても
		// 保存の確認が出ない。**閉じてよいのはここに名指しで在り、かつ一時ファイルの置き場
		// （core/FeedbackScratch）の中にあるものだけ**——利用者の図面を閉じない安全弁で、
		// `CloseDocument()` は確認なしに変更を捨てる（同 Findings）ので緩めない。
		std::vector<std::string> ownedDocuments;

		// 1 周目に選んだ IFC のパスと取り込み設定。2 周目以降はこれをそのまま使う。
		std::string ifcPath;
		ImportOptions options;

		// 済んだ周回数（1 周目が終わると 1 になる）。報告の見出しに出る。
		int round = 0;

		// 直近の周で動いていたビルドの短縮 sha（報告の「前の周からの変化」の見出しに出す）。
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

		// 直近の周の要素内訳（parse::formatTally の 1 行表現）。次の周の報告で
		// **「前回からどう変わったか」**を出すために持つ——数字の羅列を 2 つ並べて
		// 読み比べさせるのでは、往復を減らした意味が薄い。
		std::string lastTally;
	};

	// -----------------------------------------------------------------------
	// **実機テストの周がどれになるか**（M25）。`draw/Feedback` の `runTestRound` が最初に
	// 通す判断で、**SDK も IFC も知らない純粋な場合分け**なのでここに置き、無 SDK でテスト
	// する（CLAUDE.md「テスト方針」——描画側から切り離せる計算は core/ へ寄せる）。
	//
	// **同じビルドでも取り込む**（M38）。M37 までは「同じビルドなら取り込まない」で、往復の
	// パレットと人の手が同じ周を二重に投稿するのを防いでいた。投稿が無くなり、取り込み直す
	// かどうかは頼んだ側（人か Claude）が決めるので、その歯止めは要らなくなった。
	enum class FeedbackRoundKind
	{
		// 記憶が無い。IFC・設定を尋ねてから 1 周目を走らせる。
		FirstRound,
		// 記憶がある。前の周と同じ条件で続きの周を走らせる（メニューから押したときは、
		// 同じ条件でよいかを描画側が 1 度だけ尋ねる。draw/Feedback.cpp）。
		ContinueRound,
		// ダイアログを出せない場面（MCP の `vw_run_test`）なのに、尋ねないと始められない。
		// 何もしない（1 周目はメニューから人が実行する）。**テンプレートが無いときも
		// ここ**——人の居ない周に「いま開いている図面」を基準に採らせると、前の周の絵が
		// 載った図面や利用者の図面がそのまま基準になりうる（M39）。
		Refuse,
	};

	// allowDialogs はダイアログを出してよいか（メニューから実行したとき true、MCP は false）。
	FeedbackRoundKind feedbackRoundKind(const FeedbackSession& session, bool allowDialogs);

	// 記憶が「続きの周を組み立てられるだけ揃っているか」（1 周は済んでいて、その周の IFC が
	// 分かっている）。
	bool feedbackSessionRemembered(const FeedbackSession& session);

	// -----------------------------------------------------------------------
	// **いま開いている図面 openPath を、実機テストが保存せずに閉じてよいか**（M39 の安全弁）。
	//
	// `CloseDocument()` は確認なしに未保存の変更を捨てる（SDK リファレンス Findings
	// 「Documents」）ので、閉じる相手は次の 2 つを両方満たすものに限る:
	//   * 記憶（ownedDocuments）に**名指しで在る**——実機テストが自分で保存した図面である。
	//   * その記憶のパスが**一時ファイルの置き場（scratchRoot）の中**にある——記憶のファイルが
	//     壊れていたり手で書き換えられていたりしても、利用者の図面へは届かない。
	// 同じファイルかどうかは字面だけでなく std::filesystem にも尋ねる（macOS の一時
	// ディレクトリは /var と /private/var の 2 通りの綴りで返ってくる）。
	bool isOwnedTestDocument(const FeedbackSession& session, const std::string& openPath,
							 const std::string& scratchRoot);

	// -----------------------------------------------------------------------
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

	// **直近の実機テストの報告**（Markdown。parse::formatTestRoundReport）の置き場所。
	// 記憶と同じフォルダの last-round.md（記憶のパスが空なら空）。MCP の `vw_test_report`
	// がここを読む——本体を入れ替えても読めるよう、メモリではなくファイルに置く。
	std::string testReportPathFor(const std::string& sessionPath);
} // namespace HomeskzIfcImport::core
