//
//	core/FeedbackSession.h
//
//	**実機テストが周をまたいで持ち越すもの**（docs/DEV-NOTES.md M23 / M25 / M38〜M40）。
//	持ち越すのは**実機テストが自分で保存した図面のパス**だけで、次の周の頭でそれを閉じる
//	（draw/Feedback の CloseOwnedDocuments）。
//
//	【条件は持ち越さない（M43）】どの IFC を・どのテンプレートから・どの設定で取り込むかは、
//	**毎周、頼んだ側が渡す**。MCP の `vw_run_test` に Claude が ifc / template /
//	settings を毎回渡す。M42 までは 1 周目の条件と前の周の内訳・基準の
//	レイヤをここへ記憶し、2 周目以降を名指し無しで走らせていたが、周を起こすのがローカルの
//	Claude Code になり、条件も前の周の報告も Claude のセッションが持っているので要らなく
//	なった（記憶と頼んだ条件が食い違う余地も無くなる）。
//
//	【自分の図面だけは持ち越す】周の終わりに保存した描画結果は、次の周の頭で**保存せずに
//	閉じる**。その相手を「いま開いている図面のうち、置き場の中にあるもの」で選ぶと、
//	利用者が置き場から開いた図面まで閉じうる。閉じてよいのは**名指しで記録したもの**に
//	限る（CLAUDE.md「開発の基本方針」8）。本体は周の間に入れ替わる（`vw_update`）ので、
//	記録はメモリではなくファイルに置く。
//
//	【書式】**key=value を 1 行 1 つ**。JSON にしないのは、読み書きするのがこのファイルと
//	単体テストだけで、値がすべて平たいから。値に改行は入れない（入っていたら捨てる）。
//	M42 までの記憶にある条件の行（ifc / round / template / role.* など）は、知らない行として
//	通知せずに読み飛ばす。
//
//	【SDK 非依存】core/ は VectorWorks SDK を include しない。ファイルの読み書きも
//	標準ライブラリだけで完結するので、無 SDK で単体テストできる（core/Trace と同じ）。
//

#pragma once

#include "core/ImportOptions.h"
#include "core/Json.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 実機テストが周をまたいで持ち越すもの。既定値は「閉じる相手が無い」。
	struct FeedbackSession
	{
		// **実機テストが自分で保存した図面のパス**（テンプレートと、各周の描画結果）。
		//
		// 次の周の頭で、この中で**いま開いているものを保存せずに閉じる**（draw/Feedback の
		// CloseOwnedDocuments）。各周の描画結果は周の終わりに一時ファイルへ保存してある
		// ので、人が手を入れていなければ未保存の変更は無く、Vectorworks を再起動しても
		// 保存の確認が出ない。**閉じてよいのはここに名指しで在り、かつ一時ファイルの置き場
		// （core/FeedbackScratch）の中にあるものだけ**——利用者の図面を閉じない安全弁で、
		// `CloseDocument()` は確認なしに変更を捨てる（SDK リファレンス Findings「Documents」）
		// ので緩めない。
		std::vector<std::string> ownedDocuments;
	};

	// -----------------------------------------------------------------------
	// **いま開いている図面 openPath を、実機テストが保存せずに閉じてよいか**（M39 の安全弁）。
	//
	// `CloseDocument()` は確認なしに未保存の変更を捨てる（SDK リファレンス Findings
	// 「Documents」）ので、閉じる相手は次の 2 つを両方満たすものに限る:
	//   * 記録（ownedDocuments）に**名指しで在る**——実機テストが自分で保存した図面である。
	//   * その記録のパスが**一時ファイルの置き場（scratchRoot）の中**にある——記録のファイルが
	//     壊れていたり手で書き換えられていたりしても、利用者の図面へは届かない。
	// 同じファイルかどうかは字面だけでなく std::filesystem にも尋ねる（macOS の一時
	// ディレクトリは /var と /private/var の 2 通りの綴りで返ってくる）。
	bool isOwnedTestDocument(const FeedbackSession& session, const std::string& openPath,
							 const std::string& scratchRoot);

	// -----------------------------------------------------------------------
	// **MCP の `vw_run_test` に渡された settings を取り込み設定へ当てる**（M43）。
	//
	// options には先に既定（テンプレートから開いた図面にあるもので組んだもの。
	// draw::presetImportSettings）を入れておき、settings に書かれた項目だけを上書きする。
	// 書けるのは設定ダイアログで決められるものすべて:
	//   symbols       … { "<役割の表示名>": "<シンボル名>" | true | false }
	//                   （core::symbolRoleLabel。文字列はそのシンボルで取り込む・空文字列と
	//                   false は取り込まない・true は既定のシンボルのまま取り込む）
	//   title_block   … 図面枠のスタイル名（空＝置かない）
	//   dimension     … 寸法規格の名前（空＝入れない）
	//   merge_levels  … 前のレベルと同じ伏図にまとめる高さ（"<階の番号>:<高さ mm>" の配列。
	//                   core::PlanLevelKey）
	//   skip_sections … 軸組図から除外する通りの図番の配列
	//   rafter        … 垂木の断面 { "width": mm, "height": mm }
	//
	// **知らない項目・型の違う値・範囲外の値は通知せずに読み飛ばさず、false と理由を返す**
	// ——頼んだ条件と違う条件で黙って走った周の報告は、読む側を誤らせる。settings が null
	// （渡されなかった）なら何もせず true。失敗したとき options は途中まで書き換わっている
	// ことがあるので、呼び出し側は周を始めずに理由を返す。
	bool applyTestSettings(const Json& settings, ImportOptions& options, std::string& error);

	// -----------------------------------------------------------------------
	// 記録を key=value テキストへ（末尾は改行）。
	std::string formatFeedbackSession(const FeedbackSession& session);

	// key=value テキストから記録を復元する。**知らない行・壊れた行は通知せずに読み飛ばす**
	// （M42 までの記憶にある条件の行もここで落ちる）。
	FeedbackSession parseFeedbackSession(const std::string& text);

	// 記録の置き場所。**一時ディレクトリには置かない**（再起動をまたいで閉じる相手を
	// 失わないため）:
	//   macOS   … $HOME/Library/Application Support/HomeskzIfcImport/feedback.txt
	//   Windows … %LOCALAPPDATA%\HomeskzIfcImport\feedback.txt
	// どちらの環境変数も取れなければ空を返す（呼び出し側は記録を使わない）。
	// **環境変数 HOMESKZ_IFC_FEEDBACK_STATE が指定されていればそれを優先する**（試験用）。
	std::string defaultFeedbackSessionPath();

	// 読み書き。読めなければ false（＝閉じる相手が無い）。書けなければ false
	// （次の周で前の周の図面を閉じられないだけなので、呼び出し側は結末に添える）。
	bool readFeedbackSession(const std::string& path, FeedbackSession& out);
	bool writeFeedbackSession(const std::string& path, const FeedbackSession& session);

	// 記録を消す。無ければ何もしない。
	void clearFeedbackSession(const std::string& path);

	// **直近の実機テストの報告**（Markdown。parse::formatTestRoundReport）の置き場所。
	// 記録と同じフォルダの last-round.md（記録のパスが空なら空）。MCP の `vw_test_report`
	// がここを読む——本体を入れ替えても読めるよう、メモリではなくファイルに置く。
	std::string testReportPathFor(const std::string& sessionPath);
} // namespace HomeskzIfcImport::core
