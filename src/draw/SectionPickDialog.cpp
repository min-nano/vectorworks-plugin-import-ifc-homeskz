//
//	draw/SectionPickDialog.cpp
//
//	軸組図にする通りを選ぶダイアログの実装（意図と規約は draw/SectionPickDialog.h）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、プラグインビルドでのみ
//	コンパイルされる。
//
//	使う SDK API は取り込み設定ダイアログ（draw/SettingsDialog）で実機に出ているものだけ:
//	VWStaticTextCtrl・VWCheckButtonCtrl・AddFirstGroupControl / AddBelowControl /
//	AddRightControl・AddDDX_CheckButton。
//
//	【並べ方】方向ごとに見出し（"X通り" / "Y通り"）を置き、その下へ通りを**縦に**並べる。
//	1 列が kRowsPerColumn 本を超えたら右へ列を足す（30 本の通りを 1 列に積むと画面から
//	はみ出す）。Y通りの見出しは X通りのいちばん左の列の下に置く——列は左から埋まるので、
//	そこが必ず最後まで埋まっている（設定ダイアログの図面枠の行と同じ考え方）。
//
//	【チェックに文字を持たせない】行は「チェック（文字なし）＋幅を固定した名前」の 2 つで
//	組む。チェックに名前を持たせると幅が名前ごとに変わり、右の列の頭が揃わない
//	（設定ダイアログと同じ作法）。
//

#include "PluginPrefix.h"
#include "draw/SectionPickDialog.h"

#include "core/Document.h"
#include "core/ImportOptions.h"

