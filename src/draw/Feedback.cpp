//
//	draw/Feedback.cpp
//
//	実機フィードバックの往復の実装（意図と 1 周の形は draw/Feedback.h 参照）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）と VWFC のダイアログを include する。
//
//	使う SDK API はダイアログ 2 種（draw/ResultDialog と同じ作法。VWFC/VWUI/…）だけで、
//	**ネットワークには一切触れない**——
//	投稿もビルドの取得も同梱スクリプトが行い、こちらはその機械可読な出力を読む
//	（自動アップデートと同じ分担。src/Updater.h）。
//
//	【文字列の受け渡し】スクリプトの出力は `UpdaterParse` の純粋な関数で解く
//	（`ValueOf`）。**同じ解き方を 2 つ持たない**
//	ため、殻の自動アップデートが使っているものをそのまま使う
//	（CLAUDE.md「重複を作らない置き場所」）。
//
//	【待たないし、入れもしない】**新しいビルドをここで待ってはいけない。** 待つあいだ
//	モーダルのダイアログが Vectorworks を止め、その周の絵が見られなくなる——絵を見るために
//	回している往復で、それでは本末転倒である（実機 round 3 で判明。docs/DEV-NOTES.md M23）。
//	入れるのも殻の仕事で（src/UpdaterFlow.cpp）、本体のコードがスタックに載っている間は
//	本体を降ろせない以上どのみち殻へ返ってからにしかできない（src/PayloadSession.h）。
//	ここがするのは「投稿して、**次の取り込みでは尋ねずに入れてよい**と伝えて戻る」だけ。
//
//	【尋ねるのは取り込みの前だけ】ダイアログを出してよいのは planFeedbackRound（取り込みが
//	始まる前）だけで、postFeedbackRound（終わったあと）は**何も出さない**。取り込みは
//	1 分以上かかるので、終わったところに確認が待っていると席を離れられない
//	（docs/DEV-NOTES.md M23「取り込みのあとに操作を残さない」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/Feedback.h"

#include "UpdaterParse.h"
#include "core/Document.h"
#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"
#include "draw/DrawUtil.h"
#include "draw/HostServices.h"
#include "draw/ImportRun.h"
#include "draw/ResultDialog.h"
#include "draw/SettingsDialog.h"
#include "parse/Feedback.h"
#include "parse/Summary.h"

