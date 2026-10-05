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

// 周ごとにテンプレートから図面を開く（ISDK::OpenDocumentPath）。パスは IFileIdentifier で渡す。
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// **実機テストの結果ダイアログのタイトル。** 本番の取り込み（「ホームズ君 IFC
		// 取り込み」。draw/ImportCommand.cpp）と**必ず違う名前にする**——同じにすると、
		// 実機テストの顛末を本番の取り込みが言っているように見える（実機の指摘。M25）。
		constexpr const char* kTestResultTitle = "実機テスト (みんなの構造設計支援Dev)";

		// **周ごとに、テンプレートから開いた新しい図面へ描く**（M39）。
		//
		// 1 周目に、そのとき開いている図面を一時ファイルの置き場へ**テンプレート（`.sta`）
		// として別名保存**し、以後の周は毎回それを `OpenDocumentPath` で開く。`.sta` を渡すと
		// **そのファイル自体ではなく、中身を写した無題の新規文書**が開く（[SDK リファレンス
		// 「Documents」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Documents.md)）
		// ので、テンプレートには前の周の絵が 1 つも書き戻らず、どの周もまったく同じ初期状態
		// から始まる。
		//
		// **「取り消し」とレイヤ削除による戻しはやめた**（M39。利用者の指示）。取り消しは
		// 取り消しスタックの中身に、レイヤ削除は「取り込み前から在ったレイヤへ描いた分は
		// 残る」に左右され、どちらも「戻ったかどうか」を周ごとに疑う必要があった。
		//
		// **自分で保存した図面は覚えておき、次の周の頭で保存せずに閉じる**
		// （core::FeedbackSession::ownedDocuments）。各周の描き上がりは周の終わりに一時
		// ファイルへ保存するので、人が手を入れていなければ未保存の変更は無い——Vectorworks を
		// 再起動しても保存の確認が出ない。`CloseDocument()` は確認なしに未保存の変更を捨てる
		// （同 Findings）ので、閉じる相手は core::isOwnedTestDocument が 1 か所で絞る
		// （CLAUDE.md「開発の基本方針」8）。
		//
		// **ここは実機テストの周だけ。** 本番の取り込みは開いている図面へ描くのが仕事で、
		// この関数群を呼ばない（draw/Feedback.h・CLAUDE.md M25）。

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

		// IFileIdentifier のフルパス（引けなければ空）。
		std::string PathOf(const VectorWorks::Filing::IFileIdentifierPtr& fileID)
		{
			if (!fileID)
				return "";
			TXString path;
			if (fileID->GetFileFullPath(path) != kVCOMError_NoError)
				return "";
			return static_cast<const char*>(path);
		}

		// いまアクティブな図面のパス（取得できなければ空）と、保存済みか。**開けたかどうかは
		// `OpenDocumentPath` の戻り値ではなくこれで判定する**（読み戻して確かめる。
		// SDK リファレンス「Investigation Techniques」）。
		//
		// **無題の図面でもパスは返る**——「アプリケーションのあるディレクトリ＋名称未設定 N」
		// が入る（実機確認済み。SDK リファレンス「Documents」）。保存済みかどうかを見たい
		// ときはパスの有無ではなく `saved` を使うこと。
		std::string ActiveDocumentPath(bool* saved = nullptr)
		{
			VectorWorks::Filing::IFileIdentifierPtr fileID;
			bool isSaved = false;
			if (!gSDK->GetActiveDocument(&fileID, isSaved) || !fileID)
				return "";
			if (saved != nullptr)
				*saved = isSaved;
			return PathOf(fileID);
		}

		// 開いている図面 1 つ（ISDK::GetOpenFilesList の 1 件）。
		struct OpenDocument
		{
			std::string path;
			// SwitchToOpenFile へ渡す鍵。**不透明な値として扱う**（連番ではない。
			// SDK リファレンス「Documents」）。
			Sint32 fileRef = -1;
		};

		// いま開いている図面の一覧。**1 つ閉じるたびに引き直す**（fileRef を使い回さない）。
		std::vector<OpenDocument> OpenDocuments()
		{
			std::vector<OpenDocument> out;
			VectorWorks::TVWArray_OpenFileInformation list;
			gSDK->GetOpenFilesList(list);
			for (std::size_t i = 0; i < list.GetSize(); ++i)
			{
				const VectorWorks::SOpenFileInformation& info = list.GetAt(i);
				out.push_back(OpenDocument{PathOf(info.fpFileID), info.fFileRef});
			}
			return out;
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

		// その図面がいま開いているか。
		bool IsOpen(const std::string& path)
		{
			return std::ranges::any_of(OpenDocuments(), [&path](const OpenDocument& doc)
									   { return SamePath(doc.path, path); });
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
		// 上書きが効くことに賭けない。名前が尽きたら空を返す。extension は "." 付き。
		//
		// **置き場はブランチごとのフォルダ**（core/FeedbackScratch.h）。PR が閉じたら
		// フォルダごと片付けられるように、いま動いているビルドのブランチの下へ置く
		// （M38 までは一時ディレクトリの直下へ置いたきりで、PR #188 の実機確認で
		// 3.4 GB を超えて溜まっていた）。
		std::string FreshTempPath(const std::string& stem, const std::string& extension)
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
					(std::filesystem::path(dir) / (stem + "-" + std::to_string(i) + extension))
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
		// 消したフォルダに記憶のテンプレートがあったら、記憶から外す（無いファイルを
		// 開きに行かせない）。次の周は 1 周目と同じく、いま開いている図面から採る。
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
					   "ほかのブランチの一時ファイルは片付けませんでした";

			const core::ScratchCleanup cleanup =
				core::cleanUpClosedBranches(root, others, core::parsePrStates(out));
			for (const std::string& removed : cleanup.removedPaths)
			{
				if (core::pathIsInside(session.templatePath, removed))
					session.templatePath.clear();
				std::erase_if(session.ownedDocuments, [&removed](const std::string& doc)
							  { return core::pathIsInside(doc, removed); });
			}
			return core::describeScratchCleanup(cleanup);
		}

		// アクティブな図面を指定のパスへ保存する（＝別名保存）。**保存できたかは読み戻して
		// 確かめる**——アクティブな図面がそのパスのファイルになっていれば成功。
		bool SaveActiveDocumentAs(const std::string& path)
		{
			if (path.empty())
				return false;
			const VectorWorks::Filing::IFileIdentifierPtr fileID = FileIdFor(path);
			if (!fileID || gSDK->SaveActiveDocumentPath(fileID) != 0)
				return false;
			return SamePath(ActiveDocumentPath(), path);
		}

		// **自分で保存した図面のうち、いま開いているものを保存せずに閉じる**（M39）。
		// 戻り値は報告へ出す 1 行（閉じるものが無ければ空）。
		//
		// 閉じる相手は core::isOwnedTestDocument が絞る（記憶に名指しで在り、一時ファイルの
		// 置き場の中にあるものだけ）。**切り替えたあとにもう一度確かめる**——
		// SwitchToOpenFile が別の図面をアクティブにしたまま CloseDocument を呼ぶと、
		// 利用者の図面の変更を確認なしに捨ててしまう。
		//
		// **`CloseDocument` の戻り値で判定しない。** 閉じていても false を返す（実機 round 9
		// と SDK リファレンス「Documents」）。閉じたかは開いている図面の一覧を読み戻して見る。
		//
		// 閉じ終えたら、記憶から「もう開いていないもの」を外す（再起動で閉じたものも含めて）。
		// 閉じられずに開いたまま残ったものは記憶に残し、次の周でもう一度試す。
		std::string CloseOwnedDocuments(core::FeedbackSession& session)
		{
			const std::string root = TempPath(core::kScratchRootName);
			std::size_t closed = 0;
			std::vector<std::string> failed;
			// 1 つ閉じるたびに一覧を引き直す。上限は暴走止めだけ（記憶の数より多くは回らない）。
			for (std::size_t guard = 0; guard <= session.ownedDocuments.size(); ++guard)
			{
				const std::vector<OpenDocument> docs = OpenDocuments();
				const auto target = std::ranges::find_if(
					docs,
					[&](const OpenDocument& doc)
					{
						return core::isOwnedTestDocument(session, doc.path, root) &&
							   std::ranges::none_of(failed, [&doc](const std::string& f)
													{ return SamePath(f, doc.path); });
					});
				if (target == docs.end())
					break;
				const std::string path = target->path;
				if (!gSDK->SwitchToOpenFile(target->fileRef) ||
					!core::isOwnedTestDocument(session, ActiveDocumentPath(), root) ||
					!SamePath(ActiveDocumentPath(), path))
				{
					failed.push_back(path);
					continue;
				}
				(void)gSDK->CloseDocument(); // 戻り値は当てにならない（上記）
				if (IsOpen(path))
					failed.push_back(path);
				else
					++closed;
			}

			std::erase_if(session.ownedDocuments,
						  [](const std::string& doc) { return !IsOpen(doc); });

			if (closed == 0 && failed.empty())
				return "";
			std::string note =
				"前の周の図面 " + std::to_string(closed) + " 枚を保存せずに閉じました";
			if (!failed.empty())
			{
				note += "（閉じられなかったもの: ";
				for (std::size_t i = 0; i < failed.size(); ++i)
					note += (i == 0 ? "" : " / ") + failed[i];
				note += "）";
			}
			return note;
		}

		// 描く図面を用意できたか。
		enum class RoundDocument
		{
			// テンプレートから開いた新しい図面がアクティブになっている。
			Ready,
			// **描く先が無い。** 取り込みを始めてはいけない。
			Abort,
		};

		// **テンプレートを採る**（1 周目と、人が別の図面へ移ってから押した周）。いま開いて
		// いる図面を一時ファイルの置き場へ `.sta` で別名保存する。成功したら true。
		//
		// 別名保存すると**その文書自身がテンプレートのファイルになる**（同じパスはもう一度は
		// 開けない。SDK リファレンス「Documents」）。この文書は自分の図面として覚え、すぐ
		// あとの CloseOwnedDocuments で閉じる——保存した直後なので失うものは無い。元の
		// 図面のファイルには何も書き戻らない。
		bool CaptureTemplate(core::FeedbackSession& session, std::string& note)
		{
			const std::string root = TempPath(core::kScratchRootName);
			const std::string active = ActiveDocumentPath();
			if (OpenDocuments().empty() || active.empty())
			{
				note = "開いている図面が無いので、テンプレートを採れませんでした";
				return false;
			}
			// **前の周の描き上がりをテンプレートにしない**——以後の周がずっと前の周の絵の
			// 上から始まる（M25 の実機 round 13 で、汚れた図面が基準へ焼き付いた）。
			if (core::isOwnedTestDocument(session, active, root))
			{
				note = "いま開いているのは前の周の図面（" + active +
					   "）なので、テンプレートには採りませんでした";
				return false;
			}

			const std::string path = FreshTempPath("template", ".sta");
			if (!SaveActiveDocumentAs(path))
			{
				// **証拠を残す**——何を保存しようとして駄目だったのかが分からないと、次の
				// 周も同じところで止まる（実機 round 12）。
				note = "テンプレートを保存できませんでした（" +
					   (path.empty() ? std::string("一時ディレクトリが引けません") : path) +
					   " / いまの図面は " + active + "）";
				return false;
			}
			session.templatePath = path;
			session.ownedDocuments.push_back(path);
			// **基準も採り直す。** 別の図面で採った顔ぶれと引き比べても意味が無い（「基準に
			// 無いレイヤ」が出るだけで、読む側を惑わせる）。次の報告がこの図面の顔ぶれを
			// 基準として採り直す。
			session.baselineRecorded = false;
			session.baselineLayers.clear();
			note = "いま開いている図面（" + active + "）をテンプレートとして保存しました（" + path +
				   "）。元の図面のファイルは変わっていません";
			return true;
		}

		// **この周の図面を用意する。** note には診断ログと報告へ出す 1 行が入る。
		// rebased は「テンプレートを採り直した」ときにその理由（runTestRound が作る）。
		RoundDocument openRoundDocument(core::FeedbackSession& session, const std::string& rebased,
										std::string& note)
		{
			std::vector<std::string> parts;
			if (!rebased.empty())
				parts.push_back(rebased);
			const auto joined = [&parts]()
			{
				std::string text;
				for (const std::string& part : parts)
					text += (text.empty() ? "" : "。") + part;
				return text;
			};

			if (session.templatePath.empty())
			{
				std::string captured;
				const bool ok = CaptureTemplate(session, captured);
				parts.push_back(captured);
				if (!ok)
				{
					note = joined();
					return RoundDocument::Abort;
				}
			}

			// **自分の図面を閉じる**（いま採ったテンプレートの文書も、前の周の描き上がりも）。
			// テンプレートのファイルが開いたままだと、それを開いて新しい図面を作れない。
			const std::string closed = CloseOwnedDocuments(session);
			if (!closed.empty())
				parts.push_back(closed);

			if (!PathExists(session.templatePath))
			{
				// 次のメニューの周がいま開いている図面から採り直せるよう、記憶から外す。
				parts.push_back("テンプレートが見つかりません（" + session.templatePath + "）");
				session.templatePath.clear();
				note = joined();
				return RoundDocument::Abort;
			}
			if (IsOpen(session.templatePath))
			{
				parts.push_back(
					"テンプレートのファイルが開いたままなので、新しい図面を作れません（" +
					session.templatePath + "）");
				note = joined();
				return RoundDocument::Abort;
			}

			// **テンプレートから新しい図面を開く。** `.sta` を渡すと無題の新規文書が開く
			// （SDK リファレンス「Documents」）。bShowErrorMessages=false: 誰も見ていない周で
			// ダイアログを出さない。
			const std::size_t before = OpenDocuments().size();
			const VectorWorks::Filing::IFileIdentifierPtr fileID = FileIdFor(session.templatePath);
			const bool returned = fileID && gSDK->OpenDocumentPath(fileID, false);
			// **戻り値だけを信じない**——開いたかどうかは一覧とカレント文書を読み戻して確かめる。
			// 増えたのが 1 枚で、アクティブな図面が未保存（無題）なら、テンプレートから
			// 開いた新しい図面である。
			bool saved = true;
			const std::string opened = ActiveDocumentPath(&saved);
			if (OpenDocuments().size() == before + 1 && !saved &&
				!SamePath(opened, session.templatePath))
			{
				parts.push_back("テンプレートから新しい図面を開きました（" + session.templatePath +
								"）");
				note = joined();
				return RoundDocument::Ready;
			}

			// **ここから先へ進まない。** いまアクティブなのは利用者の図面か、テンプレートの
			// ファイルそのものかもしれない——描けばそこを汚す（実機 round 9 では、描く先が
			// 無いまま描いて全 18 要素が 0 件になった）。**何が起きたかを証拠つきで残して
			// 周ごと中止する。**
			std::string why = "テンプレートから新しい図面を開けなかったので、この周は走らせません"
							  "でした（OpenDocumentPath=";
			why += returned ? "true" : "false";
			why += " / いまは " + (opened.empty() ? std::string("(取得できず)") : opened);
			why += saved ? "（保存済み）" : "（未保存）";
			why += "）";
			parts.push_back(why);
			note = joined();
			return RoundDocument::Abort;
		}

		// **描き上がりを一時ファイルへ保存する**（M39）。保存しておけば未保存の変更が残らず、
		// Vectorworks を再起動しても保存の確認が出ない。保存した図面は自分の図面として覚え、
		// 次の周の頭で閉じる。戻り値は報告へ出す 1 行。
		//
		// 取り込みが失敗・中止した周も呼ぶ——描きかけの図面も自分の図面であり、残すと
		// 次の周で閉じられない。
		std::string SaveRoundDocument(core::FeedbackSession& session, int round)
		{
			const std::string path = FreshTempPath("round-" + std::to_string(round), ".vwx");
			if (!SaveActiveDocumentAs(path))
				return "描き上がりを一時ファイルへ保存できませんでした（" +
					   (path.empty() ? std::string("一時ディレクトリが引けません") : path) +
					   "）。この図面は未保存のまま残るので、次の周では閉じられず、"
					   "Vectorworks を再起動するときに保存の確認が出ます";
			session.ownedDocuments.push_back(path);
			return "描き上がりを " + path + " へ保存しました（次の周の頭で閉じます）";
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
			const std::string advice =
				"前回（round " + std::to_string(session.round) + "）の IFC: " + ifc +
				"\n前の周の図面は保存せずに閉じ、テンプレートから開いた新しい図面へ描きます。";
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

		// **片付けはテンプレートの採り直しより先に。** 記憶のテンプレートを消したなら、下の
		// 判断はそれが無いものとして進む。
		const std::string scratchNote = CleanUpClosedBranches(session, build.branch);

		// **手動で押したときは、いま開いている図面からテンプレートを採り直す。** 人が別の
		// 図面（空のテンプレート等）を開いてからメニューを押したのは「この図面で試したい」
		// という意思なのに、覚えたテンプレートを開くとその意思が黙って消える——実機で
		// 二度、空のテンプレートで試そうとして前の周の図面に上書きされた。**記憶ごと
		// 消さない**（IFC・設定はそのまま）——捨てるのは「どの図面から始めるか」だけ。
		// いま開いているのが前の周の描き上がり（自分の図面）なら続きの周で、図面が 1 枚も
		// 開いていなければ採りようが無いので、覚えたテンプレートを使う。
		// MCP の周はここへ来ない: そちらは前の周の続きなので、テンプレートから始めるのが正しい。
		std::string rebased;
		if (allowDialogs && !session.templatePath.empty() && !OpenDocuments().empty())
		{
			const std::string openPath = ActiveDocumentPath();
			if (!core::isOwnedTestDocument(session, openPath, TempPath(core::kScratchRootName)))
			{
				rebased = "いま開いている図面（" +
						  (openPath.empty() ? std::string("(取得できず)") : openPath) +
						  "）は前の周の図面ではないので、こちらから新しいテンプレートを採りました";
				session.templatePath.clear();
			}
		}

		// **どの周になるかは無 SDK 側が決める**（core/FeedbackSession.h）。
		const core::FeedbackRoundKind kind = core::feedbackRoundKind(session, allowDialogs);
		if (kind == core::FeedbackRoundKind::Refuse)
			return Failure(allowDialogs, parse::TestRoundOutcome::NotRemembered,
						   core::feedbackSessionRemembered(session)
							   ? "（テンプレートの記憶がありません——M39 より前の版の記憶か、"
								 "一時ファイルが片付けられた後です）"
							   : "");

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
			// 「直した結果どう動いたか」にならない。テンプレートと自分の図面の記憶は残す
			// ——この周の図面を用意するのに要る。
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

		// **図面の用意は 1 回だけ呼び、その説明を 2 か所へ配る**——診断ログ（prologue）と
		// 報告（FeedbackRound::preparation）。ログは上限で切り詰められるので、報告の側にも
		// 置かないと読めない周が出る（実機 round 2 で実際に落ちた）。
		std::string preparation;
		const RoundDocument document = openRoundDocument(session, rebased, preparation);
		// **「準備:」はここで 1 度だけ付ける。** 部品がめいめいに付けていた頃は、つないだ
		// 1 行に「準備:」が 2 度出ていた（PR #188 の実機確認）。
		if (!preparation.empty())
			preparation = "準備: " + preparation;
		if (!scratchNote.empty())
			preparation += (preparation.empty() ? "" : "\n") + scratchNote;
		if (document == RoundDocument::Abort)
		{
			// **描く先が無いなら取り込まない。** 記憶（テンプレートの場所・閉じ残した図面）は
			// 書き残すので、直してからもう一度実行すれば続きの周として走る。
			(void)core::writeFeedbackSession(sessionPath, session);
			core::trace::note(preparation);
			return Failure(allowDialogs, parse::TestRoundOutcome::DocumentFailed, preparation);
		}
		const ImportRound round =
			runImportRound(ifcPath, options, settingsShown, settingsNote, preparation);
		// **描き上がりは、成否にかかわらず保存して覚える**（SaveRoundDocument）。
		const std::string saved = SaveRoundDocument(session, session.round + 1);
		core::trace::note(saved);
		if (round.failed)
		{
			(void)core::writeFeedbackSession(sessionPath, session);
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
		material.preparation = preparation + (preparation.empty() ? "" : "\n") + "後始末: " + saved;
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
