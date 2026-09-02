//
//	draw/SettingsDialog.cpp
//
//	取り込み設定ダイアログの実装（意図と規約は draw/SettingsDialog.h）。【SDK 依存】
//	PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	使う SDK API は VWFC のレイアウトダイアログ（draw/ResultDialog と同じ作法）と、
//	リソース一覧・シンボル表示:
//
//	  * VWResourceList::BuildList(kSymDefNode, sort) … 図面のシンボル定義の一覧
//	    （kSymDefNode = 16。Kernel/API/Objs.TDType.h）
//	  * VWResourceList::GetResourceName(i, name)     … その名前（UTF-8 の TXString）
//	  * VWCheckButtonCtrl                            … その要素を取り込むか
//	  * VWPullDownMenuCtrl                           … 置くシンボルを名前で選ぶ
//	  * VWSymbolDisplayCtrl::CreateControl(dlg, w, h, margin) / Update(name, render, view)
//	                                                  … 選択中のシンボルの絵
//	  * AddRightControl / AddBelowControl            … 行（4 つのコントロール）の並べ方
//	  * VWDialog::EnableControl(id, bool)            … チェックを外した行を灰色にする
//
//	【絵の出し方は VW のサムネイルに合わせる】Update に渡す描画モードとビューは、
//	VectorWorks 自身がシンボルのサムネイルに使う既定値と同じ **Top/Plan（view = 2）＋
//	ワイヤーフレーム（renderMode = 0）** にしてある（Kernel/API/MiniCadCallBacks.h の
//	SymbolImgInfo の既定構築子。`SymbolImgInfo(-1, -1, -1, 2/*TopPlan*/, 0/*Wireframe*/, …)`）。
//	**3D の標準ビュー（standardViewTop = 7）ではない**——伏図記号のような 2D 部品だけの
//	シンボルは 3D ビューでは何も映らず、絵で選ぶという目的が果たせない。
//
//	【候補は図面にあるシンボルだけ】「取り込むか」のチェックがあるので、**図面に無い名前を
//	候補に混ぜる必要が無い**——置くものが図面に無いなら、その要素はチェックを外せばよい
//	（docs/DEV-NOTES.md「取り込み設定の決め事（M20）」）。名前の一覧が実在のリソースだけで
//	済むぶん、将来サムネイル付きのメニュー（VWImagePopupCtrl）へ置き換える道も開けている。
//

#include "PluginPrefix.h"
#include "draw/SettingsDialog.h"
#include "core/ImportOptions.h"

#include "VWFC/Tools/VWResourceList.h"