// 周ごとに図面を開き直す（ISDK::OpenDocumentPath）。パスは IFileIdentifier で渡す。
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace HomeskzIfcImport::UpdaterParse;

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 同梱スクリプトの名前（拡張子は殻が付ける。src/PayloadAbi.h）。
		constexpr const char* kFeedbackScript = "vw-feedback";

		// **実機テストの結果ダイアログのタイトル。** 本番の取り込み（「ホームズ君 IFC
		// 取り込み」。draw/ImportCommand.cpp）と**必ず違う名前にする**——同じにすると、
		// PR への投稿の顛末を本番の取り込みが言っているように見える（実機の指摘。M25）。
		constexpr const char* kTestResultTitle = "実機テスト (みんなの構造設計支援Dev)";

		// ダイアログのコントロール ID（1 = OK / 2 = キャンセルは SDK の予約）。
		// kNoteLabelID / kNoteID はトークンの貼り付けダイアログが使う。
		constexpr TControlID kNoteLabelID = 4;
		constexpr TControlID kNoteID = 5;
		constexpr TControlID kPrLabelID = 6;
		constexpr TControlID kPrID = 7;
		constexpr TControlID kAnonID = 9;
		constexpr TControlID kLeadID = 10;
		constexpr TControlID kSendsID = 11;
		constexpr TControlID kQuietID = 12;
		constexpr TControlID kNextID = 13;

		// トークンの貼り付け欄の幅（標準文字数）と、PR 番号の欄の幅。
		constexpr short kNoteWidthChars = 72;
		constexpr short kPrWidthChars = 10;

		// -------------------------------------------------------------------
		// 殻から借りた道具（draw/HostServices）。
		// -------------------------------------------------------------------

		bool RunScript(const char* baseName, const std::vector<std::string>& args, std::string& out)
		{
			out.clear();
			const HostServices& host = hostServices();
			if (!host.canRunScripts())
				return false;
			return host.runScript(baseName, args, out);
		}

		// -------------------------------------------------------------------
		// 一時ファイル（本文とトークンの受け渡し）。
		// -------------------------------------------------------------------

		// 一時ディレクトリに書き出して、そのパスを返す（書けなければ空）。
		// **本文を引数に乗せない**ため（コマンドラインは長さに限りがあり、プロセス一覧
		// からも見える）。tag は名前を分けるためのもの。
		std::string WriteTempFile(const std::string& tag, const std::string& contents,
								  bool ownerOnly)
		{
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
			if (ec)
				return "";
			const std::filesystem::path path =
				dir / ("homeskz-feedback-" + tag + "-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

			{
				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out)
					return "";
				out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
				if (!out.good())
					return "";
			}
			if (ownerOnly)
			{
				// **トークンを渡すファイルは本人しか読めなくする。** 一時ディレクトリは
				// 共有なので、既定の許可のまま置くと他のユーザーに読まれうる。
				std::filesystem::permissions(
					path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
					std::filesystem::perm_options::replace, ec);
			}
			return path.string();
		}

		void RemoveTempFile(const std::string& path)
		{
			if (path.empty())
				return;
			std::error_code ec;
			std::filesystem::remove(std::filesystem::path(path), ec);
		}

		// -------------------------------------------------------------------
		// トークンを 1 度だけ受け取るダイアログ。
		// -------------------------------------------------------------------

		class CTokenDialog : public VWDialog
		{
		public:
			CTokenDialog() : fLabel(kNoteLabelID), fToken(kNoteID) {}
			~CTokenDialog() override = default;

			const TXString& Token() const
			{
				return fTokenText;
			}

		protected:
			bool CreateDialogLayout() override
			{
				if (!this->CreateDialog("GitHub のトークン", "保存", "やめる", false))
					return false;
				if (!fLabel.CreateControl(
						this, "PR へ投稿するためのトークンを 1 度だけ登録します。\n"
							  "GitHub の Fine-grained token（対象リポジトリの Pull "
							  "requests: Read and write）を貼り付けてください。\n"
							  "保存先は macOS のキーチェーン / Windows の暗号化ファイルで、"
							  "図面にもログにも残りません。"))
					return false;
				if (!fToken.CreateControl(this, "", kNoteWidthChars, 1))
					return false;
				this->AddFirstGroupControl(&fLabel);
				this->AddBelowControl(&fLabel, &fToken);
				return true;
			}

			void OnDDXInitialize() override
			{
				this->AddDDX_EditText(kNoteID, &fTokenText);
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			VWStaticTextCtrl fLabel;
			VWEditTextCtrl fToken;
			TXString fTokenText;
		};

		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CTokenDialog);
		EVENT_DISPATCH_MAP_END;

		// **投稿できる状態にする。** トークンが無ければ 1 度だけ尋ねて保存する。
		// 使えるようになったら true。理由は note へ（呼び出し側が見せる）。
		bool EnsureToken(std::string& note)
		{
			std::string out;
			if (!RunScript(kFeedbackScript, {"token-status"}, out))
			{
				note = "フィードバック用のスクリプトを起動できませんでした。";
				return false;
			}
			if (ValueOf(out, "ok") == "yes")
				return true;

			CTokenDialog dialog;
			if (dialog.RunDialogLayout("") != VWFC::VWUI::kDialogButton_Ok)
			{
				note = "トークンの登録をやめました。";
				return false;
			}
			const std::string token = static_cast<const char*>(dialog.Token());
			if (token.empty())
			{
				note = "トークンが空でした。";
				return false;
			}

			// **引数に乗せずファイル経由で渡す**（プロセス一覧から秘密が見えないように）。
			// 読んだスクリプトがその場で消す約束で、こちらも念のため消す。
			const std::string file = WriteTempFile("token", token, /*ownerOnly*/ true);
			if (file.empty())
			{
				note = "トークンを一時ファイルへ書けませんでした。";
				return false;
			}
			const bool ran = RunScript(kFeedbackScript, {"login", file}, out);
			RemoveTempFile(file);
			if (!ran || Trim(out) != "ok")
			{
				const std::string reason = ValueOf(out, "error");
				note = reason.empty() ? "トークンを保存できませんでした。" : reason;
				return false;
			}
			return true;
		}

		// -------------------------------------------------------------------
		// フィードバックのダイアログ。**取り込みが始まる前に**、送るかどうか・宛先・
		// 伏せ字を 1 枚で決める。
		//
		// **結果はまだ無い。** 以前は取り込みのあとに結果を見せて「送りますか」と訊いて
		// いたが、取り込みは 1 分以上かかるので、終わったところに確認が待っていると席を
		// 離れられなかった（実機の指摘。docs/DEV-NOTES.md M23）。訊くことは全部前へ移し、
		// 終わったあとは投稿して黙る。
		// -------------------------------------------------------------------

		class CFeedbackDialog : public VWDialog
		{
		public:
			CFeedbackDialog(const std::string& title, const std::string& lead,
							const core::FeedbackSession& session)
				: fTitle(title.c_str()), fLeadText(lead.c_str()), fLead(kLeadID), fSends(kSendsID),
				  fQuiet(kQuietID), fNext(kNextID), fPrLabel(kPrLabelID), fPr(kPrID),
				  fAnon(kAnonID), fPrText(std::to_string(session.pullRequest).c_str()),
				  fAnonymize(session.anonymize)
			{
			}
			~CFeedbackDialog() override = default;

			bool Shown() const
			{
				return fShown;
			}
			std::string PullRequest() const
			{
				return static_cast<const char*>(fPrText);
			}
			bool Anonymize() const
			{
				return fAnonymize;
			}

		protected:
			bool CreateDialogLayout() override
			{
				// **ボタンは何が起きるかを名乗る。** ここで押した選択は取り込みの**あと**に
				// 効くので、「送る」だけでは何のことか分からない。
				if (!this->CreateDialog(fTitle, "取り込み結果を送る", "送らない", false))
					return false;

				if (!fLead.CreateControl(this, fLeadText))
					return false;
				this->AddFirstGroupControl(&fLead);
				if (!fSends.CreateControl(this,
										  "送るもの: 要素ごとの内訳・描画側の注意・診断ログ。"))
					return false;
				this->AddBelowControl(&fLead, &fSends);
				// **終わったあとは何も出ない**ことを、押す前に言い切る。席を離れてよいと
				// 分かることが、この往復では機能そのものである。
				if (!fQuiet.CreateControl(
						this, "投稿したあとは何も尋ねません（実行したら離れて構いません）。"))
					return false;
				this->AddBelowControl(&fSends, &fQuiet);
				// **次の周に人がすることも、ここで言い切る。** 2 周目以降はダイアログを
				// 1 枚も出さないので、頼めるのはこの 1 枚だけである（draw/Feedback.h
				// 「取り込み前へ戻すのは人の手仕事」）。
				if (!fNext.CreateControl(this, "次の周からは、実行する前に「取り消し」で図面を"
											   "取り込み前へ戻してください。"))
					return false;
				this->AddBelowControl(&fQuiet, &fNext);

				if (!fPrLabel.CreateControl(this, "送信先の PR 番号:"))
					return false;
				this->AddBelowControl(&fNext, &fPrLabel, 0, 1);
				if (!fPr.CreateControl(this, "", kPrWidthChars, 1))
					return false;
				this->AddRightControl(&fPrLabel, &fPr);

				if (!fAnon.CreateControl(this, "ファイル名とユーザー名を伏せて投稿する"))
					return false;
				this->AddBelowControl(&fPrLabel, &fAnon, 0, 1);
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				// **初期値は自分でも入れる。** DDX が流し込む前提には寄りかからない
				// （draw/SettingsDialog も同じく SetState を明示している）——ここが空だと
				// 「前回どおりでよい」ときに毎回打ち直すことになる。
				fPr.SetText(fPrText);
				fAnon.SetState(fAnonymize);
				fShown = true;
			}

			void OnDDXInitialize() override
			{
				this->AddDDX_EditText(kPrID, &fPrText);
				this->AddDDX_CheckButton(kAnonID, &fAnonymize);
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			TXString fTitle;
			TXString fLeadText;
			VWStaticTextCtrl fLead;
			VWStaticTextCtrl fSends;
			VWStaticTextCtrl fQuiet;
			VWStaticTextCtrl fNext;
			VWStaticTextCtrl fPrLabel;
			VWEditTextCtrl fPr;
			VWCheckButtonCtrl fAnon;
			TXString fPrText;
			bool fAnonymize = true;
			bool fShown = false;
		};

		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CFeedbackDialog);
		EVENT_DISPATCH_MAP_END;

		// -------------------------------------------------------------------
		// 宛先・投稿・待機。
		// -------------------------------------------------------------------

		// 10 進の PR 番号（数字以外・空は 0）。
		int ParsePullRequest(const std::string& text)
		{
			const std::string trimmed = Trim(text);
			if (trimmed.empty())
				return 0;
			int value = 0;
			for (const char c : trimmed)
			{
				if (c < '0' || c > '9')
					return 0;
				if (value > 214748363)
					return 0;
				value = value * 10 + (c - '0');
			}
			return value;
		}

		// ブランチから open な PR 番号を引く（引けなければ 0）。**人に番号を打たせない**
		// ための当て推量で、外れてもダイアログで直せる。
		int ResolvePullRequest(const std::string& repo, const std::string& branch)
		{
			if (branch.empty() || branch == "local")
				return 0;
			std::string out;
			if (!RunScript(kFeedbackScript, {"find-pr", repo, branch}, out))
				return 0;
			return ParsePullRequest(ValueOf(out, "pr"));
		}

		// 本文を投稿する。投稿できたら true で、url にコメントの在り処、createdAt に
		// GitHub が付けた時刻（ISO 8601）が入る（古いスクリプトは後者を出さないので空）。
		bool PostComment(const std::string& repo, int pullRequest, const std::string& body,
						 std::string& url, std::string& createdAt, std::string& error)
		{
			url.clear();
			createdAt.clear();
			error.clear();
			const std::string file = WriteTempFile("body", body, /*ownerOnly*/ false);
			if (file.empty())
			{
				error = "投稿する本文を一時ファイルへ書けませんでした。";
				return false;
			}
			std::string out;
			const bool ran =
				RunScript(kFeedbackScript, {"post", repo, std::to_string(pullRequest), file}, out);
			RemoveTempFile(file);
			if (!ran)
			{
				error = "フィードバック用のスクリプトを起動できませんでした。";
				return false;
			}
			const std::string reason = ValueOf(out, "error");
			if (!reason.empty())
			{
				error = reason;
				return false;
			}
			url = ValueOf(out, "url");
			createdAt = ValueOf(out, "created");
			return true;
		}

	} // namespace

	// -----------------------------------------------------------------------
	bool feedbackAvailable()
	{
#ifdef VW_DEV_BUILD
		// 殻がスクリプトを貸してくれていること（古い殻・単体テストでは貸されない）。
		return hostServices().canRunScripts();
#else
		// **安定版では動かさない。** 往復するのは PR のビルドであって、main の配布物では
		// ない——安定版から PR へコメントが飛ぶのは筋が通らないし、利用者の図面の情報が
		// 外へ出る経路を、開発用でないビルドに持たせない。
		return false;
#endif
	}

	// -----------------------------------------------------------------------
	// **ここから下は往復の内側。** M25 で公開をやめた（呼ぶのは runTestRound だけ）
	// ——本番の取り込みコマンドは往復を知らない（draw/Feedback.h）。

	namespace
	{
		// **取り込みの前に決めたこと。**
		struct FeedbackPlan
		{
			bool send = false;			   // 取り込みが終わったら投稿するか
			core::FeedbackSession session; // 送るときの宛先と記憶
		};

		// 1 周ぶんの材料（runTestRound が詰める）。
		struct FeedbackInput
		{
			const core::Document* document = nullptr;
			const core::DrawCounts* counts = nullptr;
			parse::BuildInfo build; // 動いていたビルド
			std::string ifcPath;
			unsigned long long bytes = 0;
			double seconds = 0.0;
			std::string startedAt;
			std::string log;		 // 診断ログ全文
			std::string preparation; // 取り込みの前に図面へ何をしたか（1 行）
		};

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
		std::string FreshTempPath(const std::string& stem)
		{
			for (int i = 1; i <= 100; ++i)
			{
				// **const にしない**——返すときに move されなくなり、clang-tidy の
				// performance-no-automatic-move がエラーになる（tidy-mac で実際に落ちた）。
				std::string path = TempPath(stem + "-" + std::to_string(i) + ".vwx");
				if (path.empty())
					return "";
				if (!PathExists(path))
					return path;
			}
			return "";
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

		// 作業ファイルを開き直す。note には診断ログと PR コメントへ出す 1 行が入る。
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
				std::string cleaned;
				if (!session.lastCreatedLayers.empty() || !session.lastCreatedSheets.empty())
					cleaned = prepareDrawingForRound(session) + "。";

				// **レイヤの基準も採り直す。** 別の図面で採った顔ぶれと引き比べても意味が
				// 無い（「基準に無いレイヤ」が出るだけで、読む側を惑わせる）。次の投稿が
				// この図面の顔ぶれを基準として採り直す。
				session.baselineRecorded = false;
				session.baselineLayers.clear();

				const std::string prefix = cleaned + "準備: " + rebased;
				const std::string work = FreshTempPath("homeskz-work");
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

			// **2 周目以降: 前の周の絵が載った文書を捨て場所へ移してから閉じる。**
			// 基準（作業ファイル）には前の周の絵が書き戻らない＝変更を破棄したのと同じ。
			std::string closed;
			const std::string active = ActiveDocumentPath();
			if (SamePath(active, session.workPath))
			{
				const std::string parked =
					FreshTempPath("homeskz-round-" + std::to_string(session.round));
				if (!SaveActiveDocumentAs(parked))
				{
					// 退避できないなら閉じない（未保存の文書は閉じられない。実機確認済み）。
					// いまの図面はまだ生きているので、従来のレイヤ削除へ回せる。
					note = "準備: 前の周の図面を退避できなかったので開き直しませんでした（" +
						   parked + "）。いま開いている図面へ描きます";
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
				note = "準備: 作業ファイルを開き直しました（" + session.workPath + "）。";
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
			// 指さない数字が PR に残る（実機 round 9）。**何が起きたかを証拠つきで残して
			// 周ごと中止する。**
			std::string why = "準備: 作業ファイルを開き直せなかったので、この周は走らせません";
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
				return "準備: 前の周が作ったレイヤの記録が無いので、図面はそのままにしました"
					   "（残っていれば、その上へ重ねて描きます）";
			std::string note;
			const std::size_t removed =
				RemoveCreatedLayers(session.lastCreatedLayers, session.lastCreatedSheets, note);
			if (removed == 0 && note.empty())
				return "準備: 前の周が作ったレイヤは 1 枚も残っていませんでした（図面はそのまま）";
			// **これは部分的な復元でしかない。** 取り込み前から在ったレイヤ（テンプレートの
			// もの）へ描いた分は、そのレイヤが自分の作ったものではないので取り除けない
			// ——上に描いた分だけが残る。**丸ごと戻す道は 3 つとも塞がっている**（SDK
			// リファレンス Findings「Undo」）: 閉じたイベントへ Undo は掛けられず（#23）、
			// 閉じずに返しても VW がコマンド完了時に代わりに閉じてしまい（#31）、メニューの
			// 「取り消し」を名前で起動する API も無い（#27）。ここを読む人が「戻り切った」と
			// 思わないよう、1 行で言い切っておく。
			return note + "。取り込み前から在ったレイヤへ描いた分は取り除けません";
		}

		// **毎周開き直す図面（テンプレート）を選ばせる。** 選ばなければ空のまま
		// （＝従来どおり、いま開いている図面へ描いて前の周が作ったレイヤだけを取り除く）。
		core::FeedbackSession loadFeedbackSession(const std::string& branch)
		{
			core::FeedbackSession session;
			if (!feedbackAvailable())
				return session;
			if (!core::readFeedbackSession(core::defaultFeedbackSessionPath(), session))
				return core::FeedbackSession{};

			// **別のブランチの記憶なら使わない。** 別ブランチのビルドに入れ替わったのなら、
			// それは前の往復の続きではなく、新しい往復の 1 周目である。
			if (!session.branch.empty() && !branch.empty() && session.branch != branch)
				return core::FeedbackSession{};
			return session;
		}

		FeedbackPlan planFeedbackRound(const core::FeedbackSession& remembered,
									   const parse::BuildInfo& build, bool continuing)
		{
			FeedbackPlan plan;
			if (!feedbackAvailable())
				return plan;

			plan.session = remembered;
			plan.session.branch = build.branch;

			if (continuing)
			{
				// **続きの周は何も尋ねない。** 宛先も伏せ字も 1 周目の選択のままで、ここで
				// 訊き直す理由が無い（訊けば、往復から人の操作を消した意味が無くなる）。
				plan.send = true;
			}
			else
			{
				// 宛先の PR。記憶が無ければブランチから引く（人に番号を打たせないため）。
				if (plan.session.pullRequest == 0)
					plan.session.pullRequest =
						ResolvePullRequest(plan.session.repo, plan.session.branch);

				// **ここへ来るのは 1 周目だけ。** 記憶があって同じビルドが動いているときは
				// 呼び出し側（runTestRound）が手前で折り返すので、「新しいビルドを待っている
				// あいだにもう一度実行した」という筋はここには来ない（M24 まではここで新しい
				// 1 周目として数え直していた。docs/DEV-NOTES.md M25）。
				const std::string lead = "取り込みが終わったら、結果を PR へ自動で投稿します。";
				const std::string title =
					"実機フィードバック（round " + std::to_string(plan.session.round + 1) + "）";
				CFeedbackDialog dialog(title, lead, plan.session);
				const bool accepted = dialog.RunDialogLayout("") == VWFC::VWUI::kDialogButton_Ok;
				if (!dialog.Shown())
					return FeedbackPlan{}; // ダイアログを組めなかった → 従来どおり取り込むだけ
				if (!accepted)
				{
					// **「送らない」が往復の終わり方である。** 前の往復の記憶がまだ残って
					// いるなら捨てておく——残しておくと、次に新しいビルドが出た日に、忘れた
					// ころの IFC が黙って取り込まれる。
					core::clearFeedbackSession(core::defaultFeedbackSessionPath());
					return FeedbackPlan{};
				}
				plan.session.anonymize = dialog.Anonymize();
				plan.session.pullRequest = ParsePullRequest(dialog.PullRequest());
				plan.send = true;
			}

			if (plan.session.pullRequest == 0)
			{
				// **ここで言えば、まだ取り込みは始まっていない。** 終わってから「宛先が
				// 分かりません」と言われても、その 1 分は取り返せない。
				gSDK->AlertInform("投稿先の PR が分かりません。",
								  "PR 番号を入れて、もう一度お試しください。\n"
								  "（今回は投稿せずに取り込みます）",
								  false);
				return FeedbackPlan{};
			}

			// **トークンもここで確保する。** 未登録なら 1 度だけ貼り付けを求める——これが
			// 取り込みのあとに出ては、無操作で終わるはずの取り込みに操作が 1 つ増える。
			std::string note;
			if (!EnsureToken(note))
			{
				gSDK->AlertInform("フィードバックを投稿できません。",
								  (note + "\n（今回は投稿せずに取り込みます）").c_str(), false);
				return FeedbackPlan{};
			}

			plan.session.send = true;
			return plan;
		}

		bool postFeedbackRound(const FeedbackPlan& plan, const FeedbackInput& input,
							   std::string& error)
		{
			error.clear();
			if (!plan.send || input.document == nullptr || input.counts == nullptr)
				return false;

			core::FeedbackSession session = plan.session;

			// 本文を組む（無 SDK 側。parse/Feedback）。
			parse::FeedbackRound round;
			round.build = input.build;
			round.ifcPath = input.ifcPath;
			round.bytes = input.bytes;
			round.seconds = input.seconds;
			round.startedAt = input.startedAt;
			round.log = input.log;
			round.round = session.round + 1;
			round.previousCommit = session.lastCommit;
			round.previousTally = session.lastTally;
			// 1 周目に採った基準（＝取り込み前に在ったレイヤの顔ぶれ）。次の周はここへ
			// 戻っているかを引き比べる（parse/Feedback の restoredStateLine）。
			round.baselineKnown = session.baselineRecorded;
			round.baselineLayers = session.baselineLayers;
			round.preparation = input.preparation;
			round.anonymize = session.anonymize;

			const std::string commentBody =
				parse::formatFeedbackComment(round, *input.document, *input.counts);

			std::string url;
			std::string createdAt;
			if (!PostComment(session.repo, session.pullRequest, commentBody, url, createdAt, error))
				return false; // **ここでアラートを出さない**（呼び出し側が結果へ添える）

			// **投稿できたところで記憶を進める。** 投稿できていない周を数えると、次の
			// コメントが「前の周からの変化」を持たないまま round だけ進む。
			session.round = round.round;
			// **基準は 1 周目に採る。** 以後の周では触らない——基準そのものが周ごとに動くと、
			// 「戻っているか」を引き比べる相手が消える。
			if (!session.baselineRecorded)
			{
				session.baselineRecorded = true;
				session.baselineLayers = input.counts->existingLayers;
			}
			// **キャンセルされた周は、そのビルドを「試し終えた」ことにしない。** ここで
			// lastCommit を進めると、同じビルドをもう一度実行しても
			// `feedbackRoundKind` が RearmOnly を返して取り込みが走らず、**押し間違えた
			// 一度きりで往復が再開できなくなる**（実機 round 10 で発生）。投稿はする
			// （途中までの数字にも意味がある）が、記憶の上では走っていない扱いにして、
			// 同じビルドでの取り直しを許す。
			if (!input.counts->cancelled)
				session.lastCommit = input.build.commit;
			// **次の周の前に取り除く顔ぶれ。** この周が自分で作ったレイヤだけを名指しで
			// 持つ（prepareDrawingForRound）。前の周の分は用済みなので置き換える。
			session.lastCreatedLayers = input.counts->createdLayers;
			session.lastCreatedSheets = input.counts->createdSheets;
			session.lastTally =
				parse::formatTally(parse::elementRows(*input.document, *input.counts));
			// **自動の往復（M24）はここで回り出す。** 殻のパレットは記憶の loop を見て周期的に
			// 新しいビルドを確かめ、Claude の合図（control=stop）で止まる（src/FeedbackLoop.h）。
			// 投稿の時刻は「自分の投稿より後の合図だけ」を読むための since になる。
			session.loop = true;
			if (!createdAt.empty())
				session.lastPostedAt = createdAt;
			if (!core::writeFeedbackSession(core::defaultFeedbackSessionPath(), session))
			{
				// 投稿はできている。次の周がファイル選択から始まるだけなので、**結果へ添えて
				// 伝える**（ここでアラートを出すと、無操作で終わるはずの最後に操作が増える）。
				error = "投稿しましたが、次の周のための記憶を保存できませんでした"
						"（次の取り込みはファイル選択から始まります）。";
				return false;
			}

			// **次の取り込みでは尋ねずに入れてよい。** この人はいま往復の最中にいるので、
			// 次に取り込みを実行するときには「新しいビルドがあります。インストールします
			// か？」を挟まない（src/Extensions/ExtMenu.cpp が UpdateCheckKind::Auto を選ぶ）。
			return true;
		}

	} // namespace

	// -----------------------------------------------------------------------
	// **実機テストの 1 周**（M25。draw/Feedback.h）。**往復を知っているのはここだけ。**
	bool runTestRound(bool allowDialogs, bool& active)
	{
		active = false;
		if (!feedbackAvailable())
		{
			// 安定版、または殻がスクリプトを貸してくれていない（古い殻）。押した人には
			// 言う——黙って何も起きないと「壊れている」と読まれる。
			if (allowDialogs)
				gSDK->AlertInform("実機テストは開発版でのみ使えます。",
								  "開発版（Dev）のビルドで、同梱スクリプトが揃っている"
								  "ときだけ動きます。",
								  false);
			return false;
		}

		const parse::BuildInfo build = currentBuildInfo();
		core::FeedbackSession session = loadFeedbackSession(build.branch);

		// **手動で押したときは、いま開いている図面を基準として採り直す。** 人が別の図面
		// （空のテンプレート等）を開いてからメニューを押したのは「この図面で試したい」
		// という意思なのに、覚えた作業ファイルを開き直すとその意思が黙って消える——実機で
		// 二度、空のテンプレートで試そうとして前の周の作業ファイルに上書きされた。**記憶
		// ごと消さない**（往復の宛先・IFC・設定はそのまま）——捨てるのは「どの図面から
		// 始めるか」だけである。自動の周（パレット）はここへ来ない: そちらは前の周の続き
		// なので、開いている図面が何であっても作業ファイルへ戻すのが正しい。
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
				// **すぐ書き戻す。** 同じビルドで押した周（RearmOnly）はここで戻るので、
				// 書かないと次の自動の周がまた古い作業ファイルを開いてしまう。
				(void)core::writeFeedbackSession(core::defaultFeedbackSessionPath(), session);
			}
		}

		// **どの周になるかは無 SDK 側が決める**（core/FeedbackSession.h。M25 の要点なので
		// 場合分けを描画側に散らさず、1 か所でテストできる形にしてある）。
		const core::FeedbackRoundKind kind =
			core::feedbackRoundKind(session, build.commit, allowDialogs);
		if (kind == core::FeedbackRoundKind::Refuse)
			return false; // パレットがここへ来るのは新しいビルドを入れた直後だけ

		// **同じビルドでは取り込まない。** 前の周と同じ数字が並ぶだけなので、1 分を
		// かける意味が無い——往復を回す（止まっていたら回し直す）だけにする。パレットが
		// 開いている最中に人がメニューを押しても、round が二重に投稿されない（実機で
		// 起きた。docs/DEV-NOTES.md M25）。
		if (kind == core::FeedbackRoundKind::RearmOnly)
		{
			if (!session.loop)
			{
				session.loop = true;
				(void)core::writeFeedbackSession(core::defaultFeedbackSessionPath(), session);
			}
			active = true;
			return true;
		}

		const bool continuing = kind == core::FeedbackRoundKind::ContinueRound;
		std::string ifcPath = session.ifcPath;
		core::ImportOptions options = session.options;
		bool settingsShown = true;
		std::string settingsNote;
		if (continuing)
		{
			// **続きの周は何も出さない。** 1 周目の選択（ファイル・設定）をそのまま使う
			// ——ここで人の操作を挟むと、往復を自動にした意味が無くなる。
			settingsNote = "前の周の設定をそのまま使いました（実機フィードバックの往復）";
		}
		else
		{
			if (!chooseIfcFile(ifcPath))
				return false;
			options = core::ImportOptions{};
			const draw::SettingsOutcome settings = draw::showImportSettings(options, &settingsNote);
			if (settings == draw::SettingsOutcome::Cancelled)
				return false;
			settingsShown = settings == draw::SettingsOutcome::Accepted;
		}

		// **尋ねることは全部、取り込みが始まる前に尋ね切る**（draw/Feedback.h）。
		FeedbackPlan plan = planFeedbackRound(session, build, continuing);
		if (!plan.send)
			return false; // 送らないなら、この周は走らせる意味が無い
		plan.session.ifcPath = ifcPath;
		plan.session.options = options;

		// **取り除きは 1 回だけ呼び、その説明を 2 か所へ配る**——診断ログ（prologue）と
		// PR コメント（FeedbackInput::preparation）。ログは上限で切り詰められるので、
		// コメント側にも置かないと読めない周が出る（実機 round 2 で実際に落ちた）。
		// **まず作業ファイルを開き直す。** 開き直せたなら消すものは何も無い（レイヤも
		// クラスもシンボル定義も、その図面には前の周の痕跡が 1 つも無い）。用意できな
		// かった周だけ、従来どおり「前の周が作ったレイヤ」を取り除く。
		std::string preparation;
		const RoundDocument document = openRoundDocument(plan.session, rebased, preparation);
		if (document == RoundDocument::Abort)
		{
			// **描く先が無いなら取り込まない。** 記憶（作業ファイルの場所）は残すので、
			// 人がその図面を開いてからもう一度実行すれば続きの周として走る。
			(void)core::writeFeedbackSession(core::defaultFeedbackSessionPath(), plan.session);
			core::trace::note(preparation);
			(void)draw::showImportResult(
				kTestResultTitle,
				parse::formatTestRoundResult(parse::TestRoundOutcome::DocumentFailed, preparation),
				core::trace::text());
			return false;
		}
		if (document == RoundDocument::Fallback)
			preparation += "。" + prepareDrawingForRound(plan.session);
		const ImportRound round =
			runImportRound(ifcPath, options, settingsShown, settingsNote, preparation);
		if (round.failed)
		{
			// 送るべき内訳がそもそも無い。**取り込みの完了文言（round.body）は使わない**
			// ——このコマンド自身の言葉で言う（parse/Feedback.h「実機テストの周の結末」）。
			(void)draw::showImportResult(
				kTestResultTitle,
				parse::formatTestRoundResult(parse::TestRoundOutcome::ImportFailed, {}),
				core::trace::text());
			return false;
		}

		FeedbackInput input;
		input.document = &round.document;
		input.counts = &round.counts;
		input.build = build;
		input.ifcPath = ifcPath;
		input.bytes = round.bytes;
		input.seconds = round.seconds;
		input.startedAt = round.startedAt;
		input.log = core::trace::text();
		input.preparation = preparation;

		std::string postError;
		if (postFeedbackRound(plan, input, postError))
		{
			// **投稿できたら何も出さない。** 内訳もログも PR にあるので、ここにボタンが
			// 1 つでも残ると「実行して離れる」が成立しない。
			active = true;
			return true;
		}

		// **投稿できなかったときだけ出す。** 数字が PR に載らないので、その代わりを
		// ここで見せる。
		//
		// **取り込みの完了文言（round.body）を借りて後ろへ PR の話を足さない。** 以前は
		// そうしていたが、押した人には**本番の取り込みが PR へ投稿しているように見える**
		// ——コマンドを分けた意味が見た目の上で崩れる（実機の指摘。M25）。文言はこの
		// コマンド自身のもの（parse/Feedback.h「実機テストの周の結末」）を使う。
		(void)draw::showImportResult(
			kTestResultTitle,
			parse::formatTestRoundResult(parse::TestRoundOutcome::PostFailed, postError),
			core::trace::text());
		return false;
	}

	// -----------------------------------------------------------------------
	// モードレスの往復（M24）が殻から尋ねてくるもの。

	std::string feedbackLoopStatus()
	{
		// 記憶（別ブランチのものは読まない。loadFeedbackSession）。動いているビルドの
		// **素性を作るのは draw/ImportRun ただ 1 か所**——同じ定数をここでも綴らない。
		const core::FeedbackSession session = loadFeedbackSession(currentBuildInfo().branch);
		const bool active = feedbackAvailable() && session.send && session.round > 0 &&
							session.loop && !session.ifcPath.empty();
		std::string out;
		out += std::string("active=") + (active ? "1" : "0") + "\n";
		out += "repo=" + session.repo + "\n";
		out += "pr=" + std::to_string(session.pullRequest) + "\n";
		out += "branch=" + session.branch + "\n";
		out += "round=" + std::to_string(session.round) + "\n";
		out += "build=" + session.lastCommit + "\n";
		out += "posted=" + session.lastPostedAt + "\n";
		return out;
	}

	void endFeedbackLoop(const std::string& reason, bool notifyPr)
	{
		const std::string path = core::defaultFeedbackSessionPath();
		core::FeedbackSession session;
		if (!core::readFeedbackSession(path, session))
			return; // 記憶が無い＝止めるものが無い
		if (!session.loop)
			return; // 既に下りている（二重に投稿しない）
		session.loop = false;
		(void)core::writeFeedbackSession(path, session);

		if (!notifyPr || !feedbackAvailable() || session.pullRequest <= 0)
			return;

		// **読む側（Claude）へ「もう自動の周は来ない」と伝える。** 目印は control=ended
		// ——同梱スクリプトの loop-control は `control=stop` の直後が空白か `-->` のものしか
		// 合図と読まないので、これを「止めろ」と取り違えることは無い。
		std::string body;
		body += "<!-- homeskz-ifc-feedback v1 control=ended build=" + session.lastCommit +
				" round=" + std::to_string(session.round) + " -->\n";
		body += "## 実機フィードバック — 自動の往復を終えました\n\n";
		body += reason + "。\n\n";
		body +=
			"以後、新しいビルドが出ても**自動では取り込みません**。続きが要るときは、"
			"利用者が Vectorworks で「実機テストを実行…」をもう一度実行します（同じ条件で round " +
			std::to_string(session.round + 1) + " として走り、往復もそこから回り直します）。\n";
		std::string url;
		std::string createdAt;
		std::string error;
		(void)PostComment(session.repo, session.pullRequest, body, url, createdAt, error);
	}
} // namespace HomeskzIfcImport::draw
