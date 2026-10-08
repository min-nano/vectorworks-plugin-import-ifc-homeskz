//
//	draw/Feedback.cpp
//
//	実機テストの 1 周の実装（意図と入口は draw/Feedback.h 参照）。結果は手元のファイルへ
//	書き、ローカルの Claude Code が MCP ブリッジ越しに読む（draw/McpBridge.cpp の
//	`vw_test_report`）。
//
//	**ネットワークには一切アクセスしない**（M38）。
//
//	【ダイアログを 1 枚も出さない】周を起こすのは MCP の `vw_run_test` だけで、誰も操作して
//	いない Vectorworks を止めないため。失敗も結末の文言で返す（M43 でメニューの入口を
//	削除するまでは、人が押した周だけダイアログで尋ね・伝えていた）。
//
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include する。
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
		// **周ごとに、テンプレートから開いた新しい図面へ描画する**（M39）。
		//
		// テンプレートは周ごとに `vw_run_test` の `template` で渡される（M43）。それを
		// `OpenDocumentPath` で開く。`.sta` を渡すと
		// **そのファイル自体ではなく、中身を複製した無題の新規文書**が開く（[SDK リファレンス
		// 「Documents」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Documents.md)）
		// ので、テンプレートには前の周の描画結果が 1 つも書き戻らず、どの周もまったく同じ
		// 初期状態から始まる。
		//
		// **自分で保存した図面は記録しておき、次の周の頭で保存せずに閉じる**
		// （core::FeedbackSession::ownedDocuments）。閉じる相手は core::isOwnedTestDocument
		// が 1 か所で絞る（CLAUDE.md「開発の基本方針」8）——`CloseDocument()` は確認なしに
		// 未保存の変更を捨てる（同 Findings）。各周の描画結果は周の終わりに一時ファイルへ
		// 保存するので、人が手を入れていなければ未保存の変更は無い——Vectorworks を再起動
		// しても保存の確認が出ない。
		//
		// **ここは実機テストの周だけ。** 本番の取り込みは開いている図面へ描画するのが仕事で、
		// この関数群を呼ばない（draw/Feedback.h・CLAUDE.md「本番の取り込みコマンドに実機テストを
		// 書かない」）。
		//
		// **「取り消し」とレイヤ削除による復元は廃止した**（M39。利用者の指示）。取り消しは
		// 取り消しスタックの中身に、レイヤ削除は「取り込み前から在ったレイヤへ描画した分は
		// 残る」に左右され、どちらも「復元できたかどうか」を周ごとに疑う必要があった。

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

		// IFileIdentifier のフルパス（取得できなければ空）。
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
		// 保存済みかどうかを判定するときはパスの有無ではなく `saved` を使うこと。**無題の
		// 図面でもパスは返る**——「アプリケーションのあるディレクトリ＋名称未設定 N」が入る
		// （実機確認済み。SDK リファレンス「Documents」）。
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

		// いま開いている図面の一覧。**1 つ閉じるたびに取得し直す**（fileRef を使い回さない）。
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

		// 同じファイルを指しているか。**大文字小文字や区切りの差で判定を誤らない**よう
		// std::filesystem に正規化させ、それが機能しない場面（まだ無いファイルなど）は
		// 文字列のままの比較にフォールバックする。
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

		// 一時ディレクトリの中のパスを組む（temp が取得できなければ空）。
		std::string TempPath(const std::string& name)
		{
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
			if (ec)
				return "";
			return (dir / name).string();
		}

		// そのパスに何か在るか。開けるかどうかで判定し、**`std::filesystem` では確認しない**
		// ——`file_size` の stat が MSVC の clang-analyzer に偽陽性を出した前例がある（在るのに
		// 開けないなら、どのみち上書きも当てにできない）。
		bool PathExists(const std::string& path)
		{
			if (path.empty())
				return false;
			const std::ifstream in(path, std::ios::binary);
			return in.good();
		}

		// **まだ無いパスを選ぶ。** extension は "." 付き。名前が尽きたら空を返す。
		// 上書き保存が成功することを前提にしない——既にあるファイルへ別名保存できなかった
		// 実測がある（実機 round 12。round 8 は同じ呼び出しが新規のパスで通っている）。
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
				// performance-no-automatic-move がエラーになる（tidy-mac で実際に失敗した）。
				// 名前は += で組む（ループの中の連結は clang-tidy の
				// performance-inefficient-string-concatenation に掛かる）。
				std::string name = stem;
				name += "-";
				name += std::to_string(i);
				name += extension;
				std::string path = (std::filesystem::path(dir) / name).string();
				if (!PathExists(path))
					return path;
			}
			return "";
		}

		// **PR が閉じたブランチの一時ファイルを片付ける**（core/FeedbackScratch.h。利用者の
		// ご要望で、PR が close／merge されたらその実機テストの図面は消す）。周の頭で
		// 1 度だけ呼ぶ。戻り値は報告と診断ログへ出す 1 行（何もしなければ空）。
		//
		// **PR の状態が分からなければ消さない。** PR の状態は同梱スクリプト（vw-update の
		// q-pr-state。トークンは vw-token が持つ）に尋ねる。いま動いているブランチのフォルダは
		// 問い合わせの対象にしない（使っている最中）。ほかのブランチが 1 つも無ければ GitHub へも
		// 問い合わせない——毎周の問い合わせで API の上限を消費しないため。
		//
		// 消したフォルダにあった図面は、閉じる相手の記録から削除する。
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
		// 閉じる相手は core::isOwnedTestDocument が絞る（記録に名前が在り、一時ファイルの
		// 置き場の中にあるものだけ）。**切り替えたあとにもう一度確かめる**——
		// SwitchToOpenFile が別の図面をアクティブにしたまま CloseDocument を呼ぶと、
		// 利用者の図面の変更を確認なしに捨ててしまう。
		//
		// **`CloseDocument` の戻り値で判定しない。** 閉じていても false を返す（実機 round 9
		// と SDK リファレンス「Documents」）。閉じたかは開いている図面の一覧を読み戻して確認する。
		//
		// 閉じ終えたら、記録から「もう開いていないもの」を削除する（再起動で閉じたものも含めて）。
		// 閉じられずに開いたまま残ったものは記録に残し、次の周でもう一度試す。
		std::string CloseOwnedDocuments(core::FeedbackSession& session)
		{
			const std::string root = TempPath(core::kScratchRootName);
			std::size_t closed = 0;
			std::vector<std::string> failed;
			// 1 つ閉じるたびに一覧を取得し直す。上限は無限ループ防止のためだけ（記録の数より
			// 多くは回らない）。
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

		// 描画する図面を用意できたか。
		enum class RoundDocument
		{
			// テンプレートから開いた新しい図面がアクティブになっている。
			Ready,
			// **描画先が無い。** 取り込みを始めてはいけない。
			Abort,
		};

		// **MCP に渡されたテンプレートを使えるか**（M40）。使えなければ理由を note に入れて
		// false。**写さずにそのまま開く**——`.sta` を開くと中身を複製した無題の新規文書が
		// 開き、ファイルそのものには何も書き戻らない（SDK リファレンス「Documents」）。
		// M42 までは記憶して続きの周でも開くために一時ファイルの置き場へ写していたが、
		// テンプレートは毎周渡されるようになった（M43）。
		bool CheckTemplate(const std::string& path, std::string& note)
		{
			// `.sta` 以外を `OpenDocumentPath` へ渡すと**そのファイル自体**が開き、そこへ
			// 描画することになる（SDK リファレンス「Documents」）。先に除外して理由を返す。
			const std::string::size_type dot = path.find_last_of('.');
			if (dot == std::string::npos || path.substr(dot) != ".sta")
			{
				note = "テンプレートには .sta を渡してください（" + path + "）";
				return false;
			}
			if (!PathExists(path))
			{
				note = "テンプレートが見つかりません（" + path + "）";
				return false;
			}
			return true;
		}

		// **この周の図面を用意する。** note には診断ログと報告へ出す 1 行が入る。
		RoundDocument openRoundDocument(core::FeedbackSession& session,
										const std::string& templatePath, std::string& note)
		{
			std::vector<std::string> parts;
			const auto joined = [&parts]()
			{
				std::string text;
				for (const std::string& part : parts)
					text += (text.empty() ? "" : "。") + part;
				return text;
			};

			// **自分の図面を閉じる**（前の周の描画結果）。
			const std::string closed = CloseOwnedDocuments(session);
			if (!closed.empty())
				parts.push_back(closed);

			if (!PathExists(templatePath))
			{
				parts.push_back("テンプレートが見つかりません（" + templatePath + "）");
				note = joined();
				return RoundDocument::Abort;
			}
			if (IsOpen(templatePath))
			{
				parts.push_back(
					"テンプレートのファイルが開いたままなので、新しい図面を作れません（" +
					templatePath + "）");
				note = joined();
				return RoundDocument::Abort;
			}

			// **テンプレートから新しい図面を開く。** `.sta` を渡すと無題の新規文書が開く
			// （SDK リファレンス「Documents」）。bShowErrorMessages=false: 誰も操作していない
			// 周でダイアログを出さない。
			const std::size_t before = OpenDocuments().size();
			const VectorWorks::Filing::IFileIdentifierPtr fileID = FileIdFor(templatePath);
			const bool returned = fileID && gSDK->OpenDocumentPath(fileID, false);
			// **戻り値だけで判定しない**——開いたかどうかは一覧とカレント文書を読み戻して確かめる。
			// 増えたのが 1 枚で、アクティブな図面が未保存（無題）なら、テンプレートから
			// 開いた新しい図面である。
			bool saved = true;
			const std::string opened = ActiveDocumentPath(&saved);
			if (OpenDocuments().size() == before + 1 && !saved && !SamePath(opened, templatePath))
			{
				parts.push_back("テンプレートから新しい図面を開きました（" + templatePath + "）");
				note = joined();
				return RoundDocument::Ready;
			}

			// **ここから先へ進まない。何が起きたかを詳細つきで残して周ごと中止する。**
			// いまアクティブなのは利用者の図面か、テンプレートのファイルそのものかもしれず、
			// 描画すればそこを書き換えてしまう（実機 round 9 では、描画先が無いまま描画して
			// 全 18 要素が 0 件になった）。
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

		// **描画結果を一時ファイルへ保存する**（M39）。保存した図面は自分の図面として記録し、
		// 次の周の頭で閉じる。戻り値は報告へ出す 1 行。
		//
		// 取り込みが失敗・中止した周も呼ぶ——描画途中の図面も自分の図面であり、残すと
		// 次の周で閉じられない。
		//
		// 保存しておけば未保存の変更が残らず、Vectorworks を再起動しても保存の確認が出ない。
		std::string SaveRoundDocument(core::FeedbackSession& session)
		{
			const std::string path = FreshTempPath("round", ".vwx");
			if (!SaveActiveDocumentAs(path))
				return "描き上がりを一時ファイルへ保存できませんでした（" +
					   (path.empty() ? std::string("一時ディレクトリが引けません") : path) +
					   "）。この図面は未保存のまま残るので、次の周では閉じられず、"
					   "Vectorworks を再起動するときに保存の確認が出ます";
			session.ownedDocuments.push_back(path);
			return "描き上がりを " + path + " へ保存しました（次の周の頭で閉じます）";
		}

		// 報告を書く。書けたら true。
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

		// 失敗の結末を作る。
		TestRoundResult Failure(parse::TestRoundOutcome outcome, const std::string& detail)
		{
			TestRoundResult result;
			result.message = parse::formatTestRoundResult(outcome, detail);
			return result;
		}
	} // namespace

	// -----------------------------------------------------------------------
	bool feedbackAvailable()
	{
#ifdef VW_DEV_BUILD
		return true;
#else
		// **安定版では動かさない。** 開発の道具（頼まれた条件での無人の取り込み）を
		// 利用者向けの配布物に持たせない。
		return false;
#endif
	}

	std::string testReportPath()
	{
		return core::testReportPathFor(core::defaultFeedbackSessionPath());
	}

	// -----------------------------------------------------------------------
	// **実機テストを終える**（M42。draw/Feedback.h）。
	TestCleanupResult endTestSession()
	{
		TestCleanupResult result;
		if (!feedbackAvailable())
		{
			result.message = "実機テストは開発版（Dev）のビルドでのみ使えます。";
			return result;
		}

		const std::string sessionPath = core::defaultFeedbackSessionPath();
		core::FeedbackSession session;
		(void)core::readFeedbackSession(sessionPath, session); // 読めなければ空の記録
		// **消すフォルダを選ぶ手掛かりは閉じる前に控える**——CloseOwnedDocuments は閉じた
		// 図面を記録から削除する。
		const std::vector<std::string> used = session.ownedDocuments;

		std::vector<std::string> notes;
		const std::string closed = CloseOwnedDocuments(session);
		if (!closed.empty())
			notes.push_back(closed);
		const auto joined = [&notes]()
		{
			std::string text;
			for (const std::string& note : notes)
				text += (text.empty() ? "" : "。") + note;
			return text;
		};

		// **閉じ残しがあれば何も消さない。** 開いている図面のファイルは removeScratchDir も
		// 消さない（`*.lck`）が、記録まで消すと次の周がその図面を閉じられなくなる。
		if (!session.ownedDocuments.empty())
		{
			(void)core::writeFeedbackSession(sessionPath, session);
			notes.emplace_back("閉じられない図面が残ったので、一時ファイルと記録は残しました");
			result.message = joined();
			return result;
		}

		const std::string root = TempPath(core::kScratchRootName);
		const core::ScratchCleanup cleanup =
			root.empty() ? core::ScratchCleanup{}
						 : core::removeScratchDirs(
							   root, core::sessionScratchDirs(core::listScratchDirs(root),
															  currentBuildInfo().branch, used));
		if (!cleanup.removedBranches.empty())
		{
			std::string note = "一時ファイルを片付けました（";
			for (std::size_t i = 0; i < cleanup.removedBranches.size(); ++i)
				note += (i == 0 ? "" : " / ") + cleanup.removedBranches[i];
			notes.push_back(note + "）");
		}
		if (!cleanup.kept.empty())
		{
			std::string note = "片付けなかった一時ファイル: ";
			for (std::size_t i = 0; i < cleanup.kept.size(); ++i)
				note += (i == 0 ? "" : " / ") + cleanup.kept[i];
			notes.push_back(note + "。記録は残しました");
			(void)core::writeFeedbackSession(sessionPath, session);
			result.message = joined();
			return result;
		}

		// **記録と報告を消す。** 報告を残すと、次に使うセッションが前の周の報告を自分の
		// 結果として読みうる（M42 で占有を設けた理由と同じ）。
		core::clearFeedbackSession(sessionPath);
		core::clearFeedbackSession(core::testReportPathFor(sessionPath));
		notes.emplace_back("実機テストの記録と報告を消しました");
		result.done = true;
		result.message = joined();
		return result;
	}

	// -----------------------------------------------------------------------
	// **実機テストの 1 周**（M25 / M38 / M43。draw/Feedback.h）。
	TestRoundResult runTestRound(const TestRoundRequest& request)
	{
		if (!feedbackAvailable())
		{
			TestRoundResult result;
			result.message = "実機テストは開発版（Dev）のビルドでのみ使えます。";
			return result;
		}

		const parse::BuildInfo build = currentBuildInfo();
		const std::string sessionPath = core::defaultFeedbackSessionPath();
		core::FeedbackSession session;
		(void)core::readFeedbackSession(sessionPath, session); // 読めなければ閉じる相手が無い

		// **条件は毎回渡してもらう**（M43。記憶しない）。使えない指定は、何も変えずに理由を
		// 返す。
		if (request.ifcPath.empty() || request.templatePath.empty())
			return Failure(parse::TestRoundOutcome::InvalidRequest,
						   "vw_run_test には ifc（取り込む IFC の絶対パス）と template"
						   "（テンプレートの .sta の絶対パス。リポジトリの "
						   "tests/fixtures/Default.sta）を毎回渡してください");
		if (!PathExists(request.ifcPath))
			return Failure(parse::TestRoundOutcome::InvalidRequest,
						   "IFC が見つかりません（" + request.ifcPath + "）");
		std::string why;
		if (!CheckTemplate(request.templatePath, why))
			return Failure(parse::TestRoundOutcome::InvalidRequest, why);
		// settings は図面を開く前に一度当ててみて、読めなければ図面に触れずに断る。
		// 本当に当てるのは、図面にあるもので既定を組んだあと（下）。
		core::ImportOptions probe;
		if (!core::applyTestSettings(request.settings, probe, why))
			return Failure(parse::TestRoundOutcome::InvalidRequest, why);

		const std::string scratchNote = CleanUpClosedBranches(session, build.branch);

		// **図面の用意は 1 回だけ呼び、その説明を 2 か所へ配る**——診断ログ（prologue）と
		// 報告（FeedbackRound::preparation）。ログは上限で切り詰められるので、報告の側にも
		// 置かないと読めない周が出る（実機 round 2 で実際に欠落した）。
		std::string preparation;
		const RoundDocument document =
			openRoundDocument(session, request.templatePath, preparation);
		// **「準備:」はここで 1 度だけ付ける。** 各部品がそれぞれ付けていた頃は、連結した
		// 1 行に「準備:」が 2 度出ていた（PR #188 の実機確認）。
		if (!preparation.empty())
			preparation = "準備: " + preparation;
		if (!scratchNote.empty())
			preparation += (preparation.empty() ? "" : "\n") + scratchNote;
		if (document == RoundDocument::Abort)
		{
			// **描画先が無いなら取り込まない。** 閉じ残した図面の記録は書き残す。
			(void)core::writeFeedbackSession(sessionPath, session);
			// 準備の行は結末の文言に載せる（ここではまだ取り込みのログを開いていないので、
			// ログへは書けない）。
			return Failure(parse::TestRoundOutcome::DocumentFailed, preparation);
		}
		// **設定はテンプレートから開いた図面から組み、settings で上書きする**
		// （draw::presetImportSettings。設定ダイアログをまだ一度も決めていないときの初期値と
		// 同じ）。既定を集められなければ ImportOptions の既定で続ける——設定を組めない
		// ことを理由に周を中止しない（設定ダイアログを出せなかったときと同じ考え方。
		// draw/SettingsDialog.h）。settings は上で読めることを確かめてある。
		core::ImportOptions options;
		std::string presetNote;
		if (!presetImportSettings(options, &presetNote))
			options = core::ImportOptions{};
		std::string ignored;
		(void)core::applyTestSettings(request.settings, options, ignored);
		std::string settingsNote = "ダイアログを出さず、テンプレートの図面にあるもので既定の"
								   "設定を組みました";
		if (!presetNote.empty())
			settingsNote += "（" + presetNote + "）";
		if (!request.settings.isNull())
			settingsNote += "。settings で上書きしました: " + request.settings.dump();
		const ImportRound round = runImportRound(request.ifcPath, options, /*settingsShown*/ true,
												 settingsNote, preparation);
		// **描画結果は、成否にかかわらず保存して記録する**（SaveRoundDocument）。
		// ★保存の結果は**報告か結末の文言に載せる**——取り込みのログは runImportRound が
		// 閉じ終えているので、ここで trace へ書いても捨てられる（以前は保存できなかったことが
		// どこにも残らなかった）。
		const std::string saved = "後始末: " + SaveRoundDocument(session);
		const bool sessionWritten = core::writeFeedbackSession(sessionPath, session);
		if (round.failed)
		{
			// **取り込みの完了文言（round.body）は使わない**——このコマンド自身の言葉で言う
			// （parse/Feedback.h「実機テストの周の結末」）。報告は書かない周なので、保存の
			// 結果はここで添える。
			return Failure(parse::TestRoundOutcome::ImportFailed, saved);
		}

		// 報告を組む（無 SDK 側。parse/Feedback）。
		parse::FeedbackRound material;
		material.build = build;
		material.ifcPath = request.ifcPath;
		material.bytes = round.bytes;
		material.seconds = round.seconds;
		material.startedAt = round.startedAt;
		material.log = core::trace::text();
		material.preparation = preparation + (preparation.empty() ? "" : "\n") + saved;
		const std::string report =
			parse::formatTestRoundReport(material, round.document, round.counts);

		TestRoundResult result;
		result.ran = true;
		result.report = report;
		std::string detail = build.commit;
		const std::string reportPath = core::testReportPathFor(sessionPath);
		if (!sessionWritten)
			detail += "\n保存した図面の記録を書けませんでした（次の周でこの周の図面を閉じられず、"
					  "Vectorworks を再起動するときに保存の確認が出ることがあります）。";
		if (WriteReport(reportPath, report))
			result.reportPath = reportPath;
		else
			detail += "\n報告をファイルへ書けませんでした（" +
					  (reportPath.empty() ? std::string("置き場所が分かりません") : reportPath) +
					  "）。";
		result.message = parse::formatTestRoundResult(parse::TestRoundOutcome::Completed, detail);
		return result;
	}
} // namespace HomeskzIfcImport::draw
