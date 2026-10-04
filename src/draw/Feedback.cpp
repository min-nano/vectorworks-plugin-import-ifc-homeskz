//
//	draw/Feedback.cpp
//
//	実機テストの 1 周の実装（意図と入口は draw/Feedback.h 参照）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）と VWFC のダイアログを include する。
//
//	**ネットワークには一切触れない**（M38）。結果は手元のファイルへ書き、ローカルの
//	Claude Code が MCP ブリッジ越しに読む（draw/McpBridge.cpp の `vw_test_report`）。
//
//	【尋ねるのは取り込みの前だけ】ダイアログを出してよいのは取り込みが始まる前だけで、
//	終わったあとは**何も出さない**（失敗したときを除く）。取り込みは 1 分以上かかるので、
//	終わったところに確認が待っていると席を離れられない（docs/DEV-NOTES.md M23
//	「取り込みのあとに操作を残さない」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/Feedback.h"

#include "core/Document.h"
#include "core/FeedbackScratch.h"
#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"
#include "draw/DrawUtil.h"
#include "draw/HostServices.h"
#include "draw/ImportRun.h"
#include "draw/ResultDialog.h"
#include "draw/SectionPickDialog.h"
#include "draw/SettingsDialog.h"
#include "parse/BuildDocument.h"
#include "parse/Feedback.h"
#include "parse/Summary.h"

