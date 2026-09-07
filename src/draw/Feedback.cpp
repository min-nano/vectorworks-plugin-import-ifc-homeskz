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
#include "parse/Feedback.h"
#include "parse/Summary.h"

#include <chrono>
#include <cstddef>
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

		// 動いているビルドのブランチ（記憶がこのブランチのものかを見るのに使う）。
		std::string RunningBranch()
		{
			return VW_BUILD_BRANCH;
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

			// **1 行目は「いまどこにいるか」を言う。** 記憶が残っているのにここへ来たと
			// いうことは、追っているブランチに新しい dev ビルドがまだ出ていない、という
			// ことである（続きの周なら planFeedbackRound はそもそも尋ねない。
			// draw/ImportCommand.cpp）。黙ってファイル選択から始めると、人は「往復が
			// 壊れた」と読む。
			const bool waitingForNewBuild = remembered.send && remembered.round > 0;
			const std::string lead =
				waitingForNewBuild
					? ("`" + build.branch + "` に新しいビルドはまだ出ていません（前の周は round " +
					   std::to_string(remembered.round) + "）。新しく 1 周目として取り込みます。")
					: std::string("取り込みが終わったら、結果を PR へ自動で投稿します。");

			const int shownRound = waitingForNewBuild ? 1 : plan.session.round + 1;
			const std::string title =
				"実機フィードバック（round " + std::to_string(shownRound) + "）";
			CFeedbackDialog dialog(title, lead, plan.session);
			const bool accepted = dialog.RunDialogLayout("") == VWFC::VWUI::kDialogButton_Ok;
			if (!dialog.Shown())
				return FeedbackPlan{}; // ダイアログを組めなかった → 従来どおり取り込むだけ
			if (!accepted)
			{
				// **「送らない」が往復の終わり方である。** このダイアログが出ているのは
				// 「続きの周ではない」ときだけなので、ここで送らないと決めたのなら、
				// この人はもう往復を回していない——記憶を捨てておかないと、次に新しい
				// ビルドが出た日に、忘れたころの IFC が黙って取り込まれる。
				if (waitingForNewBuild)
					core::clearFeedbackSession(core::defaultFeedbackSessionPath());
				return FeedbackPlan{};
			}
			// **新しい 1 周目として数え直す。** 前の往復の続きではないので、round を
			// 引き継ぐと「前の周からの変化」が別の IFC との比較になりかねない。
			if (waitingForNewBuild)
			{
				plan.session.round = 0;
				plan.session.lastCommit.clear();
				plan.session.lastTally.clear();
				// 基準も採り直す。前の往復の図面と引き比べても意味が無い。
				plan.session.baselineRecorded = false;
				plan.session.baselineLayers.clear();
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

	bool postFeedbackRound(const FeedbackPlan& plan, const FeedbackInput& input, std::string& error)
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
		session.lastCommit = input.build.commit;
		session.lastTally = parse::formatTally(parse::elementRows(*input.document, *input.counts));
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

	// -----------------------------------------------------------------------
	// モードレスの往復（M24）が殻から尋ねてくるもの。

	std::string feedbackLoopStatus()
	{
		// 記憶（別ブランチのものは読まない。loadFeedbackSession）。
		const core::FeedbackSession session = loadFeedbackSession(RunningBranch());
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
		body += "以後、新しいビルドが出ても**自動では取り込みません**。続きが要るときは、"
				"利用者が Vectorworks で取り込みをもう一度実行します（同じ条件で round " +
				std::to_string(session.round + 1) + " として走り、往復もそこから回り直します）。\n";
		std::string url;
		std::string createdAt;
		std::string error;
		(void)PostComment(session.repo, session.pullRequest, body, url, createdAt, error);
	}
} // namespace HomeskzIfcImport::draw
