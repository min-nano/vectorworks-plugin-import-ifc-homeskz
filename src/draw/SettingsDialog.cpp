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
//	  * AddRightControl / AddBelowControl            … 行と列の並べ方
//	  * VWDialog::EnableControl(id, bool)            … チェックを外した行を灰色にする
//
//	【サムネイルは VWThumbnailPopupCtrl で出す（VWImagePopupCtrl ではない）】名前が似た
//	コントロールが 2 つあり、**VWImagePopupCtrl は使えない**——`CreateControl` が
//	`return false` のスタブで、呼び順や初期化に関わらず必ず失敗する（SDK 同梱の実装ソースで
//	確定。[SDK リファレンス「レイアウトダイアログ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)）。
//	実装が生きているのは同じコンポーネント種別を指す双子の VWThumbnailPopupCtrl の方で、
//	項目はリソース一覧の ID と添字で足す（AddImageFromResource）。
//
//	【縦に積まず 2 列に折る】1 行の高さはサムネイルの高さで決まり、**サムネイルの大きさは
//	選べない**（kStandardSize / kLineTypeSize の 2 つだけ。上記 Findings）。役割の数ぶん
//	そのまま縦に積むと画面に対して細長くなりすぎるので、行を 2 列へ折って高さを半分にする
//	（列の頭を「前の列の先頭行の右」へ置く。下記 kColumnCount / RowTail）。
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
//	【図面枠の行はシンボルではない ── 図面枠スタイル（M28）】役割の行の次の 1 行は
//	**図面枠（タイトルブロック）のスタイル**を選ぶ行で、選択肢の集め方だけが他と違う:
//	`BuildList(kSymDefNode)` が返すシンボル定義のうち、**`GetSymbolDefSubType` が
//	552（図面枠のスタイル）のもの**を並べる——他の行が「0＝普通のシンボル定義」を並べるのと
//	表裏である（値は下記 Findings「シンボル」の実測表。**同じ一覧から 2 通りに拾うだけ**
//	なので、リソース一覧は 1 つで足りる）。チェックを外せば図面枠を置かない（＝この設定を
//	入れる前と同じ）。**図面枠スタイルが図面にあれば、初期値は「一覧の最初のもので置く」**
//	（この起動中に一度 OK で閉じたあとは、その選択に従う）。
//
//	【その下の 1 行は寸法規格（M31）】図面枠の下にもう 1 行、伏図・軸組図へ入れる寸法の
//	**寸法規格**を選ぶ行がある。寸法規格は資源（シンボル定義）ではなく**文書が配列で持つ
//	もの**なので、サムネイルを出せない——この行だけは**形に依らず名前のプルダウン**で
//	選ばせる（絵も出さない）。候補は `GetDimensionStandardVariable(index,
//	dimStdstandardName)` を組み込み（1〜9）→ カスタム（0〜−8）の順に総当たりして、名前が
//	引けた index だけを並べる（SDK リファレンス Findings「Dimensions」）。チェックを外せば
//	寸法を入れない。**初期値は図面枠と同じく「入れる」**（ご要望）——まだ決めていない
//	うちは「JIS」があればそれ、無ければ一覧の最初の規格を選んだ状態で開く。
//
//	【その下は垂木の断面】寸法規格の下に、全垂木に一律で使う**垂木の断面（幅×せい、mm）**
//	を打ち込む欄を 2 つ置く（IFC に垂木の寸法が無いので決め打ちしていた 45×45 を差し替える。
//	core/ImportOptions.h の rafterWidth / rafterHeight）。選ぶものではなく数を打つので、
//	シンボルの行の仕組み（チェック・候補・サムネイル）には乗せず、**文字の入力欄
//	（VWEditTextCtrl）を DDX で受ける**。読むのは
//	core::parseRafterSize（全角の数字も読む）で、読めない・範囲外の値は**前回の値のまま**
//	取り込み、そのことをログへ残す（Note）。初期値は前回の値（初回は 45×45）。
//
//	【いちばん下は伏図のまとめ方】垂木の断面の下に、**横架材の高さごとに作る伏図**のうち
//	「前のレベルと同じ伏図にまとめる」高さのチェックを 1 つずつ並べる（draw/SettingsDialog.h）。
//	数は IFC によって変わるので、**イベントマップには載せず DDX だけで受ける**——チェックを
//	切り替えても他のコントロールを動かす必要が無い（シンボルの行のチェックは選択肢を灰色に
//	するためにイベントを要る）。ID は kFirstMergeID から 1 つずつ。
//
//	【項目は図面のシンボル定義そのもの】どちらの形でも候補は図面に実在するシンボルだけ。
//	行ごとの「取り込む」チェックがあるのでそれで足りる——置くものが図面に無いなら、その要素は
//	チェックを外せばよい（core/ImportOptions.h、docs/DEV-NOTES.md「取り込み設定の決め事」）。
//	ただし**一覧をそのまま出さない**——`BuildList(kSymDefNode)` は VectorWorks 自身が
//	プラグインオブジェクトのスタイルとして持っている定義まで返すので、`GetSymbolDefSubType`
//	で拾い分ける（下記 SymbolSubType）。
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
#include "draw/DrawUtil.h"
#include "core/Document.h"
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
		// 【行の数】役割の数 ＋ 図面枠スタイルの 1 行 ＋ 寸法規格の 1 行（冒頭「図面枠の
		// 行は…」「その下の 1 行は寸法規格」）。どちらも役割の 2 列の下に置く。
		constexpr std::size_t kTitleBlockRow = core::kSymbolRoleCount;
		constexpr std::size_t kDimensionRow = core::kSymbolRoleCount + 1;
		constexpr std::size_t kRowCount = core::kSymbolRoleCount + 2;

		// 図面枠スタイルのシンボル定義サブタイプ。**0 以外はプラグインオブジェクトの
		// スタイル**で、値はその PIO の型（552 = 図面枠）。上記 Findings「シンボル」の実測表。
		// **型は Sint32**——`GetSymbolDefSubType` の戻り値がそれで、short で受けると
		// 縮小変換になる（clang-tidy の bugprone-narrowing-conversions。CI の tidy-mac）。
		constexpr Sint32 kTitleBlockStyleSubType = 552;

		// コントロール ID。1 = OK / 2 = キャンセルは SDK の予約。行 i は
		// [チェック, 説明, 選択, 絵] の 4 つを kFirstRowID から 4 つ刻みで使う
		// （絵は退避の形でだけ作る。ID は形に依らず固定にしておく）。
		constexpr TControlID kIntroID = 3;
		// 伏図のまとめ方の見出しと、その下のチェック（冒頭「いちばん下は伏図のまとめ方」）。
		// シンボルの行（kFirstRowID から kRowStride 刻み）と重ならない所から振る。
		constexpr TControlID kMergeIntroID = 4;
		// 垂木の断面の欄（冒頭「その下は垂木の断面」）。見出し・幅・「×」・せいの 4 つ。
		// シンボルの行（kFirstRowID = 10 から）より手前の空いた番号を使う。
		constexpr TControlID kRafterLabelID = 5;
		constexpr TControlID kRafterWidthID = 6;
		constexpr TControlID kRafterTimesID = 7;
		constexpr TControlID kRafterHeightID = 8;
		static_assert(kRafterHeightID < 10, "シンボルの行の ID（kFirstRowID）と重ねないこと");
		constexpr TControlID kFirstMergeID = 200;

		constexpr TControlID mergeID(std::size_t index)
		{
			return static_cast<TControlID>(kFirstMergeID + index);
		}
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
		// 垂木の断面の入力欄の幅（標準文字幅）。"1000.5" が収まれば足りる。
		constexpr short kRafterSizeWidthChars = 8;

		// 【行を 2 列に折る】1 行の高さはサムネイルの高さで決まり、**その大きさは選べない**
		// （`ThumbnailSizeType` は kStandardSize / kLineTypeSize の 2 つだけで、後者は
		// 線種用の細長い枠。[SDK リファレンス「レイアウトダイアログ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)）。
		// 役割の数（8）をそのまま縦に積むと画面の高さに対して細長くなりすぎるので、
		// **列に折って高さを半分にする**。列の数を増やすときはこの定数だけを変える
		// （割り切れない分は最後の列が短くなる）。
		constexpr std::size_t kColumnCount = 2;
		constexpr std::size_t kRowsPerColumn =
			(core::kSymbolRoleCount + kColumnCount - 1) / kColumnCount;

		// 列と列の間隔（標準文字幅）。0 だと左の列の絵と右の列のチェックがくっつく。
		constexpr short kColumnGapChars = 2;

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
		struct CandidateList
		{
			std::vector<std::string> names;
			std::vector<std::size_t> listIndices;

			void clear()
			{
				names.clear();
				listIndices.clear();
			}
		};

		// 候補は 2 組ある（冒頭「図面枠の行はシンボルではない」）——普通のシンボル
		// 定義（シンボルを置く行）と、図面枠スタイル（図面枠の行）。**元の一覧は同じ
		// 1 つ**で、subType で拾い分けるだけ。
		struct SymbolResources
		{
			VWFC::Tools::VWResourceList list;
			CandidateList symbols;	   // 普通のシンボル定義（subType 0）
			CandidateList titleBlocks; // 図面枠スタイル（subType 552）
			// 寸法規格（M31）。資源ではないので names だけを持つ（listIndices は空）。
			CandidateList dimensionStandards;
		};

		// そのシンボル定義のサブタイプ。
		//
		// 【なぜ要るか】`BuildList(kSymDefNode)` は図面のシンボル定義を**全部**返すので、
		// VectorWorks 自身がプラグインオブジェクトのスタイルとして持っている定義
		// （図面枠・データタグ・図面ラベル・立断面指示線・グラフィック凡例・木質構造材…）
		// まで並ぶ。シンボルを置く行の選択肢に出しても置けるものではないので外し、
		// **図面枠のスタイルだけは図面枠の行の選択肢に使う**（冒頭「図面枠の行は…」）。
		//
		// 切り分けは `GetSymbolDefSubType`——**0 なら普通のシンボル定義、0 以外はその
		// プラグインオブジェクトのスタイル**（値は PIO の型。図面枠は 552）。フォルダ名では
		// 切り分けられない（"…スタイル" フォルダに入らないものがある）し、2D/3D/ハイブリッド
		// の別も無関係
		// （[SDK リファレンス「シンボル」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Symbols.md)
		// の実測表）。読めなければ「どちらでもない」側へ倒す（-1）。
		Sint32 SymbolSubType(MCObjectHandle definition)
		{
			if (definition == nil)
				return -1;
			return gSDK->GetSymbolDefSubType(definition);
		}

		// いま開いている図面の**置けるシンボル定義**と**図面枠スタイル**（どちらも名前順）。
		// 読めなければ空（＝どの要素も取り込めない。ダイアログ自体は出せる）。
		SymbolResources CollectSymbolResources()
		{
			SymbolResources resources;
			try
			{
				const std::size_t count = resources.list.BuildList(kSymDefNode, true);
				for (std::size_t i = 0; i < count; ++i)
				{
					// **拾うのは 2 通りだけ。** ほかの PIO スタイル（データタグ・図面
					// ラベル・グラフィック凡例…）はどちらの行にも出さない。
					const Sint32 subType = SymbolSubType(resources.list.GetResource(i));
					CandidateList* into = nullptr;
					if (subType == 0)
						into = &resources.symbols;
					else if (subType == kTitleBlockStyleSubType)
						into = &resources.titleBlocks;
					if (into == nullptr)
						continue;

					TXString name;
					resources.list.GetResourceName(i, name);
					std::string text = static_cast<const char*>(name);
					if (text.empty())
						continue; // 名前で選ばせる以上、名前の無い定義は候補にしない
					into->names.push_back(std::move(text));
					into->listIndices.push_back(i);
				}
			}
			catch (...)
			{
				// リソース一覧を作れない図面でも設定ダイアログ自体は出す（候補が空になり、
				// どの要素にもチェックが入らない）。1 つの失敗で取り込みの入口を塞がない。
				// **途中まで採れていた候補は捨てる**——半端な一覧は項目と対応しない。
				resources.symbols.clear();
				resources.titleBlocks.clear();
			}
			return resources;
		}

		// 図面の寸法規格の名前（組み込み → カスタムの順。冒頭「その下の 1 行は寸法規格」）。
		// index を総当たりして、名前が引けたものだけを並べる——カスタムの並びが 0 から
		// 詰まっているとは限らないので、本数から index を引き算で出さない（Findings）。
		CandidateList CollectDimensionStandards()
		{
			CandidateList standards;
			try
			{
				for (auto& standard : DimensionStandards())
					standards.names.push_back(std::move(standard.second));
			}
			catch (...)
			{
				// 読めなければ寸法規格の行だけが選べない（ほかの行は使える）。
				standards.clear();
			}
			return standards;
		}

		// 伏図のまとめ方のチェック 1 つ。key を「前のレベルとまとめる」かを問う。
		struct MergeRow
		{
			core::PlanLevelKey key;
			std::string label;
		};

		// 候補からチェックの行を作る。**まとめる相手の居ない高さ（階の最も低い高さ）は
		// 並べない**——問う意味が無い（parse/PlanLevel はそれを無視する）。
		std::vector<MergeRow> MergeRows(const std::vector<core::PlanLevelChoice>& choices)
		{
			std::vector<MergeRow> rows;
			for (std::size_t k = 1; k < choices.size(); ++k)
			{
				const core::PlanLevelChoice& choice = choices[k];
				if (!choice.canMerge)
					continue;
				// 前のレベル＝同じ階で 1 つ低い高さ（候補は階・高さの昇順）。
				const core::PlanLevelChoice& previous = choices[k - 1];
				rows.push_back(MergeRow{choice.key, choice.planTitle + ": " + choice.heightText +
														" を " + previous.heightText +
														" と同じ伏図にまとめる"});
			}
			return rows;
		}

		// 役割の並びは表の順（core::symbolRoles()）。行番号 → 役割。**図面枠の行
		// （kTitleBlockRow）には役割が無い**ので、呼ぶ前に行を確かめること。
		core::SymbolRole roleAt(std::size_t row)
		{
			return core::symbolRoles()[row].role;
		}

		// 行の説明（下の 2 行だけ役割の表に載らない）。
		const char* rowLabel(std::size_t row)
		{
			if (row == kTitleBlockRow)
				return "図面枠スタイル";
			if (row == kDimensionRow)
				return "寸法規格";
			return core::symbolRoleLabel(roleAt(row));
		}

		// 取り込み設定ダイアログ 1 枚。行は**役割の数 ＋ 図面枠スタイルと寸法規格の 2 行**で、
		// 役割の増減は core/ImportOptions.h の表に従う（**イベントマップだけはコンパイル時の
		// ID が要る**ので、下の static_assert が「表を増やしたらここも増やせ」と教える）。
		//
		// **行の違いは「候補がどの組か」と「名前で選ぶか」だけ**に閉じてある（Candidates /
		// HasPulldown / HasPreview）——作り方・埋め方・選択の読み取りは全行で同じコードが通る。
		class CImportSettingsDialog : public VWDialog
		{
		public:
			// resources は値で受ける（形を変えて開き直すことがあるので、呼び出し側は同じ
			// 一覧を持ったまま。VWResourceList は参照カウント付きでコピーできる）。
			// decided … 設定を利用者がこの起動中に一度でも決めたか（OK で閉じたか）。
			// まだなら図面枠と寸法規格は「置く／入れる」で開く（下記）。
			CImportSettingsDialog(const core::ImportOptions& seed, SymbolResources resources,
								  std::vector<MergeRow> mergeRows, Form form, bool decided)
				: fIntro(kIntroID), fRafterLabel(kRafterLabelID), fRafterWidth(kRafterWidthID),
				  fRafterTimes(kRafterTimesID), fRafterHeight(kRafterHeightID),
				  fMergeIntro(kMergeIntroID), fResources(std::move(resources)),
				  fMergeRows(std::move(mergeRows)), fForm(form), fSeedRafterWidth(seed.rafterWidth),
				  fSeedRafterHeight(seed.rafterHeight),
				  fRafterWidthText(core::formatRafterSize(seed.rafterWidth).c_str()),
				  fRafterHeightText(core::formatRafterSize(seed.rafterHeight).c_str())
			{
				// 伏図のまとめ方は前回の選択（同じ階・同じ高さ）を初期値にする。既定は
				// まとめない（高さごとに 1 枚。ご要望）。
				for (std::size_t k = 0; k < fMergeRows.size(); ++k)
				{
					fMergeChecks.emplace_back(mergeID(k));
					fMergeStates.push_back(seed.mergesWithPrevious(fMergeRows[k].key));
				}
				for (std::size_t row = 0; row < kRowCount; ++row)
				{
					fChecks.emplace_back(checkID(row));
					fLabels.emplace_back(labelID(row));
					// **3 種類とも全行ぶん持つ**（作るのは使うものだけ。添字を行番号と
					// 揃えるため）。サムネイルとプルダウンは同じ ID だが、1 行で作るのは
					// どちらか一方だけ。
					fThumbs.emplace_back(popupID(row));
					fPopups.emplace_back(popupID(row));
					fPreviews.emplace_back(previewID(row));

					// **いまの対応先が図面にある行だけを「取り込む」で開く。** 無い名前は
					// 項目にできない（＝置きようがない）ので、チェックを外した状態にする。
					//
					// 【図面枠と寸法規格は初期値が「置く／入れる」】図面に候補があるなら、
					// **まだ決めていないうちは候補の 1 つで置く**を初期値にする（ご要望）。
					// 図面枠は一覧の最初のスタイル、寸法規格は「JIS」があればそれ・無ければ
					// 一覧の最初（DefaultIndex）。前回選んだものがいまの図面に無いときも
					// そこへ寄せる——名前は図面ごとに違うので、名前が合わないことを
					// 「置かない」理由にしない。前回チェックを外して閉じたならそれに従う。
					// 設定ダイアログを出さずに取り込むとき（core::ImportOptions の既定）は
					// 従来どおりどちらも置かない。
					const bool defaultsOn = row == kTitleBlockRow || row == kDimensionRow;
					const bool wanted =
						defaultsOn ? (!decided || SeedEnabled(seed, row)) : SeedEnabled(seed, row);
					const std::string& current = SeedName(seed, row);
					const std::size_t count = Candidates(row).names.size();
					std::size_t index = IndexOf(row, current);
					if (defaultsOn && wanted && index >= count && count > 0)
						index = DefaultIndex(row);
					const bool valid = index < count;
					fSelection[row] = valid ? index : 0;
					fEnabled[row] = wanted && valid;
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
				for (std::size_t row = 0; row < kRowCount; ++row)
				{
					const std::vector<std::string>& names = Candidates(row).names;
					const std::size_t index = fSelection[row];
					const bool valid = index < names.size();
					if (row == kTitleBlockRow)
					{
						// 図面枠は**空文字が「置かない」**（core/ImportOptions.h）。
						// チェックが無い・選べるものが無いときは空のまま返す。
						options.setTitleBlockStyle(fEnabled[row] && valid ? names[index]
																		  : std::string());
						continue;
					}
					if (row == kDimensionRow)
					{
						// 寸法規格も**空文字が「入れない」**（図面枠と同じ）。
						options.setDimensionStandard(fEnabled[row] && valid ? names[index]
																			: std::string());
						continue;
					}
					options.setEnabled(roleAt(row), fEnabled[row] && valid);
					if (valid)
						options.setSymbol(roleAt(row), names[index]);
				}
				for (std::size_t k = 0; k < fMergeRows.size(); ++k)
					options.setMergeWithPrevious(fMergeRows[k].key, fMergeStates[k]);
				// 垂木の断面。読めない欄は前回の値のまま（冒頭「その下は垂木の断面」）。
				options.setRafterSize(
					core::parseRafterSize(Text(fRafterWidthText)).value_or(fSeedRafterWidth),
					core::parseRafterSize(Text(fRafterHeightText)).value_or(fSeedRafterHeight));
				return options;
			}

			// 垂木の断面の欄に読めない値があったか（ログへ出す 1 行ぶん。無ければ空）。
			// Result と同じく OK で閉じた後に呼ぶ（DDX が欄の文字を写した後）。
			std::string RafterNote() const
			{
				std::string note;
				const auto check = [&note](const char* what, const TXString& text, double kept)
				{
					if (core::parseRafterSize(Text(text)).has_value())
						return;
					if (!note.empty())
						note += " / ";
					note += std::string("垂木の") + what + "「" + Text(text) +
							"」を読めませんでした（" + core::formatRafterSize(kept) +
							" mm のまま取り込みます）";
				};
				check("幅", fRafterWidthText, fSeedRafterWidth);
				check("せい", fRafterHeightText, fSeedRafterHeight);
				return note;
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
					fResources.symbols.names.empty()
						? "この図面にはシンボルが登録されていないため、シンボルで置く要素は"
						  "取り込めません。"
						: "取り込む要素にチェックを入れ、置くシンボル（下の 2 行は各シート"
						  "レイヤへ置く図面枠のスタイルと、伏図・軸組図へ入れる寸法の寸法規格）"
						  "を選んでください。垂木の断面は全垂木に一律で使います。";
				if (!fIntro.CreateControl(this, intro))
				{
					fNote = "説明文を作れませんでした";
					return false;
				}
				this->AddFirstGroupControl(&fIntro);

				for (std::size_t row = 0; row < kRowCount; ++row)
				{
					// チェックは文字を持たない（行の名前は隣の説明が出す）——文字を
					// 持たせると幅が行ごとに変わり、右の列が揃わない。
					if (!fChecks[row].CreateControl(this, ""))
					{
						fNote = "チェックを作れませんでした";
						return false;
					}
					if (!fLabels[row].CreateControl(this, rowLabel(row), kLabelWidthChars))
					{
						fNote = "説明を作れませんでした";
						return false;
					}
					if (!CreateSelector(row))
						return false;

					// 行の頭（チェック）の置き場所は 4 通り。**図面枠の行**は 2 列の下へ
					// 1 行ぶん空けて置く（役割の列に混ぜると、列の折り返しがずれるうえに
					// 「シンボルではないもの」がシンボルの列に紛れる）。置き先は**左の列の
					// いちばん下**——列は左から埋まるので、そこが必ず最後まで埋まっている。
					// **列の先頭**は、1 列目なら説明文の下（ここだけ 1 行ぶん空ける）、
					// 2 列目以降なら**前の列の先頭行の右端の右**——列の頭どうしを揃えると、
					// 行の高さが全列で同じなので以降の行も自然に揃う。**列の途中**は
					// 1 つ上の行の頭の下で、行間は空けない（絵が文字より背が高いぶん、
					// 詰めても窮屈にならない）。
					if (row == kTitleBlockRow)
						this->AddBelowControl(&fChecks[kRowsPerColumn - 1], &fChecks[row], 0, 1);
					else if (row == kDimensionRow)
						this->AddBelowControl(&fChecks[kTitleBlockRow], &fChecks[row]);
					else if (row % kRowsPerColumn != 0)
						this->AddBelowControl(&fChecks[row - 1], &fChecks[row]);
					else if (row == 0)
						this->AddBelowControl(&fIntro, &fChecks[row], 0, 1);
					else
						this->AddRightControl(RowTail(row - kRowsPerColumn), &fChecks[row],
											  kColumnGapChars);

					// 行の中身はチェックの右へ順に。
					this->AddRightControl(&fChecks[row], &fLabels[row]);
					if (!HasPulldown(row))
						this->AddRightControl(&fLabels[row], &fThumbs[row]);
					else
					{
						this->AddRightControl(&fLabels[row], &fPopups[row]);
						if (HasPreview(row))
							this->AddRightControl(&fPopups[row], &fPreviews[row]);
					}
				}
				if (!CreateRafterSize())
					return false;
				return CreateMergeRows();
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				// **中身を入れる前に「出た」ことにする。** ここから先で例外が出ても、
				// 呼び出し側は「組めなかった」ではなく「この形では駄目だった」と分かる。
				fShown = true;
				try
				{
					for (std::size_t row = 0; row < kRowCount; ++row)
					{
						FillSelector(row);
						fChecks[row].SetState(fEnabled[row]);
						UpdateRow(row);
					}
					for (std::size_t k = 0; k < fMergeRows.size(); ++k)
						fMergeChecks[k].SetState(fMergeStates[k]);
					// 初期値は自分でも入れる（DDX が流し込む前提に寄りかからない）。
					fRafterWidth.SetText(fRafterWidthText);
					fRafterHeight.SetText(fRafterHeightText);
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
			// 選択だけは OnDefaultButtonEvent で読む。冒頭「選択は名前で引き取る」）。
			void OnDDXInitialize() override
			{
				for (std::size_t row = 0; row < kRowCount; ++row)
				{
					this->AddDDX_CheckButton(checkID(row), &fEnabled[row]);
					if (HasPulldown(row))
						this->AddDDX_PulldownMenu(popupID(row), &fSelection[row]);
				}
				// 伏図のまとめ方は DDX だけで受ける（冒頭「いちばん下は伏図のまとめ方」）。
				for (std::size_t k = 0; k < fMergeRows.size(); ++k)
					this->AddDDX_CheckButton(mergeID(k), &fMergeStates[k]);
				// 垂木の断面も DDX だけで受ける（打ち込んでも他のコントロールは動かさない）。
				this->AddDDX_EditText(kRafterWidthID, &fRafterWidthText);
				this->AddDDX_EditText(kRafterHeightID, &fRafterHeightText);
			}

			// OK が押された。**閉じる前に**サムネイルの選択を控える（閉じた後のコントロール
			// からは読めない）。
			void OnDefaultButtonEvent() override
			{
				if (fForm == Form::Thumbnail)
				{
					try
					{
						for (std::size_t row = 0; row < kRowCount; ++row)
							if (!HasPulldown(row))
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
				for (std::size_t row = 0; row < kRowCount; ++row)
				{
					if (popupID(row) != controlID)
						continue;
					if (!HasPulldown(row))
						return; // サムネイルの行では、この ID のコントロールは別物
					fSelection[row] = fPopups[row].GetSelectedIndex();
					UpdateRow(row);
					return;
				}
			}

			// チェックが変わったら、その行の選択肢を有効／無効にする（取り込まない行が
			// 見て分かるように）。
			void OnEnabledChanged(TControlID controlID, VWDialogEventArgs& /*eventArgs*/)
			{
				for (std::size_t row = 0; row < kRowCount; ++row)
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
			// **その行の候補**（冒頭「図面枠の行はシンボルではない」）。ここだけが
			// 行による違いで、以降の作り方・埋め方・読み取りは全行で同じ。
			const CandidateList& Candidates(std::size_t row) const
			{
				if (row == kTitleBlockRow)
					return fResources.titleBlocks;
				if (row == kDimensionRow)
					return fResources.dimensionStandards;
				return fResources.symbols;
			}

			// その行を**名前のプルダウン**で選ぶか。退避の形は全行、本命の形でも寸法規格の
			// 行だけ（資源ではないのでサムネイルにできない。冒頭「その下の 1 行は寸法規格」）。
			bool HasPulldown(std::size_t row) const
			{
				return fForm == Form::NameList || row == kDimensionRow;
			}

			// その行に**絵**を出すか（退避の形のシンボルの行だけ。寸法規格には絵が無い）。
			bool HasPreview(std::size_t row) const
			{
				return fForm == Form::NameList && row != kDimensionRow;
			}

			// 前回の設定からその行の「取り込む」と名前を引く。
			static bool SeedEnabled(const core::ImportOptions& seed, std::size_t row)
			{
				if (row == kTitleBlockRow)
					return seed.hasTitleBlock();
				if (row == kDimensionRow)
					return seed.hasDimensions();
				return seed.isEnabled(roleAt(row));
			}

			// 初期値が「置く」の行（図面枠・寸法規格）で、前回の名前が使えないときに選ぶ
			// 候補。寸法規格は「JIS」を優先する（core::defaultDimensionStandardIndex。
			// 自動の 1 周目の既定の設定と同じものを選ぶよう、選び方は core に 1 つだけ置く）。
			// 図面枠には決め手が無いので最初。
			std::size_t DefaultIndex(std::size_t row) const
			{
				if (row == kDimensionRow)
					return core::defaultDimensionStandardIndex(Candidates(row).names);
				return 0;
			}

			static const std::string& SeedName(const core::ImportOptions& seed, std::size_t row)
			{
				if (row == kTitleBlockRow)
					return seed.titleBlockStyle();
				if (row == kDimensionRow)
					return seed.dimensionStandard();
				return seed.symbol(roleAt(row));
			}

			// その行の**右端**のコントロール（次の列を右へ置くときの相手）。何が右端かは
			// 形で変わる——サムネイルの形は選択そのもの、名前の形は隣に出す絵。
			VWControl* RowTail(std::size_t row)
			{
				if (!HasPulldown(row))
					return &fThumbs[row];
				if (!HasPreview(row))
					return &fPopups[row];
				return &fPreviews[row];
			}

			// 行の選択コントロールを作る（形で中身が変わる唯一の場所）。
			bool CreateSelector(std::size_t row)
			{
				if (!HasPulldown(row))
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
				if (!HasPreview(row))
					return true;
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
				const CandidateList& candidates = Candidates(row);
				const bool valid = fSelection[row] < candidates.names.size();
				if (!HasPulldown(row))
				{
					VWThumbnailPopupCtrl& popup = fThumbs[row];
					// 項目はリソース一覧の **ID と（一覧側の）添字**で足す（絵は VW が引く）。
					// **候補だけを候補の順に足す**ので、項目 i ＝ 候補 i になる。
					const Sint32 listID = fResources.list.GetListID();
					for (const std::size_t listIndex : candidates.listIndices)
						popup.AddImageFromResource(listID, listIndex);
					if (valid)
						popup.SelectItem(fSelection[row]);
					return;
				}
				VWPullDownMenuCtrl& popup = fPopups[row];
				for (const std::string& name : candidates.names)
					popup.AddItem(TXString(name.c_str()));
				if (valid)
					popup.SelectIndex(fSelection[row]);
			}

			// サムネイルで選ばれている項目を**名前で**引き当て、候補の添字にして返す
			// （冒頭「選択は名前で引き取る」）。名前が引けなければ項目の添字に落とし、
			// それも範囲外なら開いたときの選択のまま返す。
			std::size_t SelectedIndexOf(std::size_t row) const
			{
				const std::vector<std::string>& names = Candidates(row).names;
				const VWThumbnailPopupCtrl& popup = fThumbs[row];
				TXString name;
				gSDK->InternalIndexToNameN(popup.GetSelectedItem(), name);
				const std::string text = static_cast<const char*>(name);
				if (!text.empty())
				{
					const std::size_t byName = IndexOf(row, text);
					if (byName < names.size())
						return byName;
				}
				const std::size_t byIndex = popup.GetSelectedItemIndex();
				return byIndex < names.size() ? byIndex : fSelection[row];
			}

			// 名前 → その行の項目の添字。無ければ項目の数（＝範囲外）を返す。
			std::size_t IndexOf(std::size_t row, const std::string& value) const
			{
				const std::vector<std::string>& names = Candidates(row).names;
				for (std::size_t i = 0; i < names.size(); ++i)
					if (names[i] == value)
						return i;
				return names.size();
			}

			// TXString（UTF-8）→ std::string。
			static std::string Text(const TXString& text)
			{
				return static_cast<const char*>(text);
			}

			// 垂木の断面の欄を作る。寸法規格の行の下へ 1 行ぶん空けて、見出し・幅・「×」・
			// せいを横に並べる（冒頭「その下は垂木の断面」）。
			bool CreateRafterSize()
			{
				if (!fRafterLabel.CreateControl(this, "垂木の断面（幅×せい mm）"))
				{
					fNote = "垂木の断面の見出しを作れませんでした";
					return false;
				}
				this->AddBelowControl(&fChecks[kDimensionRow], &fRafterLabel, 0, 1);
				if (!fRafterWidth.CreateControl(this, "", kRafterSizeWidthChars, 1) ||
					!fRafterTimes.CreateControl(this, "×") ||
					!fRafterHeight.CreateControl(this, "", kRafterSizeWidthChars, 1))
				{
					fNote = "垂木の断面の欄を作れませんでした";
					return false;
				}
				this->AddRightControl(&fRafterLabel, &fRafterWidth);
				this->AddRightControl(&fRafterWidth, &fRafterTimes);
				this->AddRightControl(&fRafterTimes, &fRafterHeight);
				return true;
			}

			// 伏図のまとめ方の欄を作る（候補が無ければ何も作らない）。垂木の断面の欄の下へ
			// 1 行ぶん空けて見出し、その下にチェックを 1 つずつ縦に並べる——行の数は IFC
			// 次第なので、2 列に折る役割の行とは混ぜない。
			bool CreateMergeRows()
			{
				if (fMergeRows.empty())
					return true;
				if (!fMergeIntro.CreateControl(
						this, "伏図のまとめ方（伏図は横架材の高さごとに 1 枚作ります。"
							  "チェックした高さは前の高さと同じ伏図にまとめます）"))
				{
					fNote = "伏図のまとめ方の見出しを作れませんでした";
					return false;
				}
				this->AddBelowControl(&fRafterLabel, &fMergeIntro, 0, 1);
				for (std::size_t k = 0; k < fMergeRows.size(); ++k)
				{
					if (!fMergeChecks[k].CreateControl(this, TXString(fMergeRows[k].label.c_str())))
					{
						fNote = "伏図のまとめ方のチェックを作れませんでした";
						return false;
					}
					if (k == 0)
						this->AddBelowControl(&fMergeIntro, &fMergeChecks[k]);
					else
						this->AddBelowControl(&fMergeChecks[k - 1], &fMergeChecks[k]);
				}
				return true;
			}

			// その行の見た目を今の状態に合わせる。**選ぶものが無い行は常に無効**——選べる
			// ものが無いのにチェックできると、「取り込むと言ったのに何も置かれない」ことになる。
			void UpdateRow(std::size_t row)
			{
				const std::vector<std::string>& names = Candidates(row).names;
				// **候補は行ごとに数える**——図面枠スタイルが 1 つも無い図面でも
				// シンボルの行は選べるし、その逆もある。
				const bool hasItems = !names.empty();
				this->EnableControl(checkID(row), hasItems);
				this->EnableControl(popupID(row), hasItems && fEnabled[row]);
				if (!HasPreview(row))
					return;
				// 退避の形だけは絵が別のコントロールなので、選択に追随させる。
				const std::size_t index = fSelection[row];
				const TXString name =
					index < names.size() ? TXString(names[index].c_str()) : TXString("");
				fPreviews[row].Update(name, kPreviewRenderMode, kPreviewView);
				this->EnableControl(previewID(row), hasItems && fEnabled[row]);
			}

			VWStaticTextCtrl fIntro;
			// 垂木の断面（冒頭「その下は垂木の断面」）。
			VWStaticTextCtrl fRafterLabel;
			VWEditTextCtrl fRafterWidth;
			VWStaticTextCtrl fRafterTimes;
			VWEditTextCtrl fRafterHeight;
			// **deque に直接作る。** 行数ぶんのコントロールを溜めるが、vector だと追加の
			// たびに既存の要素が動いてしまう（ダイアログは生存中ずっとコントロールの
			// アドレスを持つ）。deque は追加しても既存の要素を動かさない
			// （draw/ResultDialog.cpp の本文行と同じ理由）。
			std::deque<VWCheckButtonCtrl> fChecks;
			std::deque<VWStaticTextCtrl> fLabels;
			std::deque<VWThumbnailPopupCtrl> fThumbs; // サムネイルの行だけ作る
			std::deque<VWPullDownMenuCtrl> fPopups; // 名前で選ぶ行だけ作る（HasPulldown）
			std::deque<VWSymbolDisplayCtrl> fPreviews; // 絵を出す行だけ作る（HasPreview）
			// 伏図のまとめ方（冒頭「いちばん下は伏図のまとめ方」）。コントロールも状態も
			// **deque**——DDX とダイアログがアドレスを持ち続けるので、動かしてはいけない
			// （deque<bool> は vector<bool> と違って本物の bool を並べる）。
			VWStaticTextCtrl fMergeIntro;
			std::deque<VWCheckButtonCtrl> fMergeChecks;
			std::deque<bool> fMergeStates;
			SymbolResources fResources; // 項目の元（ダイアログより長生きさせない）
			std::vector<MergeRow> fMergeRows;
			Form fForm = Form::Thumbnail;
			std::array<std::size_t, kRowCount> fSelection = {};
			std::array<bool, kRowCount> fEnabled = {};
			// 垂木の断面: 開いたときの値（読めない欄はこれに戻す）と、欄の文字（DDX の受け口）。
			double fSeedRafterWidth = core::kDefaultRafterWidth;
			double fSeedRafterHeight = core::kDefaultRafterHeight;
			TXString fRafterWidthText;
			TXString fRafterHeightText;
			bool fShown = false;
			bool fAborted = false;
			std::string fNote;
		};

		// 行を 1 つ足したら、下のイベントマップにも 2 行足すこと（コントロールの ID は
		// コンパイル時の定数でなければならないので、ここだけは表から回せない）。
		// **図面枠の行（kTitleBlockRow）もイベントマップに要る**ので、数えるのは
		// 役割の数ではなく行の数。
		static_assert(kRowCount == 10,
					  "行を増減したら CImportSettingsDialog のイベントマップも直すこと");

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
		ADD_DISPATCH_EVENT(checkID(7), OnEnabledChanged); // 継手（M33）
		ADD_DISPATCH_EVENT(checkID(8), OnEnabledChanged); // 図面枠スタイル
		ADD_DISPATCH_EVENT(checkID(9), OnEnabledChanged); // 寸法規格
		ADD_DISPATCH_EVENT(popupID(0), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(1), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(2), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(3), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(4), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(5), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(6), OnSymbolChanged);
		ADD_DISPATCH_EVENT(popupID(7), OnSymbolChanged); // 継手（M33）
		ADD_DISPATCH_EVENT(popupID(8), OnSymbolChanged); // 図面枠スタイル
		ADD_DISPATCH_EVENT(popupID(9), OnSymbolChanged); // 寸法規格
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

		// 設定を利用者がこの起動中に一度でも決めたか（OK で閉じたか）。
		// **図面枠・寸法規格は空文字が「置かない」も「まだ決めていない」も兼ねる**
		// （core/ImportOptions.h）ので、初期値を「置く」にするにはこの区別を別に持つ。
		bool& SettingsDecided()
		{
			static bool decided = false;
			return decided;
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

	bool presetImportSettings(core::ImportOptions& options, std::string* note)
	{
		try
		{
			SymbolResources resources = CollectSymbolResources();
			resources.dimensionStandards = CollectDimensionStandards();
			options =
				core::presetImportOptions(resources.symbols.names, resources.titleBlocks.names,
										  resources.dimensionStandards.names);
			return true;
		}
		catch (...)
		{
			// 集められなければ組まない（呼び出し側は ImportOptions の既定で続ける。
			// draw/Feedback.cpp の runTestRound）。
			AddNote(note, "図面からシンボル・図面枠・寸法規格を集められませんでした");
			return false;
		}
	}

	SettingsOutcome showImportSettings(core::ImportOptions& options,
									   const std::vector<core::PlanLevelChoice>& planLevels,
									   std::string* note)
	{
		try
		{
			core::ImportOptions& remembered = RememberedOptions();
			SymbolResources resources = CollectSymbolResources();
			resources.dimensionStandards = CollectDimensionStandards();

			// **本命（サムネイル）→ 退避（名前＋絵）の順に試す。** 前者で出せなかった
			// ときだけ後者へ落ちる（冒頭「2 つの形を持ち、出せた方を使う」）。
			for (const Form form : {Form::Thumbnail, Form::NameList})
			{
				CImportSettingsDialog dialog(remembered, resources, MergeRows(planLevels), form,
											 SettingsDecided());
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
				AddNote(note, dialog.RafterNote());
				SettingsDecided() = true;
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