// 周ごとに図面を開き直す（ISDK::OpenDocumentPath）。パスは IFileIdentifier で渡す。
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// **実機テストの結果ダイアログのタイトル。** 本番の取り込み（「ホームズ君 IFC
		// 取り込み」。draw/ImportCommand.cpp）と**必ず違う名前にする**——同じにすると、
		// 実機テストの顛末を本番の取り込みが言っているように見える（実機の指摘。M25）。
		constexpr const char* kTestResultTitle = "実機テスト (みんなの構造設計支援Dev)";

		// **周ごとに、まっさらな作業ファイルを開き直す。**
		//
		// レイヤ削除（下の prepareDrawingForRound）は「前の周が自分で作ったレイヤ」しか
		// 消せず、**取り込み前から在ったレイヤ**——実機のテンプレートにある通り芯の
		// 「共通」——へ描いた分は残る。実機で絵が二重になり、伏図・軸組図の「収まらない」
		// 枚数が増えた（docs/DEV-NOTES.md M25）。丸ごと戻す道は**4 本とも塞がっている**
		// （SDK リファレンス #23 / #27 / #31 / #39。スクリプトエンジン経由の間接起動も
		// 実機で「1 段も効かない」と確定した）。
		//
		// **やり方は「別名で保存して開き直す」。** 1 周目に、そのとき開いている図面を作業
		// ファイルへ**別名保存**する——以後これが基準になる。次の周の頭では、前の周の絵が
		// 載った文書を**別の捨て場所へ保存し直してから閉じ**、基準を開き直す。基準の
		// ファイルには前の周の絵が 1 つも書き戻らないので、「保存して閉じる」がそのまま
		// 「変更を破棄する」になる——**未保存の変更がある文書は `CloseDocument()` が
		// false で閉じられない**（実機確認済み）という制約への答えでもある。
		//
		// **テンプレートを複製しない。** 開いたテンプレートは普通の図面と同じものなので、
		// いま開いている図面を基準にすればよい（利用者の指摘。M25）。ファイル選択も
		// 拡張子の見分けも要らなくなる。
		//
		// 使う口は `ISDK::SaveActiveDocumentPath` / `CloseDocument` / `OpenDocumentPath` /
		// `GetActiveDocument`（[SDK リファレンス「Documents」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Documents.md)。
		// コマンド実行中に呼べること・カレント文書が即座に移ることは実機確認済み）。
		//
		// **ここは実機テストの周だけ。** 本番の取り込みは開いている図面へ描くのが仕事で、
		// この関数を呼ばない（draw/Feedback.h・CLAUDE.md M25）。

		// 絶対パスから IFileIdentifier を作る（作れなければ空の VCOMPtr）。
		VectorWorks::Filing::IFileIdentifierPtr FileIdFor(const std::string& path)
		{
			using namespace VectorWorks::Filing;
			// const で受ける（VCOMPtr の operator-> は const。draw/ImportRun.cpp と同じ
			// 作法で、clang-tidy の misc-const-correctness もこれを求める）。
			const IFileIdentifierPtr fileID(IID_FileIdentifier);
			if (!fileID)
				return IFileIdentifierPtr{};
			if (fileID->Set(TXString(path.c_str())) != kVCOMError_NoError)
				return IFileIdentifierPtr{};
			return fileID;
		}

		// いまアクティブな図面のパス（取得できなければ空）。**開けたかどうかは
		// `OpenDocumentPath` の戻り値ではなくこれで判定する**（読み戻して確かめる。
		// SDK リファレンス「Investigation Techniques」）。
		//
		// **無題の図面でもパスは返る**——「アプリケーションのあるディレクトリ＋名称未設定 N」
		// が入る（実機確認済み。SDK リファレンス「Documents」）。保存済みかどうかを見たい
		// ときはパスの有無ではなく `outSaved` を使うこと。
		std::string ActiveDocumentPath()
		{
			VectorWorks::Filing::IFileIdentifierPtr fileID;
			bool saved = false;
			if (!gSDK->GetActiveDocument(&fileID, saved) || !fileID)
				return "";
			TXString path;
			if (fileID->GetFileFullPath(path) != kVCOMError_NoError)
				return "";
			return static_cast<const char*>(path);
		}

		// 同じファイルを指しているか。**大文字小文字や区切りの差で外さない**よう
		// std::filesystem に正規化させ、それが効かない場面（まだ無いファイルなど）は
		// 素の比較に落とす。
		bool SamePath(const std::string& left, const std::string& right)
		{
			if (left.empty() || right.empty())
				return false;
			if (left == right)
				return true;
			std::error_code ec;
			return std::filesystem::equivalent(std::filesystem::path(left),
											   std::filesystem::path(right), ec) &&
				   !ec;
		}

		// 一時ディレクトリの中のパスを組む（temp が引けなければ空）。
		std::string TempPath(const std::string& name)
		{
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
			if (ec)
				return "";
			return (dir / name).string();
		}

		// そのパスに何か在るか。**`std::filesystem` で見に行かない**——`file_size` の
		// stat が MSVC の clang-analyzer に偽陽性を出した前例があるので、開けるかどうかで
		// 判じる（在るのに開けないなら、どのみち上書きも当てにできない）。
		bool PathExists(const std::string& path)
		{
			if (path.empty())
				return false;
			const std::ifstream in(path, std::ios::binary);
			return in.good();
		}

		// **まだ無いパスを選ぶ。** 既にあるファイルへ別名保存できなかった実測がある
		// （実機 round 12。round 8 は同じ呼び出しが新規のパスで通っている）ので、
		// 上書きが効くことに賭けない。名前が尽きたら空を返す。
		//
		// **置き場はブランチごとのフォルダ**（core/FeedbackScratch.h）。PR が閉じたら
		// フォルダごと片付けられるように、いま動いているビルドのブランチの下へ置く
		// （M38 までは一時ディレクトリの直下へ置いたきりで、PR #188 の実機確認で
		// 3.4 GB を超えて溜まっていた）。
		std::string FreshTempPath(const std::string& stem)
		{
			const std::string root = TempPath(core::kScratchRootName);
			const std::string dir = core::prepareBranchScratch(root, currentBuildInfo().branch);
			if (dir.empty())
				return "";
			for (int i = 1; i <= 100; ++i)
			{
				// **const にしない**——返すときに move されなくなり、clang-tidy の
				// performance-no-automatic-move がエラーになる（tidy-mac で実際に落ちた）。
				std::string path =
					(std::filesystem::path(dir) / (stem + "-" + std::to_string(i) + ".vwx"))
						.string();
				if (!PathExists(path))
					return path;
			}
			return "";
		}

		// **PR が閉じたブランチの一時ファイルを片付ける**（core/FeedbackScratch.h。利用者の
		// ご要望で、PR が close／merge されたらその実機テストの図面は消す）。周の頭で
		// 1 度だけ呼ぶ。戻り値は報告と診断ログへ出す 1 行（何もしなければ空）。
		//
		// いま動いているブランチのフォルダは尋ねもしない（使っている最中）。ほかの
		// ブランチが 1 つも無ければ GitHub へも行かない——毎周の問い合わせで API の上限を
		// 削らないため。PR の状態は同梱スクリプト（vw-update の q-pr-state。トークンは
		// vw-token が持つ）に尋ねる。**分からなければ消さない。**
		//
		// 消したフォルダに記憶の作業ファイルがあったら、記憶から外す（無いファイルを
		// 開き直しに行かせない）。次の周は 1 周目と同じく、いま開いている図面を基準に採る。
		std::string CleanUpClosedBranches(core::FeedbackSession& session, const std::string& branch)
		{
			const std::string root = TempPath(core::kScratchRootName);
			if (root.empty())
				return "";
			std::vector<core::ScratchDir> others;
			for (const core::ScratchDir& dir : core::listScratchDirs(root))
			{
				if (!dir.branch.empty() && dir.branch != branch)
					others.push_back(dir);
			}
			if (others.empty())
				return "";

			const HostServices& host = hostServices();
			std::vector<std::string> args{"q-pr-state"};
			for (const core::ScratchDir& dir : others)
				args.push_back(dir.branch);
			std::string out;
			if (!host.canRunScripts() || !host.runScript("vw-update", args, out))
				return "一時ファイル: PR の状態を尋ねるスクリプトを走らせられなかったので、"
					   "ほかのブランチの作業ファイルは片付けませんでした";

			const core::ScratchCleanup cleanup =
				core::cleanUpClosedBranches(root, others, core::parsePrStates(out));
			for (const std::string& removed : cleanup.removedPaths)
			{
				if (core::pathIsInside(session.workPath, removed))
					session.workPath.clear();
			}
			return core::describeScratchCleanup(cleanup);
		}

		// アクティブな図面を指定のパスへ保存する（＝別名保存）。成功したら true。
		bool SaveActiveDocumentAs(const std::string& path)
		{
			if (path.empty())
				return false;
			const VectorWorks::Filing::IFileIdentifierPtr fileID = FileIdFor(path);
			return fileID && gSDK->SaveActiveDocumentPath(fileID) == 0;
		}

		// 作業ファイルを用意できたか。
		enum class RoundDocument
		{
			// 作業ファイルが開いている（＝取り除くものは何も無い）。
			Ready,
			// 用意できなかったが、いまの図面へは描ける（レイヤ削除へ回す）。
			Fallback,
			// **描く先が無い。** 取り込みを始めてはいけない。
			Abort,
		};

		// 前の周が作ったレイヤを取り除く（実体は下。作業ファイルを採るときにも使うので、
		// ここで名前だけ先に出す）。
		std::string prepareDrawingForRound(const core::FeedbackSession& session);

		// **開き直す前に「取り消し」を試す。** 取り込みは自分で undo イベントを開き、作った
		// レイヤを登録している（draw/DrawUtil の ImportUndoScope）ので、**利用者に頼んで
		// いた「取り消し」と同じもの**をここから起こせる（SDK リファレンス Findings「Undo」/
		// issue #39。VectorScript 経由の `DoMenuTextByName('Undo', 0)` は実機で効くと確定
		// した）。効けば開き直しが要らないだけでなく、**取り込み前から在ったレイヤ
		// （テンプレートの「共通」等）へ描いた分まで戻る**——レイヤ削除では決して届かない
		// 範囲で、実機で絵が二重になった原因そのものである。
		//
		// **押してよい場面を 2 つの条件で絞る**（どちらも満たすときだけ）:
		//   * いま開いているのが**自分の作業ファイル**であること。利用者の図面では絶対に
		//     押さない——取り消しスタックの上に載っているのは利用者自身の編集かもしれない。
		//   * **前の周が作ったレイヤが実際に残っている**こと。＝上に載っているのは自分の
		//     取り込みだ、という根拠になる。
		//
		// 効いたかは**読み戻して**確かめる（前の周のレイヤが 1 枚も無くなったか）。1 回の
		// 取り込みが undo イベントを 1 つだけ積むとは限らないので、消えるまで数段掛け、
		// それでも消えなければ諦めて開き直しへ回す（作業ファイルの取り消しスタックに
		// 載っているのは自分の取り込みだけなので、数段戻しても利用者のものには届かない）。
		bool undoPreviousRound(const core::FeedbackSession& session, std::string& note)
		{
			note.clear();
			// 何段まで掛けるか。1 周ぶんを戻すのに要る段数は実機でしか分からないので、
			// 「消えるまで」を上限つきで回す（上限に意味は無く、暴走を止めるためだけ）。
			constexpr int kMaxUndoSteps = 8;

			if (session.workPath.empty() || session.lastCreatedLayers.empty())
				return false;
			if (!SamePath(ActiveDocumentPath(), session.workPath))
				return false; // 利用者が別の図面へ移っている。触らない
			if (!draw::AnyLayerRemains(session.lastCreatedLayers, session.lastCreatedSheets))
				return false; // 前の周の絵が無い＝取り消しスタックの上は自分のものではない

			for (int step = 1; step <= kMaxUndoSteps; ++step)
			{
				if (!draw::UndoOneStep())
				{
					note =
						"取り消しを掛けられませんでした（" + std::to_string(step - 1) + " 段まで）";
					return false;
				}
				if (!draw::AnyLayerRemains(session.lastCreatedLayers, session.lastCreatedSheets))
				{
					note = "前の周の取り込みを取り消しました（" + std::to_string(step) +
						   " 段）。取り込み前から在ったレイヤへ描いた分も戻っています";
					return true;
				}
			}
			note = "取り消しを " + std::to_string(kMaxUndoSteps) +
				   " 段掛けても前の周のレイヤが残りました";
			return false;
		}

		// 作業ファイルを開き直す。note には診断ログと報告へ出す 1 行が入る。
		// rebased は「基準を採り直した」ときにその理由（runTestRound が作る）。
		RoundDocument openRoundDocument(core::FeedbackSession& session, const std::string& rebased,
										std::string& note)
		{
			note.clear();
			if (session.workPath.empty())
			{
				// **1 周目: いま開いている図面を作業ファイルとして別名保存する。**
				// 元のファイルには何も書き戻らない（別名保存なので、以後の変更は作業
				// ファイルの側に付く）。
				// **採る前に、前の周が作ったレイヤを落とす。** 前の周が作業ファイルを
				// 用意できずに図面へ直接描いていると、**その絵が載ったまま基準になり、
				// 以後の周がずっと汚れた状態から始まる**（実機 round 13。round 12 の
				// 失敗がそのまま基準へ焼き付いた）。ここで落としておけば、採り直した
				// ときに自分で直る。
				//
				// **採り直した周（rebased）では落とさない。** 人が別の図面へ移ってから
				// 押したのなら、いま開いているのは前の周が描いた図面ではない——そこで
				// 前の周のレイヤ名（「1-FL」等）を名指しで消すと、**同じ名前を持つ利用者の
				// レイヤ**（本番で取り込んだ図面など）まで消える（CLAUDE.md「開発の基本
				// 方針」8。PR #188 の実機確認で、空の図面に対して 0/34 枚の取り除きが
				// 走っていた）。round 13 の焼き付きは作業ファイルが無い周の話で、そのとき
				// workPath は空なので rebased は立たない。
				std::string cleaned;
				if (rebased.empty() &&
					(!session.lastCreatedLayers.empty() || !session.lastCreatedSheets.empty()))
					cleaned = prepareDrawingForRound(session) + "。";

				// **レイヤの基準も採り直す。** 別の図面で採った顔ぶれと引き比べても意味が
				// 無い（「基準に無いレイヤ」が出るだけで、読む側を惑わせる）。次の報告が
				// この図面の顔ぶれを基準として採り直す。
				session.baselineRecorded = false;
				session.baselineLayers.clear();

				const std::string prefix = cleaned + rebased;
				const std::string work = FreshTempPath("work");
				if (!SaveActiveDocumentAs(work))
				{
					// **証拠を残す**——ここが空振りすると、以後の周はぜんぶレイヤ削除の
					// 控えで走る（実機 round 12）。何を保存しようとして駄目だったのかが
					// 分からないと、次の周も同じところで止まる。
					const std::string openPath = ActiveDocumentPath();
					note = prefix + "作業ファイルを用意できませんでした（" +
						   (work.empty() ? std::string("一時ディレクトリが引けません") : work) +
						   " / いまの図面は " +
						   (openPath.empty() ? std::string("(取得できず)") : openPath) +
						   "）。いま開いている図面へ描きます";
					return RoundDocument::Fallback;
				}
				session.workPath = work;
				note = prefix + "いま開いている図面を作業ファイルとして保存しました（" + work +
					   "）。次の周からはここを開き直して、毎回この状態から始めます";
				return RoundDocument::Ready;
			}

			// **まず取り消しを試す**（上の undoPreviousRound）。効けば図面はもう取り込み前へ
			// 戻っているので、開き直しもレイヤ削除も要らない。
			std::string undone;
			if (undoPreviousRound(session, undone))
			{
				note = undone;
				return RoundDocument::Ready;
			}

			// **2 周目以降: 前の周の絵が載った文書を捨て場所へ移してから閉じる。**
			// 基準（作業ファイル）には前の周の絵が書き戻らない＝変更を破棄したのと同じ。
			// 取り消しを試して駄目だったなら、その顛末も一緒に持っていく（次の周の読み手が
			// 「なぜ開き直したのか」を追えるように）。
			const std::string undoNote = undone.empty() ? std::string() : undone + "。";
			std::string closed;
			const std::string active = ActiveDocumentPath();
			if (SamePath(active, session.workPath))
			{
				const std::string parked = FreshTempPath("round-" + std::to_string(session.round));
				if (!SaveActiveDocumentAs(parked))
				{
					// 退避できないなら閉じない（未保存の文書は閉じられない。実機確認済み）。
					// いまの図面はまだ生きているので、従来のレイヤ削除へ回せる。
					note = "前の周の図面を退避できなかったので開き直しませんでした（" + parked +
						   "）。いま開いている図面へ描きます";
					return RoundDocument::Fallback;
				}
				// **`CloseDocument` の戻り値で分岐しない。** false を返しても実際には
				// 閉じていることがある——実機 round 9 で false を見て「閉じられなかった
				// から今の図面へ描く」と決めた結果、**どこにも属さない状態で描いて全 18
				// 要素が 0 件**になった。閉じられたかどうかは下の読み戻しで判る。
				const bool closeReturned = gSDK->CloseDocument();
				closed = "前の周の図面は " + parked + " へ移しました（閉じる=";
				closed += closeReturned ? "true" : "false";
				closed += "）";
			}
			else
			{
				// 人が別の図面へ移ったあと。**その図面には触らない**（閉じない）。
				closed = "前の周の図面はアクティブではありませんでした（開いたまま残します）";
			}

			const VectorWorks::Filing::IFileIdentifierPtr fileID = FileIdFor(session.workPath);
			// bShowErrorMessages=false: 誰も見ていない周でダイアログを出さない。
			const bool returned = fileID && gSDK->OpenDocumentPath(fileID, false);
			// **戻り値だけを信じない**——開いたかどうかはカレント文書を読み戻して確かめる。
			const std::string opened = ActiveDocumentPath();
			if (SamePath(opened, session.workPath))
			{
				note = undoNote + "作業ファイルを開き直しました（" + session.workPath + "）。";
				note += closed;
				// **作業ファイルそのものに前の周の絵が焼き付いていることがある**（実機
				// round 13。作業ファイルを用意できなかった周の絵が載ったまま基準として
				// 採られた）。作業ファイルは採ったときの中身のまま変わらないので、放って
				// おくと以後の周がずっと汚れた状態から始まる。ここで名指しの取り除きを
				// 通しておけば**自分で直る**——きれいな作業ファイルには 1 枚も無いので、
				// そのときは素通りする。
				if (!session.lastCreatedLayers.empty() || !session.lastCreatedSheets.empty())
					note += "。" + prepareDrawingForRound(session);
				return RoundDocument::Ready;
			}

			// **ここから先へ進まない。** 閉じたつもりの文書が本当に閉じているなら、いま
			// 描く先はどこにも無い——描けば全要素 0 件の報告が出るだけで、直すべき場所を
			// 指さない数字が報告に残る（実機 round 9）。**何が起きたかを証拠つきで残して
			// 周ごと中止する。**
			std::string why = undoNote;
			why += "作業ファイルを開き直せなかったので、この周は走らせません";
			why += "でした（" + session.workPath + " / OpenDocumentPath=";
			why += returned ? "true" : "false";
			why += " / いまは " + (opened.empty() ? std::string("(取得できず)") : opened);
			why += "）。" + closed;
			note = why;
			return RoundDocument::Abort;
		}

		// **周と周のあいだに図面を取り込み前へ戻す。** 戻り値は診断ログへ書く 1 行。
		//
		// **前の周が作ったレイヤを、名指しで取り除く。** プログラムから「取り消し」を掛ける
		// 手立ては無い——ISDK の undo 実行 API は閉じたイベントに効かず（SDK リファレンス
		// issue #23）、メニューコマンドを名前で起動する API も存在しない（同 #27。どちらも
		// 「できない」でヘッダ根拠つきに確定済み）。残る唯一の道が**レイヤのハンドルを直接
		// 消す**もので、これは実機で確認されている（同 #25。Findings「Undo」）。SDK の作法
		// （自分で undo イベントを開いて閉じる・シートを先に消す）と安全弁は
		// draw/DrawUtil の RemoveCreatedLayers が 1 か所で持つ。
		//
		// **消してよいのは「前の周が自分で作ったレイヤ」だけ。** 記憶に名前で控えてある
		// ものに限る——「基準に無いレイヤ」を消す作りにすると、利用者が別の用途で足した
		// レイヤまで巻き込む（core/FeedbackSession の lastCreatedLayers）。
		std::string prepareDrawingForRound(const core::FeedbackSession& session)
		{
			if (session.lastCreatedLayers.empty() && session.lastCreatedSheets.empty())
				return "前の周が作ったレイヤの記録が無いので、図面はそのままにしました"
					   "（残っていれば、その上へ重ねて描きます）";
			std::string note;
			const std::size_t removed =
				RemoveCreatedLayers(session.lastCreatedLayers, session.lastCreatedSheets, note);
			if (removed == 0 && note.empty())
				return "前の周が作ったレイヤは 1 枚も残っていませんでした（図面はそのまま）";
			// **これは部分的な復元でしかない。** 取り込み前から在ったレイヤ（テンプレートの
			// もの）へ描いた分は、そのレイヤが自分の作ったものではないので取り除けない
			// ——上に描いた分だけが残る。**丸ごと戻す道は 3 つとも塞がっている**（SDK
			// リファレンス Findings「Undo」）: 閉じたイベントへ Undo は掛けられず（#23）、
			// 閉じずに返しても VW がコマンド完了時に代わりに閉じてしまい（#31）、メニューの
			// 「取り消し」を名前で起動する API も無い（#27）。ここを読む人が「戻り切った」と
			// 思わないよう、1 行で言い切っておく。
			return note + "。取り込み前から在ったレイヤへ描いた分は取り除けません";
		}

		// **メニューから押した続きの周で、条件をどうするか。**
		enum class Conditions
		{
			Same,	  // 前回と同じ IFC・設定で
			Rechoose, // IFC と設定を選び直す（＝新しい 1 周目）
			Cancel,	  // やめる
		};

		// **1 度だけ尋ねる。** M37 までは続きの周を往復のパレットが回していたので、人が
		// 押す周も「何も尋ねない」に揃えていた。いまは続きの周を無人で回すのは MCP の
		// `vw_run_test` で、メニューを押すのは人だけ——その人が別の IFC や設定で試したいとき、
		// 記憶のファイルを消す以外の手段が無いのでは困る。
		Conditions AskConditions(const core::FeedbackSession& session)
		{
			// ファイル名は文字列のまま切り出す（std::filesystem::path は Windows で UTF-8 を
			// ANSI として読み、日本語の名前が化ける）。
			const std::string::size_type slash = session.ifcPath.find_last_of("/\\");
			const std::string ifc =
				slash == std::string::npos ? session.ifcPath : session.ifcPath.substr(slash + 1);
			const std::string advice = "前回（round " + std::to_string(session.round) +
									   "）の IFC: " + ifc +
									   "\n図面は取り込み前へ戻してから描き直します。";
			// AlertQuestion は 0 = 取り消し、1 = OK、2 / 3 = 追加のボタン A / B を返す
			// （src/Updater.cpp の Ask と同じ作法）。
			const short answer = gSDK->AlertQuestion(
				"前回と同じ条件で実機テストを実行しますか？", advice.c_str(),
				/*defaultButton*/ 1, "同じ条件で", "やめる", /*customButtonA*/ "選び直す",
				/*customButtonB*/ "");
			if (answer == 1)
				return Conditions::Same;
			if (answer == 2)
				return Conditions::Rechoose;
			return Conditions::Cancel;
		}

		// **1 周目の選択**（IFC → 取り込み設定 → 軸組図の通り。本番の取り込みと同じ順）。
		// どれかで取り消されたら false。
		bool ChooseConditions(std::string& ifcPath, core::ImportOptions& options,
							  bool& settingsShown, std::string& settingsNote)
		{
			if (!chooseIfcFile(ifcPath))
				return false;
			options = core::ImportOptions{};
			// 伏図のまとめ方の候補は取り込みの前に IFC を読んで集める（draw/ImportCommand と
			// 同じ。draw/SettingsDialog.h）。
			const draw::SettingsOutcome settings = draw::showImportSettings(
				options, parse::scanPlanLevelChoices(ifcPath), &settingsNote);
			if (settings == draw::SettingsOutcome::Cancelled)
				return false;
			settingsShown = settings == draw::SettingsOutcome::Accepted;
			// M34 軸組図にする通りも**1 周目で**尋ね切る。選んだ結果は設定と一緒に記憶へ
			// 入り、続きの周はそれを使う。
			std::string pickNote;
			const draw::SettingsOutcome pick = draw::showSectionPicker(
				parse::buildSectionCandidates(ifcPath, options), options, &pickNote);
			if (pick == draw::SettingsOutcome::Cancelled)
				return false;
			if (!pickNote.empty())
				settingsNote += (settingsNote.empty() ? "" : " / ") + pickNote;
			return true;
		}

		// 報告を書く（置き場所の親フォルダは記憶を書くときに用意済み）。書けたら true。
		bool WriteReport(const std::string& path, const std::string& report)
		{
			if (path.empty())
				return false;
			std::error_code ec;
			const std::filesystem::path file(path);
			if (file.has_parent_path())
				std::filesystem::create_directories(file.parent_path(), ec);
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
				return false;
			out.write(report.data(), static_cast<std::streamsize>(report.size()));
			return out.good();
		}

		// 失敗の結末を作る（メニューから押した周なら結果ダイアログでも伝える）。
		TestRoundResult Failure(bool allowDialogs, parse::TestRoundOutcome outcome,
								const std::string& detail)
		{
			TestRoundResult result;
			result.message = parse::formatTestRoundResult(outcome, detail);
			if (allowDialogs)
				(void)draw::showImportResult(kTestResultTitle, result.message, core::trace::text());
			return result;
		}
	} // namespace

	// -----------------------------------------------------------------------
	bool feedbackAvailable()
	{
#ifdef VW_DEV_BUILD
		return true;
#else
		// **安定版では動かさない。** 開発の道具（記憶した条件での無人の取り込み）を
		// 利用者向けの配布物に持たせない。
		return false;
#endif
	}

	std::string testReportPath()
	{
		return core::testReportPathFor(core::defaultFeedbackSessionPath());
	}

	// -----------------------------------------------------------------------
	// **実機テストの 1 周**（M25 / M38。draw/Feedback.h）。
	TestRoundResult runTestRound(bool allowDialogs)
	{
		if (!feedbackAvailable())
		{
			// 押した人には言う——黙って何も起きないと「壊れている」と読まれる。
			TestRoundResult result;
			result.message = "実機テストは開発版（Dev）のビルドでのみ使えます。";
			if (allowDialogs)
				gSDK->AlertInform("実機テストは開発版でのみ使えます。",
								  "開発版（Dev）のビルドで動きます。", false);
			return result;
		}

		const parse::BuildInfo build = currentBuildInfo();
		const std::string sessionPath = core::defaultFeedbackSessionPath();
		core::FeedbackSession session;
		(void)core::readFeedbackSession(sessionPath, session); // 読めなければ 1 周目

		// **片付けは基準の採り直しより先に。** 記憶の作業ファイルを消したなら、下の判断は
		// それが無いものとして進む。
		const std::string scratchNote = CleanUpClosedBranches(session, build.branch);

		// **手動で押したときは、いま開いている図面を基準として採り直す。** 人が別の図面
		// （空のテンプレート等）を開いてからメニューを押したのは「この図面で試したい」
		// という意思なのに、覚えた作業ファイルを開き直すとその意思が黙って消える——実機で
		// 二度、空のテンプレートで試そうとして前の周の作業ファイルに上書きされた。**記憶
		// ごと消さない**（IFC・設定はそのまま）——捨てるのは「どの図面から始めるか」だけ。
		// MCP の周はここへ来ない: そちらは前の周の続きなので、作業ファイルへ戻すのが正しい。
		std::string rebased;
		if (allowDialogs && !session.workPath.empty())
		{
			const std::string openPath = ActiveDocumentPath();
			if (!SamePath(openPath, session.workPath))
			{
				rebased = "いま開いている図面（" +
						  (openPath.empty() ? std::string("(取得できず)") : openPath) +
						  "）は前の周の作業ファイル（" + session.workPath +
						  "）ではないので、こちらを新しい基準として採り直しました。";
				session.workPath.clear();
			}
		}

		// **どの周になるかは無 SDK 側が決める**（core/FeedbackSession.h）。
		const core::FeedbackRoundKind kind = core::feedbackRoundKind(session, allowDialogs);
		if (kind == core::FeedbackRoundKind::Refuse)
			return Failure(allowDialogs, parse::TestRoundOutcome::NotRemembered, {});

		bool choose = kind == core::FeedbackRoundKind::FirstRound;
		if (!choose && allowDialogs)
		{
			const Conditions conditions = AskConditions(session);
			if (conditions == Conditions::Cancel)
				return TestRoundResult{}; // やめた人に結末を重ねない
			if (conditions == Conditions::Rechoose)
				choose = true;
		}

		std::string ifcPath = session.ifcPath;
		core::ImportOptions options = session.options;
		bool settingsShown = true;
		std::string settingsNote;
		if (choose)
		{
			if (!ChooseConditions(ifcPath, options, settingsShown, settingsNote))
				return TestRoundResult{};
			// **選び直したら引き比べる相手も捨てる。** 別の IFC・設定の周と内訳を並べても
			// 「直した結果どう動いたか」にならない。作業ファイルと前の周が作ったレイヤは
			// 残す——図面を戻すのに要る。
			session.round = 0;
			session.lastCommit.clear();
			session.lastTally.clear();
			session.baselineRecorded = false;
			session.baselineLayers.clear();
		}
		else
		{
			// **続きの周は何も出さない。** 1 周目の選択（ファイル・設定）をそのまま使う。
			settingsNote = "前の周の設定をそのまま使いました（実機テストの続きの周）";
		}
		session.ifcPath = ifcPath;
		session.options = options;

		// **取り除きは 1 回だけ呼び、その説明を 2 か所へ配る**——診断ログ（prologue）と
		// 報告（FeedbackRound::preparation）。ログは上限で切り詰められるので、報告の側にも
		// 置かないと読めない周が出る（実機 round 2 で実際に落ちた）。
		std::string preparation;
		const RoundDocument document = openRoundDocument(session, rebased, preparation);
		if (document == RoundDocument::Fallback)
			preparation += "。" + prepareDrawingForRound(session);
		// **「準備:」はここで 1 度だけ付ける。** 部品（openRoundDocument /
		// prepareDrawingForRound / RemoveCreatedLayers）がめいめいに付けていた頃は、
		// つないだ 1 行に「準備:」が 2 度出ていた（PR #188 の実機確認）。
		if (!preparation.empty())
			preparation = "準備: " + preparation;
		if (!scratchNote.empty())
			preparation += (preparation.empty() ? "" : "\n") + scratchNote;
		if (document == RoundDocument::Abort)
		{
			// **描く先が無いなら取り込まない。** 記憶（作業ファイルの場所）は残すので、
			// 人がその図面を開いてからもう一度実行すれば続きの周として走る。
			(void)core::writeFeedbackSession(sessionPath, session);
			core::trace::note(preparation);
			return Failure(allowDialogs, parse::TestRoundOutcome::DocumentFailed, preparation);
		}
		const ImportRound round =
			runImportRound(ifcPath, options, settingsShown, settingsNote, preparation);
		if (round.failed)
		{
			// **取り込みの完了文言（round.body）は使わない**——このコマンド自身の言葉で言う
			// （parse/Feedback.h「実機テストの周の結末」）。
			return Failure(allowDialogs, parse::TestRoundOutcome::ImportFailed, {});
		}

		// 報告を組む（無 SDK 側。parse/Feedback）。
		parse::FeedbackRound material;
		material.build = build;
		material.ifcPath = ifcPath;
		material.bytes = round.bytes;
		material.seconds = round.seconds;
		material.startedAt = round.startedAt;
		material.log = core::trace::text();
		material.round = session.round + 1;
		material.previousCommit = session.lastCommit;
		material.previousTally = session.lastTally;
		// 1 周目に採った基準（＝取り込み前に在ったレイヤの顔ぶれ）。次の周はここへ戻って
		// いるかを引き比べる（parse/Feedback の restoredStateLine）。
		material.baselineKnown = session.baselineRecorded;
		material.baselineLayers = session.baselineLayers;
		material.preparation = preparation;
		material.restorable = document == RoundDocument::Ready;
		const std::string report =
			parse::formatTestRoundReport(material, round.document, round.counts);

		// **記憶を進める。**
		session.round = material.round;
		// **基準は 1 周目に採る。** 以後の周では触らない——基準そのものが周ごとに動くと、
		// 「戻っているか」を引き比べる相手が消える。
		if (!session.baselineRecorded)
		{
			session.baselineRecorded = true;
			session.baselineLayers = round.counts.existingLayers;
		}
		// **キャンセルされた周は、そのビルドを「試し終えた」ことにしない**（実機 round 10。
		// 報告の「前の周」の見出しが、途中までしか描いていない周のビルドを名乗らないように）。
		if (!round.counts.cancelled)
			session.lastCommit = build.commit;
		// **次の周の前に取り除く顔ぶれ。** この周が自分で作ったレイヤだけを名指しで持つ
		// （prepareDrawingForRound）。前の周の分は用済みなので置き換える。
		session.lastCreatedLayers = round.counts.createdLayers;
		session.lastCreatedSheets = round.counts.createdSheets;
		session.lastTally = parse::formatTally(parse::elementRows(round.document, round.counts));

		TestRoundResult result;
		result.ran = true;
		result.round = material.round;
		result.report = report;
		std::string detail = "round " + std::to_string(material.round) + "（" + build.commit + "）";
		const std::string reportPath = core::testReportPathFor(sessionPath);
		if (!core::writeFeedbackSession(sessionPath, session))
			detail += "\n次の周のための記憶を保存できませんでした（次の実機テストはファイル選択から"
					  "始まります）。";
		if (WriteReport(reportPath, report))
			result.reportPath = reportPath;
		else
			detail += "\n報告をファイルへ書けませんでした（" +
					  (reportPath.empty() ? std::string("置き場所が分かりません") : reportPath) +
					  "）。";
		result.message = parse::formatTestRoundResult(parse::TestRoundOutcome::Completed, detail);
		// **うまく行った周は何も出さない**（draw/Feedback.h「取り込みのあとに人の操作を
		// 残さない」）。
		return result;
	}
} // namespace HomeskzIfcImport::draw
