//
//	draw/SettingsDialog.cpp
//
//	取り込み設定ダイアログの実装（意図と規約は draw/SettingsDialog.h）。【SDK 依存】
//	PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	使う SDK API は VWFC のレイアウトダイアログ（draw/ResultDialog と同じ作法）と、
//	リソース一覧・サムネイル付きメニュー／シンボル表示:
//
//	  * VWResourceList::BuildList(kSymDefNode, sort) … 図面のシンボル定義の一覧
//	    （kSymDefNode = 16。Kernel/API/Objs.TDType.h）
//	  * VWResourceList::GetResourceName(i, name)     … その名前（UTF-8 の TXString）
//	  * VWCheckButtonCtrl                            … その要素を取り込むか
//	  * VWImagePopupCtrl::AddItem(resourceList, i)   … サムネイル付きの項目（本命の形）
//	  * VWPullDownMenuCtrl + VWSymbolDisplayCtrl     … 名前で選び、絵は隣に出す（退避の形）
//	  * AddRightControl / AddBelowControl            … 行の並べ方
//	  * VWDialog::EnableControl(id, bool)            … チェックを外した行を灰色にする
//
//	【2 つの形を持ち、出せた方を使う】シンボルは絵で選びたいので、**まず VectorWorks 自身の
//	「鋼材断面を選択」などと同じサムネイル付きポップアップ**（VWImagePopupCtrl）で組む。
//	ただしこのコントロールはレイアウトダイアログでの実績が薄く、**組めなかった／項目を
//	入れられなかったときに黙って設定ダイアログごと出ないのが最悪**——実機で実際にそうなった
//	（cd8a415 の実測）。そこで組み立てに失敗したら、**名前のプルダウン＋シンボル表示
//	コントロール**という確実に出る形へ切り替えて開き直す。どちらの形で出したか（と、
//	切り替えた理由）は取り込みログに残るので、原因が追える。
//
//	【絵の出し方（退避の形）】シンボル表示コントロールへ渡す描画モードとビューは、
//	VectorWorks 自身がシンボルのサムネイルに使う既定値と同じ **Top/Plan（view = 2）＋
//	ワイヤーフレーム（renderMode = 0）**（Kernel/API/MiniCadCallBacks.h の SymbolImgInfo の
//	既定構築子）。**3D の標準ビュー（standardViewTop = 7）ではない**——伏図記号のような
//	2D 部品だけのシンボルは 3D ビューでは何も映らない。
//
//	【項目は図面のシンボル定義そのもの】どちらの形でも候補は図面に実在するシンボルだけ。
//	行ごとの「取り込む」チェックがあるのでそれで足りる——置くものが図面に無いなら、その要素は
//	チェックを外せばよい（core/ImportOptions.h、docs/DEV-NOTES.md「取り込み設定の決め事」）。
//
//	【リソース一覧はダイアログが持ち続ける】サムネイルの項目はリソース一覧を指しているので、
//	一覧を先に捨てると絵が引けなくなる。ダイアログのメンバとして生存させる
//	（VWResourceList は参照カウント付きでコピーできる）。
//
//	【選択を読む時機】画像ポップアップの選択は DDX で受けられないので、OnDefaultButtonEvent
//	（＝OK が押された瞬間）にコントロールから読む。**閉じた後では読めない**。名前の
//	プルダウンは AddDDX_PulldownMenu で受ける。
//

#include "PluginPrefix.h"
#include "draw/SettingsDialog.h"
#include "core/ImportOptions.h"