#include <cstddef>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 1 列に並べる通りの本数。これを超えたら右へ列を足す。
		constexpr std::size_t kRowsPerColumn = 10;

		// 名前の欄の幅（標準文字幅）。通り名は "X10''" / "又又い" / "1(2)" ほどの長さ。
		constexpr short kNameWidthChars = 8;

		// 列と列の間隔（標準文字幅）。
		constexpr short kColumnGapChars = 2;

		// コントロール ID。1 = OK / 2 = キャンセルは SDK の予約。通り i は
		// [チェック, 名前] の 2 つを kFirstItemID から 2 つ刻みで使う。イベントは受けない
		// （DDX で読むだけ）ので、ID は実行時に決まってよい。
		constexpr TControlID kIntroID = 3;
		constexpr TControlID kFirstHeaderID = 4; // 方向ごとに 1 つ（4 = X通り・5 = Y通り）
		constexpr TControlID kFirstItemID = 10;

		TControlID checkID(std::size_t item)
		{
			return static_cast<TControlID>(kFirstItemID + (item * 2));
		}
		TControlID nameID(std::size_t item)
		{
			return static_cast<TControlID>(checkID(item) + 1);
		}

		// 方向の見出し。X通りは定 X（平面で縦の通り芯）、Y通りは定 Y（横の通り芯）。
		const char* directionLabel(core::SectionDirection direction)
		{
			return direction == core::SectionDirection::X ? "X通り（縦の通り芯）"
														  : "Y通り（横の通り芯）";
		}

		// 通り 1 本ぶん。**deque に直接作る**——ダイアログは生存中ずっとコントロールと
		// DDX の変数のアドレスを持つので、追加で既存の要素が動いてはいけない
		// （draw/SettingsDialog.cpp と同じ理由）。
		struct Item
		{
			Item(std::size_t position, std::string name)
				: index(position), number(std::move(name)), check(checkID(position)),
				  label(nameID(position))
			{
			}

			std::size_t index; // 何本目か（コントロール ID の元）
			std::string number; // 図番（core::ImportOptions::skippedSections に入れる綴り）
			VWCheckButtonCtrl check;
			VWStaticTextCtrl label;
			bool drawn = true; // DDX の受け先。**初期値は全部描く**（ヘッダ冒頭）
		};

		// 方向 1 つぶん（見出しと、その方向の通りの範囲）。
		struct Group
		{
			core::SectionDirection direction = core::SectionDirection::X;
			std::size_t first = 0; // fItems の添字
			std::size_t count = 0;
		};

		class CSectionPickDialog : public VWDialog
		{
		public:
			explicit CSectionPickDialog(const std::vector<core::SectionCommand>& candidates)
				: fIntro(kIntroID)
			{
				// 方向ごとに括る。候補は X通り → Y通りの順で来る（parse/Section）が、
				// 並びに頼らず方向で分けてから並べる。
				for (const core::SectionDirection direction :
					 {core::SectionDirection::X, core::SectionDirection::Y})
				{
					Group group;
					group.direction = direction;
					group.first = fItems.size();
					for (const core::SectionCommand& section : candidates)
					{
						if (section.direction != direction)
							continue;
						fItems.emplace_back(fItems.size(), section.viewport.drawingNumber);
					}
					group.count = fItems.size() - group.first;
					if (group.count == 0)
						continue;
					fHeaders.emplace_back(static_cast<TControlID>(kFirstHeaderID + fGroups.size()));
					fGroups.push_back(group);
				}
			}
			~CSectionPickDialog() override = default;

			bool Shown() const
			{
				return fShown;
			}

			const std::string& Note() const
			{
				return fNote;
			}

			// チェックを外された通りの図番。
			std::vector<std::string> Skipped() const
			{
				std::vector<std::string> skipped;
				for (const Item& item : fItems)
				{
					if (!item.drawn)
						skipped.push_back(item.number);
				}
				return skipped;
			}

		protected:
			bool CreateDialogLayout() override
			{
				if (!this->CreateDialog("軸組図にする通りの選択", "取り込む", "キャンセル", false))
				{
					fNote = "ダイアログの枠を作れませんでした";
					return false;
				}
				if (!fIntro.CreateControl(this, "軸組図を描く通りにチェックを入れてください。"
												"チェックを外した通りの軸組図は描きません。"))
				{
					fNote = "説明文を作れませんでした";
					return false;
				}
				this->AddFirstGroupControl(&fIntro);

				VWControl* above = &fIntro; // 次の見出しを置く相手
				for (std::size_t g = 0; g < fGroups.size(); ++g)
				{
					const Group& group = fGroups[g];
					VWStaticTextCtrl& header = fHeaders[g];
					if (!header.CreateControl(this, directionLabel(group.direction)))
					{
						fNote = "見出しを作れませんでした";
						return false;
					}
					this->AddBelowControl(above, &header, 0, 1);

					for (std::size_t k = 0; k < group.count; ++k)
					{
						Item& item = fItems[group.first + k];
						if (!item.check.CreateControl(this, ""))
						{
							fNote = "チェックを作れませんでした";
							return false;
						}
						if (!item.label.CreateControl(this, TXString(item.number.c_str()),
													  kNameWidthChars))
						{
							fNote = "通りの名前を作れませんでした";
							return false;
						}
						// 列の先頭は、1 列目なら見出しの下、2 列目以降なら前の列の先頭の
						// 名前の右。列の途中は 1 つ上のチェックの下。
						if (k == 0)
							this->AddBelowControl(&header, &item.check);
						else if (k % kRowsPerColumn == 0)
							this->AddRightControl(&fItems[group.first + k - kRowsPerColumn].label,
												  &item.check, kColumnGapChars);
						else
							this->AddBelowControl(&fItems[group.first + k - 1].check, &item.check);
						this->AddRightControl(&item.check, &item.label);
					}
					// 次の見出しは**いちばん左の列の最後**の下（列は左から埋まる）。
					const std::size_t lastOfFirstColumn =
						(group.count < kRowsPerColumn ? group.count : kRowsPerColumn) - 1;
					above = &fItems[group.first + lastOfFirstColumn].check;
				}
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				for (Item& item : fItems)
					item.check.SetState(item.drawn);
				fShown = true;
			}

			void OnDDXInitialize() override
			{
				for (Item& item : fItems)
					this->AddDDX_CheckButton(checkID(item.index), &item.drawn);
			}

			// 個々のコントロールのイベントは受けないが、VWDialog がこの宣言を要求する
			// （draw/ResultDialog と同じ）。
			DEFINE_EVENT_DISPATH_MAP;

		private:
			VWStaticTextCtrl fIntro;
			std::deque<VWStaticTextCtrl> fHeaders;
			std::deque<Item> fItems;
			std::vector<Group> fGroups;
			bool fShown = false;
			std::string fNote;
		};

		// EVENT_DISPATCH_MAP_BEGIN は SDK のマクロ。展開に const 化できるローカルが出るが、
		// それはマクロ側のコードでこちらのものではない（draw/ResultDialog.cpp と同じ）。
		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CSectionPickDialog);
		EVENT_DISPATCH_MAP_END;
	} // namespace

	SettingsOutcome showSectionPicker(const std::vector<core::SectionCommand>& candidates,
									  core::ImportOptions& options, std::string* note)
	{
		if (candidates.empty())
			return SettingsOutcome::Unavailable;
		try
		{
			CSectionPickDialog dialog(candidates);
			const auto button = dialog.RunDialogLayout("");
			if (!dialog.Shown())
			{
				if (note != nullptr)
					*note = "通りの選択を出せません: " + dialog.Note();
				return SettingsOutcome::Unavailable;
			}
			if (button != VWFC::VWUI::kDialogButton_Ok)
				return SettingsOutcome::Cancelled;
			options.setSkippedSections(dialog.Skipped());
			return SettingsOutcome::Accepted;
		}
		catch (...)
		{
			// ダイアログ由来の異常で取り込みの入口を塞がない（全部描くで続ける）。
			if (note != nullptr)
				*note = "通りの選択で例外が出ました";
			return SettingsOutcome::Unavailable;
		}
	}
} // namespace HomeskzIfcImport::draw
