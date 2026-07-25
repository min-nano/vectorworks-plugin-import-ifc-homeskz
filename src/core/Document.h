//
//	core/Document.h
//
//	命令セット（Document）の構造体定義。IFC 解析フェーズ（parse/）と VW 描画
//	フェーズ（draw/）を結ぶ唯一の境界であり、両フェーズはこの構造体だけで接続する。
//	SDK ハンドルや STEP エンティティポインタ等「フェーズ間で運べないもの」は
//	絶対にここへ載せない（CLAUDE.md「アーキテクチャ: 2 フェーズ分離」）。
//
//	Python 版 document.py の TypedDict 群に対応させる。フィールド名・意味は
//	Python 版に合わせ、追跡しやすさと仕様のブレ防止を優先する。予約語（class 等）は
//	drawClass / className のように機械的に置換する。
//
//	いまはフォルダ骨組みとして「バージョン＋空の器」だけを持つ。各命令リスト
//	（stories / grids / members / columns / walls / slabs …）は、対応する
//	マイルストーンで要素を移植するたびに 1 つずつ追加する（ROADMAP.md）。
//

#pragma once

#include "core/Geometry.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 命令セットのスキーマバージョン。JSON ダンプ（DocumentJson・任意）や Python 版
	// 期待値とのゴールデン比較で、スキーマ変更を検出できるように持たせておく。
	inline constexpr int kDocumentVersion = 1;

	// 通り芯（グリッド）1 本の描画命令。Python 版 document.py の GridCommand（dict）に
	// 対応する。draw/Grid がこれを GridAxis オブジェクトへ変換する（ROADMAP.md M1）。
	//
	// Python 版キーとの対応:
	//   label     ← 'label'  … 軸名（IfcGridAxis の AxisTag。例 "X1" / "Y1"）
	//   layer     ← 'layer'  … 配置デザインレイヤ名（通り芯は常に "共通"）
	//   drawClass ← 'class'  … クラス名（X 通り / Y 通り。予約語 class を機械置換）
	//   start     ← 'start'  … 始点（bbox 中心でセンタリング済みの平面座標）
	//   end       ← 'end'    … 終点（同上）
	// start/end は 3D ではなく平面（Vec2）: 通り芯は配置行列・断面・ストーリを要さず、
	// ポリラインの端点だけで決まる（M1 が最初の縦切りである理由。ROADMAP.md M1）。
	struct GridCommand
	{
		std::string label;
		std::string layer = "共通";
		std::string drawClass;
		Vec2 start;
		Vec2 end;
	};

	// 命令セット本体。プレーンな構造体の集約（std::vector / std::string / double /
	// enum 等）で表す。
	//
	// TODO: 要素ごとに命令リストを追加していく（フィールド名は Python 版のキーに対応）。
	//   * M3 stories   : std::vector<StoryCommand>
	//   * M5 members   : std::vector<MemberCommand>
	//   * M6 columns   : std::vector<ColumnCommand>
	//   * M7 walls / slabs …
	//   スキーマを変えるときは構造体・validateDocument・テスト・JSON ダンプを同時更新する。
	struct Document
	{
		int version = kDocumentVersion;

		// M1 通り芯。IfcGridAxis を解析して得た GridCommand の列（入力順に依存しない
		// 決定的な並び。parse/Grid が #id 昇順で組み立てる）。
		std::vector<GridCommand> grids;
	};

	// Document を描画前に検証する（Python 版 validateDocument 相当）。draw/ は
	// 検証を通った Document だけを SDK API へ渡す。骨組みの現状では常に true
	// （空の Document は妥当）。各命令リストの追加に合わせて検証規則を足していく。
	bool validateDocument(const Document& document);
} // namespace HomeskzIfcImport::core
