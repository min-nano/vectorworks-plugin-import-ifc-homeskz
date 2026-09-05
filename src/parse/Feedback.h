//
//	parse/Feedback.h
//
//	**実機フィードバックの本文**（docs/DEV-NOTES.md M23）。開発版ビルドで取り込みを走らせた
//	結果を、そのまま PR コメントとして投げられる Markdown へ組み立てる。
//
//	【なぜ要るか】`draw/` の実描画は CI では検証できず、ローカルの VectorWorks でしか
//	確かめられない（CLAUDE.md「テスト方針」）。その確認結果を人が手で写して伝えている限り、
//	1 往復ごとに「ビルドを入れ直す・ファイルを選び直す・ログを貼る」という手間が乗る。
//	**プラグイン自身に報告させれば、人がするのは「絵を見て一言書く」だけになる。**
//
//	【何を載せるか】
//	  * 結末・所要時間・**要素ごとの内訳**（parse/Summary の elementRows。表は 1 つきり）
//	  * **前の周からの差分**——読む側が知りたいのは絶対値ではなく「直した結果どう動いたか」
//	  * 描画側が持ち帰った注意（`DrawCounts::diagnostics`）と記録（`notes`）
//	  * 診断ログの全文（折り畳み）
//	  * **実機を見た人の所見**。自動化で決して代われない唯一のもので、いちばん上に置く。
//
//	【伏せるもの】PR コメントは公開される。だから既定で**案件が分かるものを伏せる**:
//	IFC のファイル名は同じ入力なら毎回同じになる仮名（`model-8f3a12.ifc`）へ、パスの
//	ユーザー名は伏せ字へ。数字・要素名・VW の診断は案件ではなくプラグインの話なので残す。
//	伏せない選択（私有リポジトリへ投げるとき）は core::FeedbackSession::anonymize が持つ。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を include しない。ここは Document と
//	DrawCounts を読むだけの純粋な文字列組み立てなので、無 SDK で単体テストできる。
//

#pragma once

#include "core/Document.h"
#include "parse/Summary.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// 1 周ぶんの材料。**描画側（draw/Feedback）が詰めて渡すだけ**で、ここは受け取った
	// 値を並べる（完了ダイアログの文言と同じ分担。parse/Summary.h）。
	struct FeedbackRound
	{
		BuildInfo build; // 動いていたビルド（ブランチ・コミット・プラットフォーム）
		std::string ifcPath; // 取り込んだ IFC の絶対パス（**伏せ字の材料**。そのままは出さない）
		unsigned long long bytes = 0; // 対象ファイルの大きさ（0 なら出さない）
		double seconds = 0.0;		  // 所要（0 以下なら出さない）
		std::string startedAt;		  // 壁時計（core::trace::localTimestamp）
		std::string note; // **実機を見た人の所見**（空なら節ごと出さない）
		std::string log;  // 診断ログ全文（core::trace::text）

		int round = 1;				// 何周目か（1 から）
		std::string previousCommit; // 前の周のビルド（空なら 1 周目）
		std::string previousTally;	// 前の周の内訳（formatTally の 1 行表現）

		bool anonymize = true;	  // 案件が分かるものを伏せるか
		bool autoContinue = true; // 次の周を自動で回すか（末尾の案内が変わる）
	};

	// **内訳の 1 行表現**（`ストーリ:3/3,通り芯:44/44,…`）。命令が 0 の要素は載せない
	// （無い物の 0 を並べても差分の役に立たない）。次の周まで持ち越して差分を取るための
	// 形なので、**人向けの整形は一切しない**。
	std::string formatTally(const std::vector<ElementRow>& rows);

	// 2 つの内訳を突き合わせ、**変わった行だけ**を人が読める形で返す（変化が無ければ空）。
	// 片方にしか無い要素も「増えた／消えた」として出す——要素が丸ごと出なくなるのは
	// たいてい退行なので、黙って落とすと最悪の変化を見落とす。
	std::string formatTallyDiff(const std::string& previous, const std::string& current);

	// **同じ入力なら毎回同じになる仮名**（`model-8f3a12.ifc`）。周回どうしで対象が
	// 同じであることを読む側が確かめられるよう、ランダムにはしない（ファイル名の
	// 拡張子を除いた部分のハッシュ）。
	std::string anonymizedFileName(const std::string& path);

	// 本文から案件・個人が分かるものを伏せる。伏せるのは (1) 与えられた IFC のパスと
	// ファイル名、(2) ホームディレクトリのユーザー名（`/Users/<名前>` `C:\Users\<名前>`）。
	// **それ以外は触らない**——診断の中身まで削ると、伝えるべきものが伝わらない。
	std::string redactText(const std::string& text, const std::string& ifcPath);

	// **PR コメント本文**（Markdown）。先頭に機械可読の目印を置く——このコメントが
	// プラグインの自動投稿であること、何周目か、どのビルドかを、読む側（Claude）が
	// 本文の見た目に依らず拾えるようにするため。
	std::string formatFeedbackComment(const FeedbackRound& round, const core::Document& document,
									  const core::DrawCounts& counts);

	// コメント 1 通の上限（バイト）。GitHub の 65536 文字より十分低く取ってあり、
	// 超える分は**診断ログの古いほうから**削る（末尾＝結果に近いほうを残す）。
	inline constexpr std::size_t kMaxFeedbackCommentBytes = 60000;
} // namespace HomeskzIfcImport::parse
