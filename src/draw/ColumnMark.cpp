//
//	draw/ColumnMark.cpp
//
//	記号（断面記号・伏図記号）の設置の実装。意図は draw/ColumnMark.h と
//	parse/ColumnMark.h を参照。
//	【SDK 依存】PluginPrefix.h を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされる（CLAUDE.md「依存の向きは厳守する」）。
//
//	手順:配置先レイヤを用意 → CreateCustomObject で PIO を作る → 本体のクラスを設定
//	→パラメータ（検索対象レイヤ・クラス・記号スタイル・シンボル）を書く → ResetObject
//	リセットで PIO 本体（Extensions/ExtColumnMark）が対象レイヤを検索して記号を描く。
//
//	**パラメータは PIO 本体と同じ名前**でなければ黙って無視される（M6 の垂木で実証済み。
//	draw/DrawUtil の ResolveParamName の doc コメント）。名前の定義は 1 か所に集めたい
//	ので、Extensions/ExtColumnMark.h の kParam* を include して共有する。
//

#include "PluginPrefix.h"
#include "draw/ColumnMark.h"
#include "draw/DrawUtil.h"
#include "Extensions/ExtColumnMark.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 記号 1 つを置く。PIO を作ってパラメータを書き、リセットまでできたら true。
		bool PlaceOne(const core::ColumnMarkCommand& mark)
		{
			// 挿入点は原点でよい（記号は検索した柱の位置に描かれる）。第 4 引数
			// bInsert=true でアクティブレイヤへ入れる。
			const MCObjectHandle object =
				gSDK->CreateCustomObject(TXString(kColumnMarkUniversalName),
										 WorldPt(mark.position.x, mark.position.y), 0.0, true);
			if (object == nil)
				return false;

			// PIO 本体のクラス（記号の線・シンボルはこのクラスの属性で描かれる）。
			SetClassByName(object, mark.drawClass);

			try
			{
				VWParametricObj pio(object);
				pio.SetParamString(kParamTargetLayer, TXString(mark.targetLayer.c_str()));
				pio.SetParamString(kParamTargetClass, TXString(mark.targetClass.c_str()));
				pio.SetParamString(kParamMarkStyle,
								   TXString(mark.style == core::ColumnMarkStyle::Plan
												? kMarkStylePlan
												: kMarkStyleSection));
				pio.SetParamString(kParamMarkSymbol, TXString(mark.symbol.c_str()));
			}
			catch (...)
			{
				// パラメータを書けなくても PIO は図面に残る（記号 0 個になるだけ）。
				return false;
			}

			// リセットでここが本体の Recalculate を呼び、記号が描かれる。
			gSDK->ResetObject(object);
			return true;
		}
	} // namespace

	std::size_t drawColumnMarks(const core::Document& document, core::ProgressReporter& progress,
								std::string* outNote)
	{
		std::size_t drawn = 0;
		std::size_t missingLayers = 0;
		std::size_t failed = 0;

		// **記号を 1 つも作る前に、PIO の定義を「設定ダイアログを出さない」で作っておく。**
		//
		// CreateCustomObject は、その名前の PIO が文書にまだ定義されていなければ
		// DefineCustomObject で定義を作る。その既定が `kCustomObjectPrefAlways` なので、
		// **最初の 1 個を作るときだけ「オブジェクトの設定」ダイアログが出て、インポートが
		// そこで止まる**（実機で確認。2 個目以降は定義済みなので出ない）。PIO 側の
		// OnInitXProperties は定義が作られる過程で走るため、この 1 回目には間に合わない。
		// ここで先に定義してしまえば、CreateCustomObject は既存の定義を使うので出ない。
		if (!document.columnMarks.empty())
			gSDK->DefineCustomObject(TXString(kColumnMarkUniversalName), kCustomObjectPrefNever);

		for (const core::ColumnMarkCommand& mark : document.columnMarks)
		{
			if (progress.cancelled())
				break;
			progress.step();

			// 伏図記号のレイヤ（"{to}-柱伏図記号"）はストーリが作らない独立レイヤなので
			// 無ければ作る。断面記号の配置先（span レイヤ）はストーリが作るので、無ければ
			// その階の生成がスキップされたということ＝記号も置かない（ヘッダ参照）。
			const bool ownLayer = mark.style == core::ColumnMarkStyle::Plan;
			if ((ownLayer ? PrepareLayer(mark.layer) : ActivateExistingLayer(mark.layer)) == nil)
			{
				++missingLayers;
				continue;
			}

			if (PlaceOne(mark))
				++drawn;
			else
				++failed;
		}

		if (outNote != nullptr && (missingLayers > 0 || failed > 0))
		{
			std::string text = "柱記号の診断: ";
			if (missingLayers > 0)
				text += "配置先レイヤを用意できない命令 " + std::to_string(missingLayers) + " 件。";
			if (failed > 0)
				text += "記号オブジェクトを作れなかった命令 " + std::to_string(failed) + " 件。";
			*outNote = std::move(text);
		}

		return drawn;
	}

	std::vector<std::string> planMarkLayerNames(const core::Document& document)
	{
		std::vector<std::string> names;
		for (const core::ColumnMarkCommand& mark : document.columnMarks)
		{
			if (mark.style != core::ColumnMarkStyle::Plan)
				continue;
			PushUnique(names, mark.layer);
		}
		return names;
	}
} // namespace HomeskzIfcImport::draw