#include <algorithm>
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
		// [チェック, 説明, プルダウン, 絵] の 4 つを kFirstRowID から 4 つ刻みで使う。
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

		// 行の大きさ。説明は**幅を固定して**左の列を揃える（可変幅だとプルダウンの左端が
		// 行ごとにずれる。チェックは文字を持たないので幅が揃う）。絵はサムネイルとして
		// 見分けが付く程度で、7 行並べても画面に収まる大きさ。
		constexpr short kLabelWidthChars = 22;
		constexpr short kPopupWidthChars = 30;
		constexpr short kPreviewSizePixels = 56;
		constexpr short kPreviewMarginPixels = 2;

		// シンボルの絵の出し方（冒頭「絵の出し方は VW のサムネイルに合わせる」）。
		constexpr TRenderMode kPreviewRenderMode = 0; // ワイヤーフレーム
		constexpr TStandardView kPreviewView = 2;	  // Top/Plan

		// いま開いている図面のシンボル定義の名前（名前順・重複なし）。読めなければ空
		// （＝どの要素も取り込めない。ダイアログは出せる）。
		std::vector<std::string> DocumentSymbolNames()
		{
			std::vector<std::string> names;
			try
			{
				VWFC::Tools::VWResourceList list;
				const std::size_t count = list.BuildList(kSymDefNode, true);
				names.reserve(count);
				for (std::size_t i = 0; i < count; ++i)
				{
					TXString name;
					list.GetResourceName(i, name);
					std::string text = static_cast<const char*>(name);
					if (!text.empty())
						names.push_back(std::move(text));
				}
			}
			catch (...)
			{
				// リソース一覧を作れない図面でも設定ダイアログ自体は出す（候補が空になり、
				// どの要素にもチェックが入らない）。1 つの失敗で取り込みの入口を塞がない。
				// **途中まで採れていた名前は捨てる**——半端な一覧を「図面にあるもの」として
				// 見せると、無い名前が選べてしまう。
				names.clear();
			}
			std::ranges::sort(names);
			const auto duplicates = std::ranges::unique(names);
			names.erase(duplicates.begin(), duplicates.end());
			return names;
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
			CImportSettingsDialog(const core::ImportOptions& seed,
								  std::vector<std::string> candidates)
				: fIntro(kIntroID), fCandidates(std::move(candidates))
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					fChecks.emplace_back(checkID(row));
					fLabels.emplace_back(labelID(row));
					fPopups.emplace_back(popupID(row));
					fPreviews.emplace_back(previewID(row));

					// **いまの対応先が図面にある役割だけを「取り込む」で開く。** 無い名前は
					// 候補に出せない（＝置きようがない）ので、チェックを外した状態にする。
					const std::size_t index = IndexOf(seed.symbol(roleAt(row)));
					fSelection[row] = index < fCandidates.size() ? index : 0;
					fEnabled[row] = seed.isEnabled(roleAt(row)) && index < fCandidates.size();
				}
			}
			~CImportSettingsDialog() override = default;

			// **実際に出せたか。** 組めなかったときは呼び出し側が「既定のまま取り込む」へ
			// 落とす（draw/SettingsDialog.h の Unavailable）。
			bool Shown() const
			{
				return fShown;
			}

			// 選ばれた対応。チェックの無い役割は「取り込まない」で、名前は既定のまま
			// （名前は使われない。core/ImportOptions.h）。
			core::ImportOptions Result() const
			{
				core::ImportOptions options;
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					const std::size_t index = fSelection[row];
					const bool valid = index < fCandidates.size();
					options.setEnabled(roleAt(row), fEnabled[row] && valid);
					if (valid)
						options.setSymbol(roleAt(row), fCandidates[index]);
				}
				return options;
			}

		protected:
			bool CreateDialogLayout() override
			{
				// hasHelp = false。OK のボタン名は行き先（取り込み）そのものにする。
				if (!this->CreateDialog("ホームズ君 IFC 取り込みの設定", "取り込む", "キャンセル",
										false))
					return false;
				// 図面にシンボルが 1 つも無いなら、選ばせる前にそう言う。
				const TXString intro =
					fCandidates.empty()
						? "この図面にはシンボルが登録されていないため、シンボルで置く要素は"
						  "取り込めません。"
						: "取り込む要素にチェックを入れ、置くシンボルを選んでください。";
				if (!fIntro.CreateControl(this, intro))
					return false;
				this->AddFirstGroupControl(&fIntro);

				VWControl* previousRow = &fIntro;
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					VWCheckButtonCtrl& check = fChecks[row];
					VWStaticTextCtrl& label = fLabels[row];
					VWPullDownMenuCtrl& popup = fPopups[row];
					VWSymbolDisplayCtrl& preview = fPreviews[row];
					// チェックは文字を持たない（役割の名前は隣の説明が出す）——文字を
					// 持たせると幅が行ごとに変わり、右の列が揃わない。
					if (!check.CreateControl(this, ""))
						return false;
					if (!label.CreateControl(this, core::symbolRoleLabel(roleAt(row)),
											 kLabelWidthChars))
						return false;
					if (!popup.CreateControl(this, kPopupWidthChars))
						return false;
					if (!preview.CreateControl(this, kPreviewSizePixels, kPreviewSizePixels,
											   kPreviewMarginPixels))
						return false;
					// 行の頭（チェック）は前の行の頭の下、残りはその右へ。行間を空けるのは
					// 説明文の下（＝最初の行の上）だけ——絵が文字より背が高いぶん、行そのものは
					// 詰めても窮屈にならない。
					this->AddBelowControl(previousRow, &check, 0, row == 0 ? 1 : 0);
					this->AddRightControl(&check, &label);
					this->AddRightControl(&label, &popup);
					this->AddRightControl(&popup, &preview);
					previousRow = &check;
				}
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					for (const std::string& candidate : fCandidates)
						fPopups[row].AddItem(TXString(candidate.c_str()));
					if (fSelection[row] < fCandidates.size())
						fPopups[row].SelectIndex(fSelection[row]);
					fChecks[row].SetState(fEnabled[row]);
					UpdateRow(row);
				}
				fShown = true;
			}

			// コントロールと変数を結ぶ（OK で確定する）。
			void OnDDXInitialize() override
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					this->AddDDX_CheckButton(checkID(row), &fEnabled[row]);
					this->AddDDX_PulldownMenu(popupID(row), &fSelection[row]);
				}
			}

			// プルダウンが動いたら**その行の絵**を差し替える。DDX は OK のときにしか
			// 流れないので、いまの選択はコントロールから直接読む。
			void OnSymbolChanged(TControlID controlID, VWDialogEventArgs& /*eventArgs*/)
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					if (popupID(row) != controlID)
						continue;
					fSelection[row] = fPopups[row].GetSelectedIndex();
					UpdateRow(row);
					return;
				}
			}

			// チェックが変わったら、その行の選択肢と絵を有効／無効にする（取り込まない行が
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
			// 名前 → 候補の添字。無ければ候補の数（＝範囲外）を返す。
			std::size_t IndexOf(const std::string& value) const
			{
				for (std::size_t i = 0; i < fCandidates.size(); ++i)
					if (fCandidates[i] == value)
						return i;
				return fCandidates.size();
			}

			// その行の見た目を今の状態に合わせる（絵の差し替えと、取り込まない行の無効化）。
			// **候補が 1 つも無い行は常に無効**——選べるものが無いのにチェックできると、
			// 「取り込むと言ったのに何も置かれない」ことになる。
			void UpdateRow(std::size_t row)
			{
				const std::size_t index = fSelection[row];
				const bool valid = index < fCandidates.size();
				const TXString name = valid ? TXString(fCandidates[index].c_str()) : TXString("");
				fPreviews[row].Update(name, kPreviewRenderMode, kPreviewView);
				this->EnableControl(checkID(row), valid);
				this->EnableControl(popupID(row), valid && fEnabled[row]);
				this->EnableControl(previewID(row), valid && fEnabled[row]);
			}

			VWStaticTextCtrl fIntro;
			// **deque に直接作る。** 行数ぶんのコントロールを溜めるが、vector だと追加の
			// たびに既存の要素が動いてしまう（ダイアログは生存中ずっとコントロールの
			// アドレスを持つ）。deque は追加しても既存の要素を動かさない
			// （draw/ResultDialog.cpp の本文行と同じ理由）。
			std::deque<VWCheckButtonCtrl> fChecks;
			std::deque<VWStaticTextCtrl> fLabels;
			std::deque<VWPullDownMenuCtrl> fPopups;
			std::deque<VWSymbolDisplayCtrl> fPreviews;
			std::vector<std::string> fCandidates; // 図面にあるシンボル名（名前順）
			std::array<std::size_t, core::kSymbolRoleCount> fSelection = {};
			std::array<bool, core::kSymbolRoleCount> fEnabled = {};
			bool fShown = false;
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
	} // namespace

	SettingsOutcome showImportSettings(core::ImportOptions& options)
	{
		try
		{
			core::ImportOptions& remembered = RememberedOptions();
			CImportSettingsDialog dialog(remembered, DocumentSymbolNames());
			const auto button = dialog.RunDialogLayout("");
			if (!dialog.Shown())
				return SettingsOutcome::Unavailable; // レイアウトを組めなかった
			if (button != VWFC::VWUI::kDialogButton_Ok)
				return SettingsOutcome::Cancelled;
			remembered = dialog.Result();
			options = remembered;
			return SettingsOutcome::Accepted;
		}
		catch (...)
		{
			// ダイアログ由来の異常で取り込みの入口を塞がない（既定の対応で続ける）。
			return SettingsOutcome::Unavailable;
		}
	}
} // namespace HomeskzIfcImport::draw
