//
//	parse/ColumnMark.h
//
//	Phase 1（IFC 解析）の断面記号・伏図記号モジュール。Python 版 ifc/column_mark.py に
//	対応する（ROADMAP.md M12）。**IFC のジオメトリは一切参照しない**——記号の位置・
//	大きさ・種別は、すでに組み立て終えた**柱の命令**（core::ColumnCommand）だけで決まる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	作るものは 2 つで、**柱 1 本につき 1 つずつ**（Python 版は span レイヤにつき 1 つの
//	PIO を置き、PIO がリセット時に対象レイヤの構造材を検索して記号群を描いていた）:
//
//	  * **断面記号**（buildColumnSectionMarkCommands）… その柱を切った位置に重ねる
//	    ×（柱）／／（小屋束）。柱と同じ span レイヤ（"1to2-柱"）へ、極細実線クラスで
//	    **素の直線**として描く。両端は柱の実断面（幅×せいの矩形）の角そのもの。
//	  * **伏図記号**（buildColumnPlanMarkCommands）… 専用レイヤ "{to}-柱伏図記号" へ
//	    置く平面記号。**VW 同梱のデータタグ**で描き、その柱へ関連付ける。
//
//	［Python 版との差異・意図的］
//	  * **カスタム PIO（"柱束伏図記号"）を使わない。** 姉妹プロジェクトの PIO は、それを
//	    導入していないマシンで図面の表示が崩れる。本移植は記号を素のジオメトリ（直線）と
//	    VW 本体同梱の PIO（データタグ）だけで描き、**プラグインが無い環境でも図面が
//	    そのまま読める**ことを優先する。
//	  * **記号の絵は柱ごとに解析側で計算する。** PIO のリセットが担っていた「対象レイヤの
//	    構造材を検索して実断面に合わせた記号を描く」計算を、解析側が柱命令から済ませる
//	    （幅・せい・中心はすべて命令にある）。結果として描画側は線を引くだけになり、
//	    無 SDK テストで期待値を確かめられる（CLAUDE.md「テスト方針」）。
//	  * **柱の編集への追随は伏図記号（データタグ）だけが担う。** VW には素の図形を他の
//	    図形へ追随させる仕組みが無い（ISDK の association は読み取りと削除のみ／レコードは
//	    値の入れ物で再計算の引き金にならない）。追随するのは「リセットされるもの」＝ PIO
//	    だけなので、データタグに関連付けを持たせて追随させ、断面記号は静的な線とする。
//	    断面記号は柱と同じレイヤ・同じ位置にあるので、ズレれば図面上ですぐ分かる。
//	  * **記号サイズ（Python 版 DEFAULT_MARK_SIZE=300mm）は持たない。** 断面記号は実断面の
//	    対角線そのもので、伏図記号はタグスタイルが絵を持つため、どちらもサイズ指定を
//	    使わない（Python 版でも断面記号のフォールバック用途だけだった）。
//
//	【span レイヤは単一種別】柱は span（またぐレベル区間）ごとのレイヤに分かれており
//	（parse/Column）、1 つの span レイヤには柱（構造用途 4）だけ、または小屋束（構造用途 5）
//	だけが載る。記号の絵の選択（× か ／ か、柱伏図記号か束伏図記号か）は柱ごとの
//	structuralUse で決めるので、この性質に依存せず正しく描き分けられる。
//

#pragma once