#include "VWFC/Tools/VWResourceList.h"

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// コントロール ID。1 = OK / 2 = キャンセルは SDK の予約。行 i は
		// [チェック, 説明, 選択, 絵] の 4 つを kFirstRowID から 4 つ刻みで使う
		// （絵は退避の形でだけ作る。ID は形に依らず固定にしておく）。
		constexpr TControlID kIntroID = 3;
		constexpr TControlID kFirstRowID = 10;
		constexpr TControlID kRowStride = 4;

		constexpr TControlID checkID(std::size_t row)
		{
			return static_cast<TControlID>(kFirstRowID + (row * kRowStride));
		}
		constexpr TControlID labelID(std::size_t row)
		{
			return static_cast<TControlID>(checkID(row) + 1);
		}
		constexpr TControlID popupID(std::size_t row)
		{
			return static_cast<TControlID>(checkID(row) + 2);
		}
		constexpr TControlID previewID(std::size_t row)
		{
			return static_cast<TControlID>(checkID(row) + 3);
		}

		// 説明は**幅を固定して**左の列を揃える（可変幅だと選択コントロールの左端が行ごとに
		// ずれる。チェックは文字を持たないので幅が揃う）。
		constexpr short kLabelWidthChars = 22;
		constexpr short kPopupWidthChars = 30;
		constexpr short kPreviewSizePixels = 56;
		constexpr short kPreviewMarginPixels = 2;

		// 退避の形で使うシンボルの絵の出し方（冒頭「絵の出し方（退避の形）」）。
		constexpr TRenderMode kPreviewRenderMode = 0; // ワイヤーフレーム
		constexpr TStandardView kPreviewView = 2;	  // Top/Plan

		// ダイアログの形。**本命はサムネイル、退避は名前＋絵**（冒頭「2 つの形を持ち…」）。
		enum class Form
		{
			ImagePopup,
			NameList,
		};

		// 図面のシンボル定義の一覧と、その並びのままの名前。**名前は添字で項目と対応する**
		// ——ポップアップの項目はこの一覧の順に足すので、選択された項目の添字がそのまま
		// 名前の添字になる。
		struct SymbolResources
		{
			VWFC::Tools::VWResourceList list;
			std::vector<std::string> names;
		};

		// いま開いている図面のシンボル定義（名前順）。読めなければ空（＝どの要素も
		// 取り込めない。ダイアログ自体は出せる）。
		SymbolResources CollectSymbolResources()
		{
			SymbolResources resources;
			try
			{
				const std::size_t count = resources.list.BuildList(kSymDefNode, true);
				resources.names.reserve(count);
				for (std::size_t i = 0; i < count; ++i)
				{
					TXString name;
					resources.list.GetResourceName(i, name);
					// 名前が取れなくても**枠は詰めない**（項目の添字と名前の添字が
					// ずれると、選んだ絵と置かれるシンボルが食い違う）。
					resources.names.emplace_back(static_cast<const char*>(name));
				}
			}
			catch (...)
			{
				// リソース一覧を作れない図面でも設定ダイアログ自体は出す（候補が空になり、
				// どの要素にもチェックが入らない）。1 つの失敗で取り込みの入口を塞がない。
				// **途中まで採れていた名前は捨てる**——半端な一覧は項目と対応しない。
				resources.names.clear();
			}
			return resources;
		}

		// 役割の並びは表の順（core::symbolRoles()）。行番号 → 役割。
		core::SymbolRole roleAt(std::size_t row)
		{
			return core::symbolRoles()[row].role;
		}

		// 取り込み設定ダイアログ 1 枚。行は役割の数だけで、増減は core/ImportOptions.h の
		// 表に従う（**イベントマップだけはコンパイル時の ID が要る**ので、下の
		// static_assert が「表を増やしたらここも増やせ」と教える）。
		class CImportSettingsDialog : public VWDialog
		{
		public:
			CImportSettingsDialog(const core::ImportOptions& seed, const SymbolResources& resources,
								  Form form)
				: fIntro(kIntroID), fResources(resources), fForm(form)
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					fChecks.emplace_back(checkID(row));
					fLabels.emplace_back(labelID(row));
					if (fForm == Form::ImagePopup)
						fImagePopups.emplace_back(popupID(row));
					else
					{
						fPopups.emplace_back(popupID(row));
						fPreviews.emplace_back(previewID(row));
					}

					// **いまの対応先が図面にある役割だけを「取り込む」で開く。** 無い名前は
					// 項目にできない（＝置きようがない）ので、チェックを外した状態にする。
					const std::size_t index = IndexOf(seed.symbol(roleAt(row)));
					fSelection[row] = index < fResources.names.size() ? index : 0;
					fEnabled[row] = seed.isEnabled(roleAt(row)) && index < fResources.names.size();
				}
			}
			~CImportSettingsDialog() override = default;

			// **実際に出せたか。** 組めなかったときは呼び出し側が次の形（または
			// 「既定のまま取り込む」）へ落とす（draw/SettingsDialog.h）。
			bool Shown() const
			{
				return fShown;
			}

			// この形では出せないと分かった（＝別の形で開き直してほしい）。
			bool Failed() const
			{
				return !fShown || fAborted;
			}

			// 何が起きたか（ログへ出す 1 行ぶん。問題が無ければ空）。
			const std::string& Note() const
			{
				return fNote;
			}

			// 選ばれた対応。チェックの無い役割は「取り込まない」で、名前は既定のまま
			// （名前は使われない。core/ImportOptions.h）。
			core::ImportOptions Result() const
			{
				core::ImportOptions options;
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					const std::size_t index = fSelection[row];
					const bool valid = index < fResources.names.size();
					options.setEnabled(roleAt(row), fEnabled[row] && valid);
					if (valid)
						options.setSymbol(roleAt(row), fResources.names[index]);
				}
				return options;
			}

		protected:
			bool CreateDialogLayout() override
			{
				// hasHelp = false。OK のボタン名は行き先（取り込み）そのものにする。
				if (!this->CreateDialog("ホームズ君 IFC 取り込みの設定", "取り込む", "キャンセル",
										false))
				{
					fNote = "ダイアログの枠を作れませんでした";
					return false;
				}
				// 図面にシンボルが 1 つも無いなら、選ばせる前にそう言う。
				const TXString intro =
					fResources.names.empty()
						? "この図面にはシンボルが登録されていないため、シンボルで置く要素は"
						  "取り込めません。"
						: "取り込む要素にチェックを入れ、置くシンボルを選んでください。";
				if (!fIntro.CreateControl(this, intro))
				{
					fNote = "説明文を作れませんでした";
					return false;
				}
				this->AddFirstGroupControl(&fIntro);

				VWControl* previousRow = &fIntro;
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					// チェックは文字を持たない（役割の名前は隣の説明が出す）——文字を
					// 持たせると幅が行ごとに変わり、右の列が揃わない。
					if (!fChecks[row].CreateControl(this, ""))
					{
						fNote = "チェックを作れませんでした";
						return false;
					}
					if (!fLabels[row].CreateControl(this, core::symbolRoleLabel(roleAt(row)),
													kLabelWidthChars))
					{
						fNote = "説明を作れませんでした";
						return false;
					}
					if (!CreateSelector(row))
						return false;

					// 行の頭（チェック）は前の行の頭の下、残りはその右へ。行間を空けるのは
					// 説明文の下（＝最初の行の上）だけ——絵が文字より背が高いぶん、行そのものは
					// 詰めても窮屈にならない。
					this->AddBelowControl(previousRow, &fChecks[row], 0, row == 0 ? 1 : 0);
					this->AddRightControl(&fChecks[row], &fLabels[row]);
					if (fForm == Form::ImagePopup)
						this->AddRightControl(&fLabels[row], &fImagePopups[row]);
					else
					{
						this->AddRightControl(&fLabels[row], &fPopups[row]);
						this->AddRightControl(&fPopups[row], &fPreviews[row]);
					}
					previousRow = &fChecks[row];
				}
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				// **中身を入れる前に「出た」ことにする。** ここから先で例外が出ても、
				// 呼び出し側は「組めなかった」ではなく「この形では駄目だった」と分かる。
				fShown = true;
				try
				{
					for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
					{
						FillSelector(row);
						fChecks[row].SetState(fEnabled[row]);
						UpdateRow(row);
					}
				}
				catch (...)
				{
					// 項目を入れられなかった（サムネイル側で起きうる）。**この形は諦めて
					// 開き直してもらう**——中身の無いダイアログを見せない。
					fAborted = true;
					fNote = "選択肢を入れられませんでした";
					this->SetDialogClose(false); // キャンセル扱いで閉じる
				}
			}

			// チェックは DDX で受ける。名前のプルダウンも DDX で受けられる（サムネイルの
			// 選択だけは OnDefaultButtonEvent で読む。冒頭「選択を読む時機」）。
			void OnDDXInitialize() override
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					this->AddDDX_CheckButton(checkID(row), &fEnabled[row]);
					if (fForm == Form::NameList)
						this->AddDDX_PulldownMenu(popupID(row), &fSelection[row]);
				}
			}

			// OK が押された。**閉じる前に**サムネイルの選択を控える（閉じた後のコントロール
			// からは読めない）。
			void OnDefaultButtonEvent() override
			{
				if (fForm == Form::ImagePopup)
				{
					try
					{
						for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
							fSelection[row] = fImagePopups[row].GetSelectedItemIndex();
					}
					catch (...)
					{
						// 読めなければ初期値（開いたときの選択）のまま確定する。
						fNote = "選択を読み取れませんでした（開いたときの選択で取り込みます）";
					}
				}
				VWDialog::OnDefaultButtonEvent();
			}

			// プルダウン（退避の形）が動いたら**その行の絵**を差し替える。DDX は OK のときに
			// しか流れないので、いまの選択はコントロールから直接読む。
			void OnSymbolChanged(TControlID controlID, VWDialogEventArgs& /*eventArgs*/)
			{
				if (fForm != Form::NameList)
					return; // サムネイルの形では、この ID のコントロールは別物
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					if (popupID(row) != controlID)
						continue;
					fSelection[row] = fPopups[row].GetSelectedIndex();
					UpdateRow(row);
					return;
				}
			}

			// チェックが変わったら、その行の選択肢を有効／無効にする（取り込まない行が
			// 見て分かるように）。
			void OnEnabledChanged(TControlID controlID, VWDialogEventArgs& /*eventArgs*/)
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					if (checkID(row) != controlID)
						continue;
					fEnabled[row] = fChecks[row].GetState();
					UpdateRow(row);
					return;
				}
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			// 行の選択コントロールを作る（形で中身が変わる唯一の場所）。
			bool CreateSelector(std::size_t row)
			{
				if (fForm == Form::ImagePopup)
				{
					if (fImagePopups[row].CreateControl(this))
						return true;
					fNote = "サムネイルの選択肢を作れませんでした";
					return false;
				}
				if (!fPopups[row].CreateControl(this, kPopupWidthChars))
				{
					fNote = "選択肢を作れませんでした";
					return false;
				}
				if (!fPreviews[row].CreateControl(this, kPreviewSizePixels, kPreviewSizePixels,
												  kPreviewMarginPixels))
				{
					fNote = "絵を作れませんでした";
					return false;
				}
				return true;
			}

			// 行の選択コントロールへ候補を流し込む（**項目はリソース一覧の順**——項目の
			// 添字と名前の添字を一致させておくと、選択をそのまま名前へ引き直せる）。
			void FillSelector(std::size_t row)
			{
				const bool valid = fSelection[row] < fResources.names.size();
				if (fForm == Form::ImagePopup)
				{
					VWImagePopupCtrl& popup = fImagePopups[row];
					popup.ShowImage(true); // 閉じているときも選択中の絵を出す
					for (std::size_t i = 0; i < fResources.names.size(); ++i)
						popup.AddItem(fResources.list, i);
					if (valid)
						popup.SetSelectedItemIndex(fSelection[row]);
					return;
				}
				VWPullDownMenuCtrl& popup = fPopups[row];
				for (const std::string& name : fResources.names)
					popup.AddItem(TXString(name.c_str()));
				if (valid)
					popup.SelectIndex(fSelection[row]);
			}

			// 名前 → 項目の添字。無ければ項目の数（＝範囲外）を返す。
			std::size_t IndexOf(const std::string& value) const
			{
				for (std::size_t i = 0; i < fResources.names.size(); ++i)
					if (fResources.names[i] == value)
						return i;
				return fResources.names.size();
			}

			// その行の見た目を今の状態に合わせる。**選ぶものが無い行は常に無効**——選べる
			// ものが無いのにチェックできると、「取り込むと言ったのに何も置かれない」ことになる。
			void UpdateRow(std::size_t row)
			{
				const bool hasItems = !fResources.names.empty();
				this->EnableControl(checkID(row), hasItems);
				this->EnableControl(popupID(row), hasItems && fEnabled[row]);
				if (fForm != Form::NameList)
					return;
				// 退避の形だけは絵が別のコントロールなので、選択に追随させる。
				const std::size_t index = fSelection[row];
				const TXString name = index < fResources.names.size()
										  ? TXString(fResources.names[index].c_str())
										  : TXString("");
				fPreviews[row].Update(name, kPreviewRenderMode, kPreviewView);
				this->EnableControl(previewID(row), hasItems && fEnabled[row]);
			}

			VWStaticTextCtrl fIntro;
			// **deque に直接作る。** 行数ぶんのコントロールを溜めるが、vector だと追加の
			// たびに既存の要素が動いてしまう（ダイアログは生存中ずっとコントロールの
			// アドレスを持つ）。deque は追加しても既存の要素を動かさない
			// （draw/ResultDialog.cpp の本文行と同じ理由）。
			std::deque<VWCheckButtonCtrl> fChecks;
			std::deque<VWStaticTextCtrl> fLabels;
			std::deque<VWImagePopupCtrl> fImagePopups; // Form::ImagePopup のときだけ
			std::deque<VWPullDownMenuCtrl> fPopups;	   // Form::NameList のときだけ
			std::deque<VWSymbolDisplayCtrl> fPreviews; // 同上
			SymbolResources fResources; // 項目の元（ダイアログより長生きさせない）
			Form fForm = Form::ImagePopup;
			std::array<std::size_t, core::kSymbolRoleCount> fSelection = {};
			std::array<bool, core::kSymbolRoleCount> fEnabled = {};
			bool fShown = false;
			bool fAborted = false;
			std::string fNote;
		};

		// 役割を 1 つ足したら、下のイベントマップにも 2 行足すこと（コントロールの ID は
		// コンパイル時の定数でなければならないので、ここだけは表から回せない）。
		static_assert(core::kSymbolRoleCount == 7,
					  "役割を増減したら CImportSettingsDialog のイベントマップも直すこと");

		// EVENT_DISPATCH_MAP_BEGIN は SDK のマクロで、その展開が misc-const-correctness に
		// 引っかかる（マクロ側のコードでこちらの落ち度ではない。draw/ResultDialog.cpp と同じ）。
		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CImportSettingsDialog);
		ADD_DISPATCH_EVENT(checkID(0), OnEnabledChanged);
		ADD_DISPATCH_EVENT(checkID(1), OnEnabledChanged);
		ADD_DISPATCH_EVENT(checkID(2), OnEnabledChanged);
		ADD_DISPATCH_EVENT(checkID(3), OnEnabledChanged);
		ADD_DISPATCH_EVENT(checkID(4), OnEnabledChanged);
		ADD_DISPATCH_EVENT(checkID(5), OnEnabledChanged);
		ADD_DISPATCH_EVENT(checkID(6), OnEnabledChanged);
		ADD_DISPATCH_EVENT(popupID(0), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(1), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(2), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(3), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(4), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(5), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(6), OnSymbolChanged);
		EVENT_DISPATCH_MAP_END;

		// 前回の選択（この VectorWorks を起動している間だけ覚えている）。初回は役割の表の
		// 既定名＋全要素を取り込む＝従来と同じ対応。**図面には何も書かない**——名前付き
		// リソースを増やさないのと同じで、取り込みの設定を図面へ書き戻すことはしない
		// （CLAUDE.md「開発の基本方針」4）。
		core::ImportOptions& RememberedOptions()
		{
			static core::ImportOptions options;
			return options;
		}

		// note へ 1 行足す（複数の形を試したときは、試した順に並ぶ）。
		void AddNote(std::string* note, const std::string& line)
		{
			if (note == nullptr || line.empty())
				return;
			if (!note->empty())
				*note += " / ";
			*note += line;
		}
	} // namespace

	SettingsOutcome showImportSettings(core::ImportOptions& options, std::string* note)
	{
		try
		{
			core::ImportOptions& remembered = RememberedOptions();
			const SymbolResources resources = CollectSymbolResources();

			// **本命（サムネイル）→ 退避（名前＋絵）の順に試す。** 前者で出せなかった
			// ときだけ後者へ落ちる（冒頭「2 つの形を持ち、出せた方を使う」）。
			for (const Form form : {Form::ImagePopup, Form::NameList})
			{
				CImportSettingsDialog dialog(remembered, resources, form);
				const auto button = dialog.RunDialogLayout("");
				if (dialog.Failed())
				{
					AddNote(note, (form == Form::ImagePopup ? "サムネイルの形で出せません: "
															: "名前の形でも出せません: ") +
									  dialog.Note());
					continue; // 次の形で開き直す
				}
				if (form == Form::NameList)
					AddNote(note, "名前の形で表示しました");
				AddNote(note, dialog.Note());
				if (button != VWFC::VWUI::kDialogButton_Ok)
					return SettingsOutcome::Cancelled;
				remembered = dialog.Result();
				options = remembered;
				return SettingsOutcome::Accepted;
			}
			return SettingsOutcome::Unavailable;
		}
		catch (...)
		{
			// ダイアログ由来の異常で取り込みの入口を塞がない（既定の対応で続ける）。
			AddNote(note, "設定ダイアログで例外が出ました");
			return SettingsOutcome::Unavailable;
		}
	}
} // namespace HomeskzIfcImport::draw
