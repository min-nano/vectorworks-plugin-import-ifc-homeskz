//
//	Extensions/ExtFoundation.h
//
//	基礎（立上り・底盤・地中梁・床付け）を **1 つの立体オブジェクト**として描く PIO
//	（パラメトリックオブジェクト。docs/DEV-NOTES.md M20）。取り込みが 1 つ置き、以後は
//	ユーザーが OIP で寸法を編集できる。
//
//	【何をするか】リセットのたびに、レコードに保存した部品（取り込んだ IFC の実寸。
//	core::decodeFoundation）と OIP の寸法パラメータから基礎の形を決め直し
//	（core::applyFoundationParams）、底盤・その下の砕石・立上り・地中梁・床付けを押し出し
//	ソリッドとして自分の中に描く（core::foundationSolids）。2D/平面向けには底盤の外形・
//	立上りの矩形・地中梁の天端幅の矩形を描く（core::foundationPlanShapes）。幾何の計算は
//	すべて core/Foundation（無 SDK・テスト済み）にあり、ここは SDK のオブジェクトを作るだけ。
//
//	【なぜ PIO か】M9〜M17 は立上り＝壁・底盤＝スラブ・地中梁＝モディファイア＋可視ソリッドと
//	別々の VW オブジェクトで描いていた。噛み合わせは SDK から作れず、接ぎ目を隠す細工
//	（壁結合・キャップ・呑み込み）が要素の数だけ要った。1 つの PIO の中に同素材のソリッドを
//	重ねれば接ぎ目は同じオブジェクトの中に閉じ、断面では構造用図形（ovIsStructural）として
//	一体に表示される。さらに**寸法を後から変えられる**——スラブを厚くする・立上りを高くする
//	といった設計変更を取り込み直さずに OIP で済ませられる。
//
//	【編集の規則】OIP の値は**代表値**（取り込み時に最も多かった実寸）で、変えた差を部品へ
//	配る（core::applyFoundationParams の doc コメント）。実データは立上り幅が 120 / 150 /
//	300 と混在するので、値そのものを一律に置き換えると細部が失われる。
//
//	【登録名はこのプラグイン固有にする】ユニバーサル名は "HomeskzFoundation"。
//
//	【レコードの文字列欄に部品を保存する】部品（座標・寸法）は kParamData の文字列
//	パラメータに直列化して持つ（core::encodeFoundation）。PIO はパラメータが変わるたびに
//	部品から描き直すので、部品自体を自分の中に持っていなければならない。OIP には
//	「取り込みデータ（編集不可）」として出る——欄を隠す口（IProviderShapePane）は
//	SDK リファレンスの `Info/` に作法があるが、実機で確かめてから採る。
//

#pragma once

#include "PluginPrefix.h"

#include "VWFC/PluginSupport/VWExtensionParametric.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// PIO のユニバーサル名。**解析側が命令に載せる名前ではなく、描画側が CreateCustomObject
	// へ渡す名前**なので、draw/Footing と共有する。
	constexpr const char* kFoundationUniversalName = "HomeskzFoundation";

	// パラメータのユニバーサル名。**draw/Footing が書く名前とここが食い違うと setter は
	// 黙って無視される**ので、定義はここ 1 か所。並びは OIP に出る順（core::FoundationParams
	// と 1 対 1）。単位はすべて mm（図面の単位で表示される）。
	constexpr const char* kParamSlabThickness = "SlabThickness"; // 底盤のコンクリート厚
	constexpr const char* kParamSlabTop = "SlabTop";	   // 底盤天端の高さ（GL から）
	constexpr const char* kParamRiserWidth = "RiserWidth"; // 立上りの幅
	constexpr const char* kParamRiserTop = "RiserTop";	 // 立上り天端の高さ（GL から）
	constexpr const char* kParamBeamDepth = "BeamDepth"; // 地中梁のせい（底盤底面から）
	constexpr const char* kParamHaunchWidth = "HaunchWidth"; // 地中梁の斜め部の片側の幅
	constexpr const char* kParamHaunchHeight = "HaunchHeight"; // 地中梁の斜め部の高さ
	constexpr const char* kParamData = "FoundationData"; // 部品の直列化（core::encodeFoundation）

	// ------------------------------------------------------------------------
	// リセット時に基礎を描く本体。
	class CFoundation_EventSink : public VWParametric_EventSink
	{
	public:
		CFoundation_EventSink(IVWUnknown* parent);
		~CFoundation_EventSink() override;

		// レコードの部品と OIP の寸法から基礎のソリッドを描き直す（PIO のリセット）。
		EObjectEvent Recalculate() override;

		// 「生成時に設定ダイアログを出さない」を宣言する（Extensions/ExtColumnMark と同じ）。
		EObjectEvent OnInitXProperties(CodeRefID objectID) override;
	};

	// ------------------------------------------------------------------------
	// 拡張そのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtFoundation : public VWExtensionParametric
	{
		DEFINE_VWParametricExtension;

	public:
		CExtFoundation(CallBackPtr cbp);
		~CExtFoundation() override;
	};
} // namespace HomeskzIfcImport
