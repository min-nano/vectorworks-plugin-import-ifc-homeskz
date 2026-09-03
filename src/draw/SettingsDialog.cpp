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
//	  * VWThumbnailPopupCtrl                         … サムネイル付きの選択（本命の形）
//	  * VWPullDownMenuCtrl + VWSymbolDisplayCtrl     … 名前で選び、絵は隣に出す（退避の形）
//	  * AddRightControl / AddBelowControl            … 行の並べ方
//	  * VWDialog::EnableControl(id, bool)            … チェックを外した行を灰色にする
//
//	【サムネイルは VWThumbnailPopupCtrl で出す（VWImagePopupCtrl ではない）】名前が似た
//	コントロールが 2 つあり、**VWImagePopupCtrl は使えない**——`CreateControl` が
//	`return false` のスタブで、呼び順や初期化に関わらず必ず失敗する（SDK 同梱の実装ソースで
//	確定。[SDK リファレンス「レイアウトダイアログ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)）。
//	実装が生きているのは同じコンポーネント種別を指す双子の VWThumbnailPopupCtrl の方で、
//	項目はリソース一覧の ID と添字で足す（AddImageFromResource）。
//
//	【2 つの形を持ち、出せた方を使う】それでも**組み立てに失敗したときに黙って設定
//	ダイアログごと出ないのが最悪**なので（画像ポップアップで実際にそうなった。cd8a415 の
//	実測）、失敗したら**名前のプルダウン＋シンボル表示コントロール**という確実に出る形へ
//	切り替えて開き直す。どちらの形で出したか（と、切り替えた理由）は取り込みログに残る。
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
//	ただし**一覧をそのまま出さない**——`BuildList(kSymDefNode)` は VectorWorks 自身が
//	プラグインオブジェクトのスタイルとして持っている定義まで返すので、`GetSymbolDefSubType`
//	で外す（下記 IsPlaceableSymbol）。
//
//	【リソース一覧はダイアログが持ち続ける】サムネイルの項目はリソース一覧を **ID で**
//	指しているので、一覧を先に捨てると絵が引けなくなる（VWResourceList は参照カウント式で、
//	最後の 1 つが消えるときに一覧そのものを破棄する）。ダイアログのメンバとして生存させる。
//
//	【選択は名前で引き取る】サムネイルの選択は DDX で受けられないので、OnDefaultButtonEvent
//	（＝OK が押された瞬間。**閉じた後のコントロールからは読めない**）に読む。読むのは
//	`GetSelectedItem()`（選ばれたリソースの InternalIndex）→ `InternalIndexToNameN` で
//	**名前**——項目の添字と候補の対応に頼らずに済む（対応は取れているが、候補を絞って
//	足している以上、名前で引く方が崩れない）。名前を引けなかったときだけ添字
//	（`GetSelectedItemIndex()`）へ落とす。名前のプルダウンは AddDDX_PulldownMenu で受ける。
//
//	**「まだ選んでいない」は読み取れない**——項目を足した時点で先頭が選ばれた状態になる
//	（実機で確認済み。上記 Findings）。この画面では行ごとの「取り込む」チェックが
//	その役目を持つので、未選択を判別する必要は無い。
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
			Thumbnail,
			NameList,
		};

		// 図面のシンボル定義の一覧と、そこから採った**候補**。names[i] が i 番目の候補の
		// 名前で、listIndices[i] がその一覧側の添字（サムネイルの項目はこの添字で足す）。
		// **候補の並びと項目の並びは 1 対 1**なので、選択された項目の添字がそのまま候補の
		// 添字になる。
		struct SymbolResources
		{
			VWFC::Tools::VWResourceList list;
			std::vector<std::string> names;
			std::vector<std::size_t> listIndices;
		};

		// そのシンボル定義が**ユーザが図面へ置く部品**か。
		//
		// 【なぜ要るか】`BuildList(kSymDefNode)` は図面のシンボル定義を**全部**返すので、
		// VectorWorks 自身がプラグインオブジェクトのスタイルとして持っている定義
		// （図面枠・データタグ・図面ラベル・立断面指示線・グラフィック凡例・木質構造材…）
		// まで並ぶ。選択肢に出しても置けるものではないので外す。
		//
		// 切り分けは `GetSymbolDefSubType`——**0 なら普通のシンボル定義、0 以外はその
		// プラグインオブジェクトのスタイル**（値は PIO の型）。フォルダ名では切り分けられない
		// （"…スタイル" フォルダに入らないものがある）し、2D/3D/ハイブリッドの別も無関係
		// （[SDK リファレンス「シンボル」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Symbols.md)
		// の実測表）。
		bool IsPlaceableSymbol(MCObjectHandle definition)
		{
			if (definition == nil)
				return false;
			return gSDK->GetSymbolDefSubType(definition) == 0;
		}

		// いま開いている図面の**置けるシンボル定義**（名前順）。読めなければ空（＝どの要素も
		// 取り込めない。ダイアログ自体は出せる）。
		SymbolResources CollectSymbolResources()
		{
			SymbolResources resources;
			try
			{
				const std::size_t count = resources.list.BuildList(kSymDefNode, true);
				resources.names.reserve(count);
				resources.listIndices.reserve(count);
				for (std::size_t i = 0; i < count; ++i)
				{
					if (!IsPlaceableSymbol(resources.list.GetResource(i)))
						continue;
					TXString name;
					resources.list.GetResourceName(i, name);
					std::string text = static_cast<const char*>(name);
					if (text.empty())
						continue; // 名前で選ばせる以上、名前の無い定義は候補にしない
					resources.names.push_back(std::move(text));
					resources.listIndices.push_back(i);
				}
			}
			catch (...)
			{
				// リソース一覧を作れない図面でも設定ダイアログ自体は出す（候補が空になり、
				// どの要素にもチェックが入らない）。1 つの失敗で取り込みの入口を塞がない。
				// **途中まで採れていた候補は捨てる**——半端な一覧は項目と対応しない。
				resources.names.clear();
				resources.listIndices.clear();
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
			// resources は値で受ける（形を変えて開き直すことがあるので、呼び出し側は同じ
			// 一覧を持ったまま。VWResourceList は参照カウント付きでコピーできる）。
			CImportSettingsDialog(const core::ImportOptions& seed, SymbolResources resources,
								  Form form)
				: fIntro(kIntroID), fResources(std::move(resources)), fForm(form)
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					fChecks.emplace_back(checkID(row));
					fLabels.emplace_back(labelID(row));
					if (fForm == Form::Thumbnail)
						fThumbs.emplace_back(popupID(row));
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
					if (fForm == Form::Thumbnail)
						this->AddRightControl(&fLabels[row], &fThumbs[row]);
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
				if (fForm == Form::Thumbnail)
				{
					try
					{
						for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
							fSelection[row] = SelectedIndexOf(row);
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
				if (fForm == Form::Thumbnail)
				{
					if (fThumbs[row].CreateControl(this, kStandardSize))
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
				if (fForm == Form::Thumbnail)
				{
					VWThumbnailPopupCtrl& popup = fThumbs[row];
					// 項目はリソース一覧の **ID と（一覧側の）添字**で足す（絵は VW が引く）。
					// **候補だけを候補の順に足す**ので、項目 i ＝ 候補 i になる。
					const Sint32 listID = fResources.list.GetListID();
					for (const std::size_t listIndex : fResources.listIndices)
						popup.AddImageFromResource(listID, listIndex);
					if (valid)
						popup.SelectItem(fSelection[row]);
					return;
				}
				VWPullDownMenuCtrl& popup = fPopups[row];
				for (const std::string& name : fResources.names)
					popup.AddItem(TXString(name.c_str()));
				if (valid)
					popup.SelectIndex(fSelection[row]);
			}

			// サムネイルで選ばれている項目を**名前で**引き当て、候補の添字にして返す
			// （冒頭「選択は名前で引き取る」）。名前が引けなければ項目の添字に落とし、
			// それも範囲外なら開いたときの選択のまま返す。
			std::size_t SelectedIndexOf(std::size_t row) const
			{
				const VWThumbnailPopupCtrl& popup = fThumbs[row];
				TXString name;
				gSDK->InternalIndexToNameN(popup.GetSelectedItem(), name);
				const std::string text = static_cast<const char*>(name);
				if (!text.empty())
				{
					const std::size_t byName = IndexOf(text);
					if (byName < fResources.names.size())
						return byName;
				}
				const std::size_t byIndex = popup.GetSelectedItemIndex();
				return byIndex < fResources.names.size() ? byIndex : fSelection[row];
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
			std::deque<VWThumbnailPopupCtrl> fThumbs;  // Form::Thumbnail のときだけ
			std::deque<VWPullDownMenuCtrl> fPopups;	   // Form::NameList のときだけ
			std::deque<VWSymbolDisplayCtrl> fPreviews; // 同上
			SymbolResources fResources; // 項目の元（ダイアログより長生きさせない）
			Form fForm = Form::Thumbnail;
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
			for (const Form form : {Form::Thumbnail, Form::NameList})
			{
				CImportSettingsDialog dialog(remembered, resources, form);
				const auto button = dialog.RunDialogLayout("");
				if (dialog.Failed())
				{
					AddNote(note, (form == Form::Thumbnail ? "サムネイルの形で出せません: "
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
