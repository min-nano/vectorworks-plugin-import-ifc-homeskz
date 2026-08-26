//
//	draw/Legend.cpp
//
//	グラフィック凡例の設置の実装。意図・規約は draw/Legend.h と core/Document.h の
//	LegendCommand を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API:
//	  * gSDK->DefineCustomObject("GraphicLegend", kCustomObjectPrefNever) … 設定ダイアログ抑止
//	  * gSDK->CreateCustomObject("GraphicLegend", 位置, 0, bInsert)      … 凡例 PIO の生成
//	  * gSDK->SetCurrentLayer(sheetLayer)                                … 置き場所（用紙）の指定
//	  * gSDK->SetLineWeight / SetFillPat                                 … 見た目（線の太さ・塗り）
//	  * gSDK->GetObjectInternalIndex(viewport)                           … フィルタ先の参照
//	  * gSDK->TaggedDataCreate / TaggedDataSet          … フィルタとソース定義の書き込み
//	  * gSDK->ResetObject                                                … 反映（中身の計算）
//
//	**スタイルは扱わない**（draw/Legend.h の ★）。`SetPluginObjectStyle` も
//	`UpdateStyledObjects` も呼ばない——スタイル無しで置くので、中身は `ResetObject` の時点で
//	決まる。
//

#include "PluginPrefix.h"
#include "draw/Legend.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// グラフィック凡例の内部プラグイン名。
		// **表示名「グラフィック凡例」とは別物**で、登録名はスペース無しの "GraphicLegend"。
		constexpr const char* kGraphicLegendPlugin = "GraphicLegend";

		// 箱幅パラメータ。凡例は矩形モードの PIO なので、点で生成すると幅 0 のまま潰れる
		// （draw/Legend.h）。用紙上（ドキュメント単位 mm）の適当な幅を与えて可視化し、
		// **ローカルの VW で最終調整する**。高さは行の内容から自動で決まるので与えない。
		constexpr const char* kFieldBoxWidth = "BoxWidth";
		constexpr double kBoxWidth = 150.0;

		// 写したソース定義（下記 kSourceDefinition）に含まれるイメージの縮率。**書き換える
		// 手立てがまだ無い**ので、伏図の縮尺がこれと違えば診断で知らせるだけにする
		// （draw/Legend.h の scaleMismatch）。比較は許容付き——縮尺は表示レイヤから読んだ
		// double なので、丸め誤差で「違う」と言い出さないようにする。
		constexpr double kSourceImageScale = 50.0;
		constexpr double kScaleEps = 1e-6;

		// 見た目。凡例 PIO が内部で描く枠線・セルは**クラスでは制御できない**ので、
		// オブジェクトの属性として直接与える（draw/Legend.h）。線の太さの単位はミル（1/1000
		// インチ）で、5 ミル = 0.127mm を VW は 0.13mm と表示する。塗りパターン 0 = なし。
		constexpr short kLineWeightMils = 5;
		constexpr InternalIndex kFillNone = 0;

		// 生成した凡例の箱幅を与える。**例外を外へ出さない**——書けなくても凡例そのものは
		// 図面に残るので、件数だけ counts へ積む。
		//
		// **縮率（イメージの縮率）はここでは書けない。** レコードには `ImageScale` という
		// 実数の欄があるが、**表示名が空＝OIP に出ない別物**で、実機ではデザインレイヤの
		// 縮尺（100）が入ったまま OIP は 1:50 を表示していた。縮率は「イメージの定義...」
		// ダイアログ側＝レコードの外にある（docs/DEV-NOTES.md「グラフィック凡例」）。
		// パラメータ名を当てにいく実装はもう試して外れているので、書き直さないこと。
		void ApplyBoxWidth(MCObjectHandle object, LegendCounts& counts)
		{
			try
			{
				VWParametricObj pio(object);
				if (!SetParamRealChecked(pio, TXString(kFieldBoxWidth), kBoxWidth))
					++counts.widthLeft;
			}
			catch (...)
			{
				// PIO として開けなかった（＝箱幅を書けていない）。幅 0 のまま潰れるだけで
				// 凡例自体は図面に残るので、続ける。
				++counts.paramsFailed;
			}
		}

		// 「ビューポートでフィルタ」の保存先（draw/Legend.h 冒頭・docs/DEV-NOTES.md）。
		//
		// **`'GrLg'` を文字リテラルで書かない**のは、多文字リテラルが処理系定義で警告の
		// 対象になるため。値は 'G'=0x47 / 'r'=0x72 / 'L'=0x4C / 'g'=0x67 を並べたもので、
		// 実機のダンプではデータオブジェクトの中身の +86 にこの並びで入っていた。
		constexpr OSType kFilterContainer = 0x47724C67; // 'GrLg'（Graphic Legend）

		// オブジェクト参照の配列（Kernel/API/MiniCadCallBacks.h の
		// kTaggedDataObjectRefArrayTypeID）と、その中でフィルタが使うタグ。
		constexpr Sint32 kFilterDataType = 15;
		constexpr Sint32 kFilterDataTag = 5;
		constexpr Sint32 kFilterElementCount = 1;

		// 凡例を「そのシートのビューポートに映っているものだけ」に絞る。書けたら true。
		//
		// **UI の「ビューポートでフィルタ…」と同じ状態**を作る。VW はこれを凡例にぶら下がる
		// データオブジェクトのタグ付きデータとして持っており、SDK からは
		// TaggedDataCreate ＋ TaggedDataSet で書ける（読み書きの API はあるが、**凡例の
		// フィルタ専用の呼び出しは SDK にも VectorScript にも無い**——容れ物と型とタグは
		// 実機のバイト列から突き止めた。docs/DEV-NOTES.md「グラフィック凡例」）。
		bool ApplyViewportFilter(MCObjectHandle legend, MCObjectHandle viewport)
		{
			if (legend == nil || viewport == nil)
				return false;

			// フィルタ先は**オブジェクトの内部参照**で持つ。0 は「参照が無い」なので、
			// そのまま書くと「フィルタ無し」と区別が付かない状態を作ってしまう。
			const InternalIndex reference = gSDK->GetObjectInternalIndex(viewport);
			if (reference == 0)
				return false;

			if (!gSDK->TaggedDataCreate(legend, kFilterContainer, kFilterDataType, kFilterDataTag,
										kFilterElementCount))
				return false;
			return gSDK->TaggedDataSet(legend, kFilterContainer, kFilterDataType, kFilterDataTag, 0,
									   &reference) != 0;
		}

		// ソース定義（凡例に何を並べるか）の保存先。フィルタの `'GrLg'` と 1 文字違いの
		// **`'GrLe'`**（'G'=0x47 / 'r'=0x72 / 'L'=0x4C / 'e'=0x65）で、実機のダンプでは
		// **「凡例ソースの定義...」を手で設定した凡例にだけ**ぶら下がっていた。
		constexpr OSType kSourceContainer = 0x47724C65; // 'GrLe'（Graphic Legend の別の容れ物）

		// byte 配列（kTaggedDataByteArrayTypeID = 1）のタグ 0。**16 ビット幅の値も byte 配列に
		// 載る**——タグ付きデータの型は 6 種類しか無く（byte / uint32 / double / matrix /
		// colorref / objectref）、VWFC の `CTaggedDataContainer::CreateTagUint16` も byte 配列を
		// 使う。したがってこの 22 バイトは 11 個の 16 ビット値と読める。
		constexpr Sint32 kSourceDataType = 1;
		constexpr Sint32 kSourceDataTag = 0;

		// **手で設定した凡例からそのまま写した 22 バイト**（実機のダンプ）。ソース＝シンボル・
		// 検索条件 `(INVIEWPORT & (T=SYMBOL))` の 3 枚が**完全に同じ中身**で、集計基準だけ
		// 違う 3 枚だった（集計基準はパラメトリックレコード側なので辻褄が合う）。
		// 16 ビットで読むと 666 / 662 / 798 / 662 / 777 / 15 / 1607 / 1637 / 1615 / 677 / 1680 で、
		// `INVIEWPORT` などの文字列はどこにも出てこない＝**検索条件はトークン列（番号の列）**
		// として保存されている。番号の意味はまだ解けていない。
		//
		// **中身は「シンボルのみ」**（`(INVIEWPORT & (T=SYMBOL))`）。凡例を載せるのは基礎伏図
		// だけで、並べたいのはアンカーボルト＝ハイブリッドシンボルなので用途と合うが、
		// **構造材ツールの部材は並ばない**（draw/Legend.h）。
		//
		// **これは実験である。** 既定のソースが空で、スタイルを当てないと凡例が何も表示しない
		// （実機で確認）以上、ソース定義を per-instance で書き込む以外に道が無い。写した値が
		// 文書をまたいで通用するか・VW の版が変わっても通用するかは**確かめられていない**ので、
		// 実機で「並ぶかどうか」を見て判断する（docs/DEV-NOTES.md「グラフィック凡例」）。
		constexpr std::array<Uint8, 22> kSourceDefinition{
			0x9a, 0x02, 0x96, 0x02, 0x1e, 0x03, 0x96, 0x02, 0x09, 0x03, 0x0f,
			0x00, 0x47, 0x06, 0x65, 0x06, 0x4f, 0x06, 0xa5, 0x02, 0x90, 0x06};

		// ソース定義を書き込む（書けたら true）。フィルタと同じく**`ResetObject` より前**に
		// 済ませる（凡例の作り直しでセルが決まるため）。
		bool ApplySourceDefinition(MCObjectHandle legend)
		{
			if (legend == nil)
				return false;
			if (!gSDK->TaggedDataCreate(legend, kSourceContainer, kSourceDataType, kSourceDataTag,
										static_cast<Sint32>(kSourceDefinition.size())))
				return false;

			for (std::size_t i = 0; i < kSourceDefinition.size(); ++i)
			{
				Uint8 value = kSourceDefinition[i];
				if (gSDK->TaggedDataSet(legend, kSourceContainer, kSourceDataType, kSourceDataTag,
										static_cast<Sint32>(i), &value) == 0)
					return false;
			}
			return true;
		}
	} // namespace

	void prepareGraphicLegendPlugin()
	{
		gSDK->DefineCustomObject(TXString(kGraphicLegendPlugin), kCustomObjectPrefNever);
	}

	bool drawSheetLegend(MCObjectHandle sheetLayer, const core::LegendCommand& command,
						 double viewportScale, MCObjectHandle filterViewport, LegendCounts& counts)
	{
		if (sheetLayer == nil)
		{
			++counts.failed;
			return false;
		}

		// 凡例は**シートレイヤの上**に置く（用紙に載る）。PIO は bInsert=true でカレント
		// レイヤへ入るので、先にそのシートレイヤをアクティブにする。
		gSDK->SetCurrentLayer(sheetLayer);

		const MCObjectHandle object =
			gSDK->CreateCustomObject(TXString(kGraphicLegendPlugin),
									 WorldPt(command.position.x, command.position.y), 0.0, true);
		if (object == nil)
		{
			++counts.failed;
			return false;
		}

		// 箱幅。**スタイルは当てない**（draw/Legend.h の ★）ので、凡例の姿を決めるのは
		// このオブジェクト自身の設定だけになる。
		ApplyBoxWidth(object, counts);

		// ソース定義（何を並べるか）。**既定のソースは空**なので、これを書かないと
		// スタイル無しの凡例は 1 セットも表示しない（実機で確認）。
		if (ApplySourceDefinition(object))
			++counts.sourced;
		else
			++counts.sourceLeft;

		// そのシートのビューポートで絞る（**ResetObject より前**——凡例の作り直しで
		// 並ぶセルが決まるため）。書けなくても凡例自体は残るので、件数だけ持ち帰って続ける
		// （文書中の全シンボルが並ぶ状態になる）。
		if (ApplyViewportFilter(object, filterViewport))
			++counts.filtered;
		else
			++counts.filterLeft;

		// 縮率は写したソース定義に含まれる 1:50 のままになる（書き込む手立てがまだ無い）。
		// 伏図の縮尺がそれと違うなら、凡例のシンボルだけ図と大きさが揃わない。
		if (viewportScale > 0.0 && std::fabs(viewportScale - kSourceImageScale) > kScaleEps)
			++counts.scaleMismatch;

		gSDK->ResetObject(object);

		// 見た目はクラスでは効かないのでオブジェクトの属性として直接与える。**ResetObject
		// の後**に置くと by-instance の属性として保たれる。
		gSDK->SetLineWeight(object, kLineWeightMils);
		gSDK->SetFillPat(object, kFillNone);

		++counts.drawn;
		return true;
	}

	std::string legendDiagnostics(const LegendCounts& counts)
	{
		if (counts.failed == 0 && counts.widthLeft == 0 && counts.paramsFailed == 0 &&
			counts.sourceLeft == 0 && counts.filterLeft == 0 && counts.scaleMismatch == 0)
			return {};

		std::string text = "伏図のグラフィック凡例の診断: ";
		if (counts.failed > 0)
			text += "凡例を置けなかった命令 " + std::to_string(counts.failed) + " 件。";
		if (counts.paramsFailed > 0)
			text += "パラメータを書けなかった凡例 " + std::to_string(counts.paramsFailed) +
					" 件（幅 0 に潰れます）。";
		if (counts.widthLeft > 0)
			text += "箱幅を設定できなかった凡例 " + std::to_string(counts.widthLeft) +
					" 件（幅 0 に潰れます）。";
		if (counts.sourceLeft > 0)
			text += "ソース定義を書けなかった凡例 " + std::to_string(counts.sourceLeft) +
					" 件（何も並びません）。";
		if (counts.filterLeft > 0)
			text += "ビューポートで絞れなかった凡例 " + std::to_string(counts.filterLeft) +
					" 件（その図に無いシンボルも並びます）。";
		if (counts.scaleMismatch > 0)
			text += "イメージの縮率が伏図の縮尺と違う凡例 " + std::to_string(counts.scaleMismatch) +
					" 件（凡例は 1:" + std::to_string(static_cast<int>(kSourceImageScale)) +
					" 固定です）。";
		return text;
	}
} // namespace HomeskzIfcImport::draw
