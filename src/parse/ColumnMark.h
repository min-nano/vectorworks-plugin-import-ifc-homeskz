//
//	parse/ColumnMark.h
//
//	Phase 1（IFC 解析）の断面記号・伏図記号モジュール（docs/DEV-NOTES.md M12）。**IFC
//	のジオメトリは一切参照しない**——記号の位置・大きさ・種別は、すでに組み立て終えた**柱の命令
//	**（core::ColumnCommand）だけで決まる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	作るものは 2 つで、**実在する span 柱レイヤにつき 1 つずつ**:
//
//	  * **断面記号**（style=Section）… その span レイヤ自身を検索対象にし、レイヤ自身へ
//	    重ねて置く。PIO が見つけた柱・小屋束の**実断面**に合わせて ×（柱）／／（小屋束）を
//	    極細実線で描く。
//	  * **伏図記号**（style=Plan）… 検索対象は同じ span レイヤ、配置先は専用レイヤ
//	    "{to}-柱伏図記号"。各柱の位置にシンボル（柱＝"柱伏図記号" / 小屋束＝"束伏図記号"）を
//	    置く。
//
//	【なぜ柱 1 本ごとではないか】記号を柱ごとの静的ジオメトリで持つと、**柱の断面が
//	変わったときに古い記号が残り、しかもそれが「間違った断面の記号」になる**——記号が
//	無いより悪い。PIO なら、リセットのたびに対象レイヤの実物から位置・大きさ・本数を
//	導き直すので、嘘をつき続けることがない。したがって命令は「どのレイヤの何を、どこへ、
//	どう描くか」だけを持ち、記号の絵は PIO（Extensions/ExtColumnMark）が描く。
//
//	【設計上の要点】
//	  * **記号を描く PIO は本プラグインが提供する**（Extensions/ExtColumnMark）。
//	    1 つのモジュールにメニューコマンドと一緒に登録するので、インストールも自動更新も
//	    1 つで済み、記号の規約（構造用途・レイヤ名・クラス名）を解析側と共有できる。
//	    登録名はこのプラグイン固有にしてある（同種の記号 PIO を持つ環境で衝突させないため）。
//	  * **記号サイズは持たない。** 断面記号は実断面の対角線そのもので、伏図記号はシンボルをそ
//	    のまま置くので、サイズ指定を使う経路が無い。
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
	// 伏図記号を置く専用デザインレイヤの接尾辞。レイヤ名は "{to}-柱伏図記号" で、to は span
	// の上側の数値。**同じ to の span（"1to2.5-柱" と "2to2.5-柱"）は同じレイヤに載る**——伏図
	// は「切断位置の直下のレベル」を 1 枚だけ映すので、レベルでまとめておくとその
	// 1 枚で足りる。
	inline constexpr const char* kPlanMarkLayerSuffix = "柱伏図記号";

	// span の to レベルから伏図記号レイヤ名 "{to}-柱伏図記号" を組み立てる。**レイヤ名の規約
	// はここが唯一**で、記号がレイヤを名乗るとき（buildColumnPlanMarkCommands）と、
	// 伏図がそのレイヤを映すとき（parse/Sheet）の両方がこれを通る。レベルの表記は span
	// 柱レイヤと同じ formatSpanLevel（parse/Story）。
	std::string planMarkLayerName(double toLevel);

	// 断面記号の作図クラス。実断面の対角線を極細の実線で引く。存在しないクラスは VW
	// が自動生成する。
	inline constexpr const char* kSectionMarkClass = "01作図-01線-02実線-01極細線";

	// 伏図記号の作図クラス。記号クラス。
	inline constexpr const char* kPlanMarkClass = "01作図-04記号-04構造-一般";

	// 伏図記号で使うシンボル名。span レイヤは単一種別なので、そのレイヤの柱の構造用途（柱=4
	// / 小屋束=5）でどちらかに決まる。
	// **シンボル定義はプラグインが作らない**——テンプレートやリソースライブラリから
	// 供給される前提（シンボル置換系と同じ作法。draw/Symbol.cpp）。
	inline constexpr const char* kPlanMarkSymbolColumn = "柱伏図記号"; // 柱（管柱・通し柱）
	inline constexpr const char* kPlanMarkSymbolKoyazuka = "束伏図記号"; // 小屋束

	// 柱命令から記号の命令を組み立てる。
	//
	// 実在する span 柱レイヤ（collectColumnSpans。(from, to) 昇順）ごとに、断面記号
	// 1 つと伏図記号 1 つを作る。**断面記号をすべて先に、続けて伏図記号をすべて**並べる（順序
	// は描画に影響しないが、決定的にするため固定する）。柱が無ければ空。
	//
	// 伏図記号のシンボルはその span レイヤの柱の構造用途で決める（span レイヤは単一種別＝
	// 柱のみ、または小屋束のみ。parse/Column.h）。
	std::vector<core::ColumnMarkCommand>
	buildColumnMarkCommands(const std::vector<core::ColumnCommand>& columns);

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

	// 切断レベル cut の**直下**（to < cut を満たす最大の to）の伏図記号レイヤ名を返す。
	// 該当が無ければ空文字。
	//
	// 伏図は「その図が対象とする横架材の**下**にある柱・小屋束」を平面記号で示すので、
	// 切断より下のレベルを 1 枚だけ映す。断面記号（span レイヤ）が [from ≤ cut ≤ to] で
	// 選ばれるのと**排他**になり（to ≥ cut と to < cut で分かれる）、同じ柱が断面記号と
	// 伏図記号の両方で出ることはない（parse/Sheet.h「切断レベルという考え方」）。
	std::string planMarkLayerBelowCut(const std::vector<PlanMarkLayer>& layers, double cut);
} // namespace HomeskzIfcImport::parse