#include "core/Document.h"
#include "parse/Column.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// 伏図記号を置く専用デザインレイヤの接尾辞（Python 版 LAYER_PLAN_MARK_SUFFIX）。
	// レイヤ名は "{to}-柱伏図記号" で、to は span の上側の数値。**同じ to の span
	// （"1to2.5-柱" と "2to2.5-柱"）は同じレイヤに載る**——伏図は「切断位置の直下の
	// レベル」を 1 枚だけ映すので、レベルでまとめておくとその 1 枚で足りる。
	inline constexpr const char* kPlanMarkLayerSuffix = "柱伏図記号";

	// span の to レベルから伏図記号レイヤ名 "{to}-柱伏図記号" を組み立てる（Python 版
	// plan_mark_layer_name）。**レイヤ名の規約はここが唯一**で、記号がレイヤを名乗るとき
	// （buildColumnPlanMarkCommands）と、伏図がそのレイヤを映すとき（parse/Sheet）の
	// 両方がこれを通る。レベルの表記は span 柱レイヤと同じ formatSpanLevel（parse/Story）。
	std::string planMarkLayerName(double toLevel);

	// 断面記号の作図クラス（Python 版 SECTION_MARK_CLASS）。実断面の対角線を極細の実線で
	// 引く。存在しないクラスは VW が自動生成する。
	inline constexpr const char* kSectionMarkClass = "01作図-01線-02実線-01極細線";

	// 伏図記号の作図クラス（Python 版 PLAN_MARK_CLASS）。記号クラス。
	inline constexpr const char* kPlanMarkClass = "01作図-04記号-04構造-一般";

	// 伏図記号のデータタグスタイル名（Python 版 SYMBOL_COLUMN / SYMBOL_KOYAZUKA と同じ
	// 名前）。**スタイルはプラグインが作らない**——テンプレートやリソースライブラリから
	// 供給される前提で、その中に Python 版と同じシンボル（柱伏図記号／束伏図記号）を
	// 焼き込んでおく（draw/ColumnMark.h「スタイルは作らない」）。
	inline constexpr const char* kPlanMarkStyleColumn = "柱伏図記号"; // 柱（管柱・通し柱）
	inline constexpr const char* kPlanMarkStyleKoyazuka = "束伏図記号"; // 小屋束

	// 柱命令から断面記号の命令を組み立てる（柱 1 本につき 1 つ、columns と同じ並び）。
	//
	// 記号は柱の断面矩形（position を中心とする width×depth・軸平行）の対角線で、
	//   * 柱（構造用途 4）  … ×（2 本の対角線）
	//   * 小屋束（構造用途 5）… ／（左下→右上の 1 本）
	// になる。配置先は柱と同じ span レイヤ、クラスは極細実線（kSectionMarkClass）。
	// **断面が退化している柱（幅・せいのどちらかが 0 以下）は記号を作らない**（縮退した
	// 線は図面に何も描かず選択もできないゴミになる。core の isValidSectionMark と同じ考え）。
	std::vector<core::ColumnSectionMarkCommand>
	buildColumnSectionMarkCommands(const std::vector<core::ColumnCommand>& columns);

	// 柱命令から伏図記号（データタグ）の命令を組み立てる（柱 1 本につき 1 つ、columns と
	// 同じ並び）。配置先は "{to}-柱伏図記号"、スタイルは構造用途で選ぶ（柱＝柱伏図記号／
	// 小屋束＝束伏図記号）、挿入点は柱の断面中心、columnIndex は **columns の添字**。
	//
	// **配置先レイヤ名は柱の span レイヤ名から to を読み取って作る**（parse/Story の
	// parseSpanLayer）。span レイヤでない配置先（将来レイヤの分け方を変えたとき）の柱は
	// 記号を作らない——存在しないレイヤ名の命令を出すより、記号が出ないほうが原因を
	// 追いやすい。
	std::vector<core::ColumnPlanMarkCommand>
	buildColumnPlanMarkCommands(const std::vector<core::ColumnCommand>& columns);

	// 実在する伏図記号レイヤを **to の昇順**で（重複を除いて）列挙する。伏図が「切断位置の
	// 直下のレイヤ」を 1 枚選ぶのに使う（parse/Sheet）。
	//
	// **同じ to の span はまとめて 1 枚になる**（"1to2.5-柱" と "2to2.5-柱" → "2.5-柱伏図記号"）。
	// 入力の spans は collectColumnSpans（parse/Column）が (from, to) 昇順で返すものなので、
	// 結果も入力順に依存しない決定的な並びになる（CLAUDE.md「決定性を守る」）。
	struct PlanMarkLayer
	{
		double to = 0.0;
		std::string layer;
	};
	std::vector<PlanMarkLayer> collectPlanMarkLayers(const std::vector<ColumnSpan>& spans);

	// 切断レベル cut の**直下**（to < cut を満たす最大の to）の伏図記号レイヤ名を返す
	// （Python 版 _plan_mark_layer_below_cut）。該当が無ければ空文字。
	//
	// 伏図は「その図が対象とする横架材の**下**にある柱・小屋束」を平面記号で示すので、
	// 切断より下のレベルを 1 枚だけ映す。断面記号（span レイヤ）が [from ≤ cut ≤ to] で
	// 選ばれるのと**排他**になり（to ≥ cut と to < cut で分かれる）、同じ柱が断面記号と
	// 伏図記号の両方で出ることはない（parse/Sheet.h「切断レベルという考え方」）。
	std::string planMarkLayerBelowCut(const std::vector<PlanMarkLayer>& layers, double cut);
} // namespace HomeskzIfcImport::parse
