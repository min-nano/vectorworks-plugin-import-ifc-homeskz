//
//	draw/Footing.cpp
//
//	基礎の配置の実装。意図は draw/Footing.h と Extensions/ExtFoundation.h を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	手順（draw/ColumnMark と同じ作法）:
//	  1. DefineCustomObject で PIO の定義を「設定ダイアログを出さない」で先に作る
//	     （CreateCustomObject の 1 個目でダイアログが出て止まるのを防ぐ。SDK リファレンス
//	     Findings「Parametric Objects」）。
//	  2. 配置先レイヤ（"F-基礎"。基礎ストーリの GL レベル＝高さ 0）をアクティブにする。
//	  3. CreateCustomObject で原点に PIO を作り、本体のクラスを設定する。**挿入点は原点**——
//	     部品の座標はセンタリング済みのワールド座標で、PIO のローカル座標＝ワールド座標に
//	     なるよう原点に置く（PIO を動かせば基礎全体が動く。それが 1 つのオブジェクトにした
//	     意味）。
//	  4. 寸法パラメータ（代表値）と、部品を直列化した文字列（core::encodeFoundation）を
//	     書く。**書いたら読み戻して確かめる**（setter は黙って無視することがある。draw/DrawUtil
//	     の SetParamRealChecked）。
//	  5. ResetObject で PIO の Recalculate が走り、ソリッドが描かれる。描かれたか（PIO の
//	     中に子オブジェクトがあるか）を見て診断に出す。
//

#include "PluginPrefix.h"
#include "draw/Footing.h"
#include "draw/DrawUtil.h"
#include "Extensions/ExtFoundation.h"
#include "core/Document.h"
#include "core/Foundation.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 代表値の 7 つを PIO へ書く。書けなかったパラメータ名を改行区切りで返す（空＝全部書けた）。
		std::string WriteParams(VWParametricObj& pio, const core::FoundationParams& params)
		{
			std::string failed;
			const auto write = [&](const char* name, double value)
			{
				if (!SetParamRealChecked(pio, TXString(name), value))
				{
					if (!failed.empty())
						failed += " ";
					failed += name;
				}
			};
			write(kParamSlabThickness, params.slabThickness);
			write(kParamSlabTop, params.slabTop);
			write(kParamRiserWidth, params.riserWidth);
			write(kParamRiserTop, params.riserTop);
			write(kParamBeamDepth, params.beamDepth);
			write(kParamHaunchWidth, params.haunchWidth);
			write(kParamHaunchHeight, params.haunchHeight);
			return failed;
		}

		// 部品の直列化を PIO の文字列パラメータへ書き、読み戻して一致するか確かめる。
		// **長さ**が問題になりうる（レコードの文字列欄の上限は SDK ヘッダに書かれていない）ので、
		// 一致しなければ書けた長さと要る長さを診断に残す。
		bool WriteData(VWParametricObj& pio, const std::string& encoded, std::string& outNote)
		{
			pio.SetParamString(TXString(kParamData), TXString(encoded.c_str()));
			const std::string back = pio.GetParamString(TXString(kParamData)).GetStdString();
			if (back == encoded)
				return true;
			outNote = "基礎: 部品を PIO のレコードへ保存できませんでした（要 " +
					  std::to_string(encoded.size()) + " バイト・保存できたのは " +
					  std::to_string(back.size()) + " バイト）。";
			return false;
		}
	} // namespace

	std::size_t drawFoundation(const core::Document& document, core::ProgressReporter& progress,
							   std::string* outNote)
	{
		if (!document.foundation.has_value())
			return 0;
		const core::FoundationCommand& command = *document.foundation;

		std::string note;
		const auto fail = [&](const std::string& reason)
		{
			note = "基礎: " + reason;
			if (outNote != nullptr)
				*outNote = note;
			return std::size_t{0};
		};

		if (progress.cancelled())
			return 0;
		progress.step();

		// PIO の定義を「設定ダイアログを出さない」で先に作る（ヘッダ冒頭の手順 1）。
		gSDK->DefineCustomObject(TXString(kFoundationUniversalName), kCustomObjectPrefNever);

		// 配置先レイヤ（"F-基礎"）が無ければ置かない（基礎ストーリの生成がスキップされた）。
		if (ActivateExistingLayer(command.layer) == nil)
			return fail("配置先レイヤ「" + command.layer + "」がありません。");

		const MCObjectHandle object = gSDK->CreateCustomObject(TXString(kFoundationUniversalName),
															   WorldPt(0.0, 0.0), 0.0, true);
		if (object == nil)
			return fail("PIO「" + std::string(kFoundationUniversalName) +
						"」を作れませんでした（プラグインの登録を確かめてください）。");
		SetClassByName(object, command.drawClass);

		try
		{
			VWParametricObj pio(object);
			const std::string failedParams = WriteParams(pio, command.params);
			if (!failedParams.empty())
				return fail("寸法パラメータを書けませんでした（" + failedParams + "）。");
			std::string dataNote;
			if (!WriteData(pio, core::encodeFoundation(command), dataNote))
				return fail(dataNote.substr(std::string("基礎: ").size()));
		}
		catch (...)
		{
			return fail("PIO のパラメータを書けませんでした。");
		}

		// リセットで PIO 本体（Extensions/ExtFoundation）が部品からソリッドを描く。描けたかは
		// PIO の中に子オブジェクトがあるかで見る（部品はあるのに 0 個なら描画側の問題）。
		gSDK->ResetObject(object);
		if (gSDK->FirstMemberObj(object) == nil)
			return fail("PIO をリセットしてもソリッドが 1 つも描かれませんでした。");
		return 1;
	}
} // namespace HomeskzIfcImport::draw
