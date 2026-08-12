//
//	Extensions/ExtColumnMark.h
//
//	柱・小屋束の記号を描く PIO（パラメトリックオブジェクト）。ROADMAP.md M12 の
//	【決定】でこのプラグインに同梱すると決めたもので、Python 版が使う姉妹プロジェクトの
//	カスタム PIO「柱束伏図記号」に相当する。
//
//	【何をするか】リセットのたびに、パラメータで指定された**対象レイヤ**の構造材
//	（構造用途 4＝柱 / 5＝小屋束）を検索し、見つけた 1 本ごとに記号を描く。
//	  * 記号スタイル = Section … その柱の**実断面**（幅×せい）の対角線。柱は ×、
//	    小屋束は ／（1 本）。
//	  * 記号スタイル = Plan    … その柱の位置にシンボルを 1 つ（MarkSymbol）。
//
//	【なぜ PIO か】記号を静的なジオメトリで持つと、柱の断面が変わったときに
//	**間違った断面の記号が残る**——記号が無いより悪い。PIO はリセットのたびに実物から
//	位置・大きさ・本数を導き直すので、嘘をつき続けることがない（core/Document.h の
//	ColumnMarkCommand 参照）。VW は PIO が描いたジオメトリを図面に保存するので、
//	プラグインを入れていない環境でも図面はそのまま表示できる（更新だけができない）。
//
//	【登録名は Python 版と分ける】姉妹プロジェクトの "柱束伏図記号" と同じ名前で登録すると、
//	両方を入れた環境で衝突する。ユニバーサル名は "HomeskzColumnMark" にしてある。
//

#pragma once

#include "PluginPrefix.h"

#include "VWFC/PluginSupport/VWExtensionParametric.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// PIO のユニバーサル名。**解析側が命令に載せる名前ではなく、描画側が
	// CreateCustomObject へ渡す名前**なので、draw/ColumnMark と共有する。
	constexpr const char* kColumnMarkUniversalName = "HomeskzColumnMark";

	// パラメータのユニバーサル名（Python 版 PARAM_* に対応）。**draw/ColumnMark が
	// 書く名前とここが食い違うと setter は黙って無視される**ので、定義はここ 1 か所。
	constexpr const char* kParamTargetLayer = "TargetLayer"; // 検索対象のデザインレイヤ名
	constexpr const char* kParamTargetClass = "TargetClass"; // 検索対象クラス（空＝全クラス）
	constexpr const char* kParamMarkStyle = "MarkStyle";   // 記号スタイル（下記）
	constexpr const char* kParamMarkSymbol = "MarkSymbol"; // 平面記号のシンボル名

	// MarkStyle の値。ユニバーサル名なので言語に依存しない綴りにする（Python 版は
	// '断面' / '平面' だったが、自前の PIO なので英語の universal 名で持つ）。
	constexpr const char* kMarkStyleSection = "Section"; // 実断面の対角線（柱×・小屋束／）
	constexpr const char* kMarkStylePlan = "Plan"; // 各柱位置にシンボル

	// ------------------------------------------------------------------------
	// リセット時に記号を描く本体。
	class CColumnMark_EventSink : public VWParametric_EventSink
	{
	public:
		CColumnMark_EventSink(IVWUnknown* parent);
		~CColumnMark_EventSink() override;

		// 対象レイヤを検索して記号を描き直す（PIO のリセット）。
		EObjectEvent Recalculate() override;

		// オブジェクトプロパティの初期化。**「生成時に設定ダイアログを出さない」を
		// ここで宣言する。**
		//
		// PIO の既定は `DefineCustomObject(name, prefWhen = kCustomObjectPrefAlways)`
		// ——つまり**オブジェクトを作るたびに「オブジェクトの設定」ダイアログが出る**。
		// 手で 1 つ置くツールならそれでよいが、記号はインポートが span レイヤの数だけ
		// 自動生成するので、そのたびに応答を求められてインポートが止まる（実機で確認）。
		// パラメータは描画側（draw/ColumnMark）が命令から書くため、ユーザーに尋ねる
		// ことは何も無い。
		EObjectEvent OnInitXProperties(CodeRefID objectID) override;
	};

	// ------------------------------------------------------------------------
	// 拡張そのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtColumnMark : public VWExtensionParametric
	{
		DEFINE_VWParametricExtension;

	public:
		CExtColumnMark(CallBackPtr cbp);
		~CExtColumnMark() override;
	};
} // namespace HomeskzIfcImport
