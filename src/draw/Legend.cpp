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
//	  * gSDK->ResetObject                                                … 反映（中身の計算）
//	  * VWParametricObj の GetParamsCount / GetParamName / GetParamLocalizedName
//	                                                        … パラメータ名の解決（下記）
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
#include <cstddef>
#include <cstdio>
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

		// イメージの縮率（＝凡例に並ぶセルの中で部材を描く縮尺）のパラメータ名の候補。
		// **正しい名前は SDK ヘッダにもマニュアルにも無い**（凡例は VW 同梱の PIO で、
		// パラメータ名はその PIO のパラメトリックレコードにしか無い）。そこで
		// universal 名の候補と**OIP の表示名**（日本語 UI での「イメージの縮率」）を順に
		// 引き、最初に実在したものを使う（draw/DrawUtil の ResolveParamName と同じ考え方を、
		// 候補が複数あるこちら用に広げたもの）。
		constexpr std::array<const char*, 4> kImageScaleNames{"ImageScale", "Image Scale",
															  "ImageScaleFactor", "イメージの縮率"};

		// 縮率が「カスタム」のときに読まれる分母（OIP の「カスタム 1:」）。縮率のポップアップ
		// が既定値の一覧（1:1 … 1:100）に無い縮尺を指せるように、**ポップアップより先に**
		// 入れておく。無い環境（名前が違う）でも縮率そのものが入れば実害は無いので、
		// 見つからなければ黙って飛ばす。
		constexpr std::array<const char*, 4> kCustomScaleNames{"CustomScale", "ImageScaleCustom",
															   "カスタム 1:", "カスタム 1"};

		// 見た目。凡例 PIO が内部で描く枠線・セルは**クラスでは制御できない**ので、
		// オブジェクトの属性として直接与える（draw/Legend.h）。線の太さの単位はミル（1/1000
		// インチ）で、5 ミル = 0.127mm を VW は 0.13mm と表示する。塗りパターン 0 = なし。
		constexpr short kLineWeightMils = 5;
		constexpr InternalIndex kFillNone = 0;

		// 診断へ出すパラメータ名の上限（凡例の PIO はパラメータが多いので頭から数個で切る）。
		constexpr std::size_t kMaxParamNames = 24;

		// 候補名（universal 名でも OIP の表示名でもよい）で実在するパラメータを探し、その
		// universal 名を返す。**どれも無ければ空**——呼び出し側はそれを「このパラメータは
		// この環境の PIO に無い」として扱う（存在しない名前へ書いても setter は黙って何も
		// しないので、書く前に必ず引く）。
		TXString FindParam(const VWParametricObj& pio, const std::array<const char*, 4>& names)
		{
			for (const char* candidate : names)
			{
				const TXString name(candidate);
				if (pio.GetParamIndex(name) != static_cast<size_t>(-1))
					return name;
			}

			const size_t count = pio.GetParamsCount();
			for (const char* candidate : names)
			{
				const TXString localized(candidate);
				for (size_t i = 0; i < count; ++i)
				{
					if (pio.GetParamLocalizedName(i) == localized)
						return pio.GetParamName(i);
				}
			}
			return {};
		}

		// 縮尺の表記（分母 50 → "1:50"）。ポップアップ・文字のフィールドはこの形の文字列で
		// 縮尺を持つ（OIP の「イメージの縮率」に出ているのと同じ表記）。
		TXString ScaleText(double viewportScale)
		{
			std::array<char, 32> buffer{};
			std::snprintf(buffer.data(), buffer.size(), "1:%g", viewportScale);
			return {buffer.data()};
		}

		// 縮率のパラメータ 1 つへ書く（書けたら true）。**フィールドの種別で入れ方を変える**
		// ——ポップアップ（縮尺の一覧）と文字のフィールドは "1:50" という表記を持つので数値を
		// 入れても効かず、逆に実数のフィールドへ "1:50" を入れても効かない。どちらの登録でも
		// 入るように、種別で選んでから**読み戻して確かめる**（draw/DrawUtil の
		// SetParamRealChecked と同じ用心。名前も型も 1 つ違えば setter は黙って無視される）。
		bool WriteScale(VWParametricObj& pio, const TXString& param, double viewportScale)
		{
			const EFieldStyle style = pio.GetParamStyle(param);
			if (style == kFieldPopUp || style == kFieldText)
			{
				const TXString text = ScaleText(viewportScale);
				pio.SetParamString(param, text);
				if (pio.GetParamString(param) == text)
					return true;
			}
			return SetParamRealChecked(pio, param, viewportScale);
		}

		// イメージの縮率を viewportScale（1:50 なら 50）へ合わせる。書けたら true。
		bool ApplyImageScale(VWParametricObj& pio, double viewportScale)
		{
			// カスタム欄が先（上記 kCustomScaleNames）。こちらは分母そのもの（50）を持つ
			// 数値のフィールドなので、表記ではなく値を入れる。
			if (const TXString custom = FindParam(pio, kCustomScaleNames); !custom.IsEmpty())
				SetParamRealChecked(pio, custom, viewportScale);

			const TXString scale = FindParam(pio, kImageScaleNames);
			if (scale.IsEmpty())
				return false;
			return WriteScale(pio, scale, viewportScale);
		}

		// PIO のパラメータ名の一覧（"universal 名(表示名)" を空白区切り）。**縮率を書けな
		// かったときだけ**診断へ出し、正しい名前を実機から持ち帰るための目にする
		// （draw/Legend.h の scaleParams）。
		std::string ParamNames(const VWParametricObj& pio)
		{
			const size_t count = pio.GetParamsCount();
			std::string text;
			for (size_t i = 0; i < count && i < kMaxParamNames; ++i)
			{
				if (!text.empty())
					text += " ";
				text += pio.GetParamName(i).GetStdString();
				text += "(";
				text += pio.GetParamLocalizedName(i).GetStdString();
				text += ")";
			}
			if (count > kMaxParamNames)
				text += " …";
			return text;
		}

		// 生成した凡例のパラメータ（箱幅・イメージの縮率）を与える。**例外を外へ出さない**
		// ——どちらも書けなくても凡例そのものは図面に残るので、件数だけ counts へ積む。
		void ApplyParams(MCObjectHandle object, double viewportScale, LegendCounts& counts)
		{
			try
			{
				VWParametricObj pio(object);
				if (!SetParamRealChecked(pio, TXString(kFieldBoxWidth), kBoxWidth))
					++counts.widthLeft;

				// 縮尺が分からなかった伏図（表示レイヤから読めなかった）では触らない
				// ——既定の縮率のまま置く方が、当てずっぽうの値を書くよりましである。
				if (viewportScale <= 0.0)
				{
					++counts.scaleUnknown;
					return;
				}
				if (!ApplyImageScale(pio, viewportScale))
				{
					++counts.scaleLeft;
					if (counts.scaleParams.empty())
						counts.scaleParams = ParamNames(pio);
				}
			}
			catch (...)
			{
				// PIO として開けなかった（＝パラメータを 1 つも書けていない）。幅 0 のまま
				// 潰れる・縮率が既定のままになるだけで凡例自体は図面に残るので、続ける。
				++counts.paramsFailed;
			}
		}
	} // namespace

	void prepareGraphicLegendPlugin()
	{
		gSDK->DefineCustomObject(TXString(kGraphicLegendPlugin), kCustomObjectPrefNever);
	}

	bool drawSheetLegend(MCObjectHandle sheetLayer, const core::LegendCommand& command,
						 double viewportScale, LegendCounts& counts)
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

		// 箱幅とイメージの縮率。**スタイルは当てない**（draw/Legend.h の ★）ので、
		// 凡例の姿を決めるのはこのオブジェクトのパラメータだけになる。
		ApplyParams(object, viewportScale, counts);

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
		if (counts.failed == 0 && counts.widthLeft == 0 && counts.scaleLeft == 0 &&
			counts.scaleUnknown == 0 && counts.paramsFailed == 0)
			return {};

		std::string text = "伏図のグラフィック凡例の診断: ";
		if (counts.failed > 0)
			text += "凡例を置けなかった命令 " + std::to_string(counts.failed) + " 件。";
		if (counts.paramsFailed > 0)
			text += "パラメータを書けなかった凡例 " + std::to_string(counts.paramsFailed) +
					" 件（幅 0 に潰れ、縮率も既定のままになります）。";
		if (counts.widthLeft > 0)
			text += "箱幅を設定できなかった凡例 " + std::to_string(counts.widthLeft) +
					" 件（幅 0 に潰れます）。";
		if (counts.scaleUnknown > 0)
			text += "伏図の縮尺が分からず既定の縮率で置いた凡例 " +
					std::to_string(counts.scaleUnknown) + " 件。";
		if (counts.scaleLeft > 0)
		{
			text += "イメージの縮率を設定できなかった凡例 " + std::to_string(counts.scaleLeft) +
					" 件（凡例だけ図と縮尺が揃いません）。";
			if (!counts.scaleParams.empty())
				text += "凡例のパラメータ: " + counts.scaleParams + "。";
		}
		return text;
	}
} // namespace HomeskzIfcImport::draw
