//
//	draw/SettingsDialog.cpp
//
//	取り込み設定ダイアログの実装（意図と規約は draw/SettingsDialog.h）。【SDK 依存】
//	PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	使う SDK API は VWFC のレイアウトダイアログ（draw/ResultDialog と同じ作法）と、
//	リソース一覧・サムネイル付きメニュー:
//
//	  * VWResourceList::BuildList(kSymDefNode, sort) … 図面のシンボル定義の一覧
//	    （kSymDefNode = 16。Kernel/API/Objs.TDType.h）
//	  * VWResourceList::GetResourceName(i, name)     … その名前（UTF-8 の TXString）
//	  * VWCheckButtonCtrl                            … その要素を取り込むか
//	  * VWImagePopupCtrl::AddItem(resourceList, i)   … サムネイル付きの項目を 1 つ足す
//	  * VWImagePopupCtrl::Set/GetSelectedItemIndex   … 選択（項目の添字）
//	  * VWImagePopupCtrl::ShowImage(true)            … 閉じているときも絵を出す
//	  * AddRightControl / AddBelowControl            … 行（3 つのコントロール）の並べ方
//	  * VWDialog::EnableControl(id, bool)            … チェックを外した行を灰色にする
//
//	【シンボルは絵で選ぶ】VectorWorks 自身の「鋼材断面を選択」などと同じ、**サムネイルを
//	並べたポップアップ**（VWImagePopupCtrl）で選ばせる。絵は VW がリソースに対して持っている
//	サムネイルそのものなので、こちらで描画モードやビューを決める必要が無い。
//
//	【項目は図面のシンボル定義そのもの】このコントロールの項目は**実在するリソース**で、
//	名前だけの項目は作れない。行ごとの「取り込む」チェックがあるおかげでそれで足りる——
//	置くものが図面に無いなら、その要素はチェックを外せばよい（core/ImportOptions.h、
//	docs/DEV-NOTES.md「取り込み設定の決め事（M20）」）。
//
//	【リソース一覧はダイアログが持ち続ける】項目はリソース一覧を指しているので、一覧を
//	先に捨てると項目の絵が引けなくなる。ダイアログのメンバとして生存させる
//	（VWResourceList は参照カウント付きでコピーできる）。
//
//	【選択は OK が押された瞬間に読む】画像ポップアップの選択は DDX で受けず、
//	OnDefaultButtonEvent（＝OK）でコントロールから読み取る。**ダイアログが閉じた後では
//	コントロールから読めない**ため、押された瞬間に控えておく必要がある（チェックの方は
//	AddDDX_CheckButton で受けられる）。
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
		// [チェック, 説明, ポップアップ] の 3 つを kFirstRowID から 3 つ刻みで使う。
		constexpr TControlID kIntroID = 3;
		constexpr TControlID kFirstRowID = 10;
		constexpr TControlID kRowStride = 3;

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

		// 説明は**幅を固定して**左の列を揃える（可変幅だとポップアップの左端が行ごとに
		// ずれる。チェックは文字を持たないので幅が揃う）。
		constexpr short kLabelWidthChars = 22;

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
			CImportSettingsDialog(const core::ImportOptions& seed, SymbolResources resources)
				: fIntro(kIntroID), fResources(std::move(resources))
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					fChecks.emplace_back(checkID(row));
					fLabels.emplace_back(labelID(row));
					fPopups.emplace_back(popupID(row));

					// **いまの対応先が図面にある役割だけを「取り込む」で開く。** 無い名前は
					// 項目にできない（＝置きようがない）ので、チェックを外した状態にする。
					const std::size_t index = IndexOf(seed.symbol(roleAt(row)));
					fSelection[row] = index < fResources.names.size() ? index : 0;
					fEnabled[row] = seed.isEnabled(roleAt(row)) && index < fResources.names.size();
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
					return false;
				// 図面にシンボルが 1 つも無いなら、選ばせる前にそう言う。
				const TXString intro =
					fResources.names.empty()
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
					VWImagePopupCtrl& popup = fPopups[row];
					// チェックは文字を持たない（役割の名前は隣の説明が出す）——文字を
					// 持たせると幅が行ごとに変わり、右の列が揃わない。
					if (!check.CreateControl(this, ""))
						return false;
					if (!label.CreateControl(this, core::symbolRoleLabel(roleAt(row)),
											 kLabelWidthChars))
						return false;
					if (!popup.CreateControl(this))
						return false;
					// 行の頭（チェック）は前の行の頭の下、残りはその右へ。行間を空けるのは
					// 説明文の下（＝最初の行の上）だけ——絵が文字より背が高いぶん、行そのものは
					// 詰めても窮屈にならない。
					this->AddBelowControl(previousRow, &check, 0, row == 0 ? 1 : 0);
					this->AddRightControl(&check, &label);
					this->AddRightControl(&label, &popup);
					previousRow = &check;
				}
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
				{
					VWImagePopupCtrl& popup = fPopups[row];
					// **項目はリソース一覧の順に足す**——項目の添字と名前の添字を一致させて
					// おくと、選択をそのまま名前へ引き直せる。
					popup.ShowImage(true); // 閉じているときも選択中の絵を出す
					for (std::size_t i = 0; i < fResources.names.size(); ++i)
						popup.AddItem(fResources.list, i);
					if (fSelection[row] < fResources.names.size())
						popup.SetSelectedItemIndex(fSelection[row]);
					fChecks[row].SetState(fEnabled[row]);
					UpdateRow(row);
				}
				fShown = true;
			}

			// チェックだけ DDX で受ける（選択は OnDefaultButtonEvent で読む。冒頭参照）。
			void OnDDXInitialize() override
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
					this->AddDDX_CheckButton(checkID(row), &fEnabled[row]);
			}

			// OK が押された。**閉じる前に**各行の選択を控える（閉じた後のコントロールからは
			// 読めない）。
			void OnDefaultButtonEvent() override
			{
				for (std::size_t row = 0; row < core::kSymbolRoleCount; ++row)
					fSelection[row] = fPopups[row].GetSelectedItemIndex();
				VWDialog::OnDefaultButtonEvent();
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
			}

			VWStaticTextCtrl fIntro;
			// **deque に直接作る。** 行数ぶんのコントロールを溜めるが、vector だと追加の
			// たびに既存の要素が動いてしまう（ダイアログは生存中ずっとコントロールの
			// アドレスを持つ）。deque は追加しても既存の要素を動かさない
			// （draw/ResultDialog.cpp の本文行と同じ理由）。
			std::deque<VWCheckButtonCtrl> fChecks;
			std::deque<VWStaticTextCtrl> fLabels;
			std::deque<VWImagePopupCtrl> fPopups;
			SymbolResources fResources; // 項目の元（ダイアログより長生きさせない）
			std::array<std::size_t, core::kSymbolRoleCount> fSelection = {};
			std::array<bool, core::kSymbolRoleCount> fEnabled = {};
			bool fShown = false;
		};

		// 役割を 1 つ足したら、下のイベントマップにも 1 行足すこと（コントロールの ID は
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
			CImportSettingsDialog dialog(remembered, CollectSymbolResources());
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
