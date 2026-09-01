//
//	draw/SettingsDialog.cpp
//
//	取り込み設定ダイアログの実装（意図と規約は draw/SettingsDialog.h）。【SDK 依存】
//	PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	使う SDK API は VWFC のレイアウトダイアログ（draw/ResultDialog と同じ作法）と、
//	リソース一覧・シンボル表示の 2 つ:
//
//	  * VWResourceList::BuildList(kSymDefNode, sort) … 図面のシンボル定義の一覧
//	    （kSymDefNode = 16。Kernel/API/Objs.TDType.h）
//	  * VWResourceList::GetResourceName(i, name)     … その名前（UTF-8 の TXString）
//	  * VWPullDownMenuCtrl                           … 名前を選ぶプルダウン
//	  * VWSymbolDisplayCtrl::CreateControl(dlg, w, h, margin) / Update(name, render, view)
//	                                                  … 選択中のシンボルの絵
//	  * AddRightControl / AddBelowControl            … 行（説明・プルダウン・絵）の並べ方
//
//	【絵の出し方は VW のサムネイルに合わせる】Update に渡す描画モードとビューは、
//	VectorWorks 自身がシンボルのサムネイルに使う既定値と同じ **Top/Plan（view = 2）＋
//	ワイヤーフレーム（renderMode = 0）** にしてある（Kernel/API/MiniCadCallBacks.h の
//	SymbolImgInfo の既定構築子。`SymbolImgInfo(-1, -1, -1, 2/*TopPlan*/, 0/*Wireframe*/, …)`）。
//	**3D の標準ビュー（standardViewTop = 7）ではない**——伏図記号のような 2D 部品だけの
//	シンボルは 3D ビューでは何も映らず、絵で選ぶという目的が果たせない。
//
//	【プルダウン＋絵にした理由】シンボルをサムネイル付きのメニューで選ぶコントロール
//	（VWImagePopupCtrl）も SDK にはあるが、あちらは項目が**リソースそのもの**なので
//	「図面に無い名前」を項目にできない。取り込みの既定名がテンプレート未適用の図面に
//	無いことは普通にあり、そのとき現在の対応を出せずに勝手な 1 つを選んだことにしてしまう
//	（＝黙って違うシンボルが置かれる）。名前の一覧はこちらが完全に決められる形にして、
//	絵は別のコントロールで見せる（docs/DEV-NOTES.md M20「シンボルの対応を選ぶ」）。
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
		// [説明, プルダウン, 絵] の 3 つを kFirstRowID から 3 つ刻みで使う。
		constexpr TControlID kIntroID = 3;
		constexpr TControlID kFirstRowID = 10;
		constexpr TControlID kRowStride = 3;

		constexpr TControlID labelID(std::size_t row)
		{
			return static_cast<TControlID>(kFirstRowID + (row * kRowStride));
		}
		constexpr TControlID popupID(std::size_t row)
		{
			return static_cast<TControlID>(labelID(row) + 1);
		}
		constexpr TControlID previewID(std::size_t row)
		{
			return static_cast<TControlID>(labelID(row) + 2);
		}

		// 行の大きさ。説明は**幅を固定して**左の列を揃える（可変幅だとプルダウンの左端が
		// 行ごとにずれる）。絵はサムネイルとして見分けが付く程度で、7 行並べても画面に
		// 収まる大きさ。
		constexpr short kLabelWidthChars = 22;
		constexpr short kPopupWidthChars = 30;
		constexpr short kPreviewSizePixels = 56;
		constexpr short kPreviewMarginPixels = 2;

		// シンボルの絵の出し方（冒頭「絵の出し方は VW のサムネイルに合わせる」）。
		constexpr TRenderMode kPreviewRenderMode = 0; // ワイヤーフレーム
		constexpr TStandardView kPreviewView = 2;	  // Top/Plan

		// 図面に無い名前に添える但し書き。**選ぶ前に分かるようにする**ためのもので、
		// このまま取り込むと（シンボル定義が無いので）その要素は 1 つも置かれない。
		constexpr const char* kMissingSuffix = "（図面にありません）";

		// プルダウン 1 項目。value が命令へ書き込む名前で、label が画面に出る文字列。
		struct Candidate
		{
			std::string value;
			TXString label;
		};

		// いま開いている図面のシンボル定義の名前（名前順・重複なし）。読めなければ空
		// （＝候補は現在の対応だけになる。ダイアログは出せる）。
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
				// リソース一覧を作れない図面でも設定ダイアログ自体は出す（候補が
				// 現在の対応だけになる）。1 つの失敗で取り込みの入口を塞がない。
			}
			std::ranges::sort(names);
			const auto duplicates = std::ranges::unique(names);
			names.erase(duplicates.begin(), duplicates.end());
			return names;
		}

		// 候補の一覧。図面のシンボル ∪ いまの対応先で、名前順。図面に無いものだけ
		// 但し書きを添える（値そのものは名前のまま）。
		std::vector<Candidate> BuildCandidates(const std::vector<std::string>& documentSymbols,
											   const core::ImportOptions& current)
		{
			std::vector<std::string> values = documentSymbols;
			for (const core::SymbolRoleInfo& info : core::symbolRoles())
				values.push_back(current.symbol(info.role));
			std::ranges::sort(values);
			const auto duplicates = std::ranges::unique(values);
			values.erase(duplicates.begin(), duplicates.end());

			std::vector<Candidate> candidates;
			candidates.reserve(values.size());
			for (const std::string& value : values)
			{
				const bool inDocument = std::ranges::binary_search(documentSymbols, value);
				const std::string label = inDocument ? value : value + kMissingSuffix;
				Candidate candidate;
				candidate.value = value;
				candidate.label = TXString(label.c_str());
				candidates.push_back(std::move(candidate));
			}
			return candidates;
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
								  std::vector<Candidate> candidates)
				: fIntro(kIntroID), fCandidates(std::move(candidates))
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					fLabels.emplace_back(labelID(row));
					fPopups.emplace_back(popupID(row));
					fPreviews.emplace_back(previewID(row));
					fSelection[row] = IndexOf(seed.symbol(roleAt(row)));
				}
			}
			~CImportSettingsDialog() override = default;

			// **実際に出せたか。** 組めなかったときは呼び出し側が「既定のまま取り込む」へ
			// 落とす（draw/SettingsDialog.h の Unavailable）。
			bool Shown() const
			{
				return fShown;
			}

			// 選ばれた対応。選択が候補の範囲外（あり得ないが）なら既定へ戻す。
			core::ImportOptions Result() const
			{
				core::ImportOptions options;
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					const std::size_t index = fSelection[row];
					if (index < fCandidates.size())
						options.setSymbol(roleAt(row), fCandidates[index].value);
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
				if (!fIntro.CreateControl(this, "配置するシンボルを選んでください。"))
					return false;
				this->AddFirstGroupControl(&fIntro);

				VWControl* previousRow = &fIntro;
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					VWStaticTextCtrl& label = fLabels[row];
					VWPullDownMenuCtrl& popup = fPopups[row];
					VWSymbolDisplayCtrl& preview = fPreviews[row];
					if (!label.CreateControl(this, core::symbolRoleLabel(roleAt(row)),
											 kLabelWidthChars))
						return false;
					if (!popup.CreateControl(this, kPopupWidthChars))
						return false;
					if (!preview.CreateControl(this, kPreviewSizePixels, kPreviewSizePixels,
											   kPreviewMarginPixels))
						return false;
					// 行の頭（説明）は前の行の頭の下、プルダウンと絵はその右へ。
					this->AddBelowControl(previousRow, &label, 0, 1);
					this->AddRightControl(&label, &popup);
					this->AddRightControl(&popup, &preview);
					previousRow = &label;
				}
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					for (const Candidate& candidate : fCandidates)
						fPopups[row].AddItem(candidate.label);
					if (fSelection[row] < fCandidates.size())
						fPopups[row].SelectIndex(fSelection[row]);
					UpdatePreview(row);
				}
				fShown = true;
			}

			// プルダウンの選択と fSelection を結ぶ（OK で確定する）。
			void OnDDXInitialize() override
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
					this->AddDDX_PulldownMenu(popupID(row), &fSelection[row]);
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
					UpdatePreview(row);
					return;
				}
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			// 名前 → 候補の添字。無ければ 0（候補は必ず現在の対応を含むので、実際には
			// 見つかる）。
			std::size_t IndexOf(const std::string& value) const
			{
				for (std::size_t i = 0; i < fCandidates.size(); ++i)
					if (fCandidates[i].value == value)
						return i;
				return 0;
			}

			// 選択中のシンボルの絵を出す。名前が空なら空の表示になる（SDK の仕様）。
			void UpdatePreview(std::size_t row)
			{
				const std::size_t index = fSelection[row];
				const TXString name = index < fCandidates.size()
										  ? TXString(fCandidates[index].value.c_str())
										  : TXString("");
				fPreviews[row].Update(name, kPreviewRenderMode, kPreviewView);
			}

			VWStaticTextCtrl fIntro;
			// **deque に直接作る。** 行数ぶんのコントロールを溜めるが、vector だと追加の
			// たびに既存の要素が動いてしまう（ダイアログは生存中ずっとコントロールの
			// アドレスを持つ）。deque は追加しても既存の要素を動かさない
			// （draw/ResultDialog.cpp の本文行と同じ理由）。
			std::deque<VWStaticTextCtrl> fLabels;
			std::deque<VWPullDownMenuCtrl> fPopups;
			std::deque<VWSymbolDisplayCtrl> fPreviews;
			std::vector<Candidate> fCandidates;
			std::array<std::size_t, core::kSymbolRoleCount> fSelection = {};
			bool fShown = false;
		};

		// 役割を 1 つ足したら、下のイベントマップにも 1 行足すこと（プルダウンの ID は
		// コンパイル時の定数でなければならないので、ここだけは表から回せない）。
		static_assert(core::kSymbolRoleCount == 7,
					  "役割を増減したら CImportSettingsDialog のイベントマップも直すこと");

		// EVENT_DISPATCH_MAP_BEGIN は SDK のマクロで、その展開が misc-const-correctness に
		// 引っかかる（マクロ側のコードでこちらの落ち度ではない。draw/ResultDialog.cpp と同じ）。
		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CImportSettingsDialog);
		ADD_DISPATCH_EVENT(popupID(0), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(1), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(2), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(3), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(4), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(5), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(6), OnSymbolChanged);
		EVENT_DISPATCH_MAP_END;

		// 前回の選択（この VectorWorks を起動している間だけ覚えている）。初回は役割の表の
		// 既定名＝従来と同じ対応。**図面には何も書かない**——名前付きリソースを増やさないのと
		// 同じで、取り込みの設定を図面へ書き戻すことはしない（CLAUDE.md「開発の基本方針」4）。
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
			CImportSettingsDialog dialog(remembered,
										 BuildCandidates(DocumentSymbolNames(), remembered));
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
