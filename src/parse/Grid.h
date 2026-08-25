//
//	parse/Grid.h
//
//	Phase 1（IFC 解析）の通り芯モジュール。IfcGridAxis を辿ってポリライン端点を取り出し、
//	重複線除去・bbox 中心でのセンタリング・X/Y 通り判定・クラス名付与を行って GridCommand
//	の列を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step の Model）と自前幾何（core/Geometry の Vec2）だけで完結し、
//	通常の C++ ツールチェインでコンパイル・単体テストできる（CLAUDE.md「Phase 1」）。
//
//	通り芯が最初の縦切り（docs/DEV-NOTES.md M1）である理由: 配置行列・断面・ストーリを一切
//	必要とせず、IfcGridAxis → IfcPolyline の端点 → 中心オフセット → GridAxis という
//	最短経路で 2 フェーズが端から端まで通ることを実証できる。
//

#pragma once

#include "core/Document.h"
#include "parse/Step.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 解析途中の 1 本の通り芯（センタリング**前**の生端点＋軸名）。センタリング中心は
	// 全端点の bbox から決まるので、線分の収集と中心の算出を分けられるようにこの中間型を
	// 公開する（床・垂木・野地板も同じ中心を使うため、線分収集は 1 回で済ませたい）。
	struct GridLine
	{
		std::string label;
		core::Vec2 start;
		core::Vec2 end;
	};

	// IfcGridAxis から通り芯の線分を集める。連続する点対（区間）ごとに 1 本を作り、
	// 幾何的に重複する線分（向きの反転も同一）は最初に現れた 1 本へ畳む。#id 昇順・
	// 点順で処理するので、入力の列挙順に依存しない決定的な並びになる。
	std::vector<GridLine> collectGridLines(const Model& model);

	// 通り芯 1 本が X 通り（＝定 X の縦線）か。軸名の先頭が X/Y ならそれに従い（大文字小文字を
	// 無視）、判別できなければ線の向き（|Δx| < |Δy| なら縦線＝X 通り）で決める。
	//
	// **X/Y の判定はここが唯一の定義**で、通り芯のクラス分け（buildGridCommands）と軸組図の
	// 通り名の照合（parse/Section の namedAxes）が同じ述語を通す。別々に書くと「通り芯は
	// X 通りなのに軸組図では Y 通り」という食い違いが起きうる（CLAUDE.md「重複を作らない置き場所」）。
	bool isXAxis(const GridLine& line);

	// 線分群の bbox 中心（＝**全要素に共通のセンタリングオフセット**）を返す。床・屋根組・
	// 基礎・部材はいずれもこのオフセットで平面座標を補正する（要素ごとに別の中心を使うと図面
	// がずれる）。線分が空なら false（out は変更しない）＝補正しない。
	//
	// 解析中は parse/Context がこの結果を 1 度だけ求めて共有する（Context::gridCenter）。
	bool gridCenterOf(const std::vector<GridLine>& lines, core::Vec2& out);

	// STEP Model から通り芯の描画命令を組み立てる。
	//
	// 手順（docs/DEV-NOTES.md M1）:
	//   1. IfcGridAxis の AxisCurve(IfcPolyline) の全点を取り、連続する点対（線分）
	//      ごとに 1 本を作る（多点ポリラインは複数本になる）。
	//   2. 幾何的に重複する線分を除去（反転も同一とみなす）。
	//   3. 全端点の bbox 中心を原点へ移すセンタリング（VW 上で原点付近に置く）。
	//   4. X/Y 通り判定（軸名が X/Y で始まればそれ、無ければ |Δx|<|Δy| で縦横判定）。
	//   5. 判定に応じてクラス名を付与。
	//
	// 入力（IfcGridAxis）の列挙順に依存しない決定的な結果を返す（#id 昇順で処理し、
	// 重複除去も最初に現れた 1 本を残す）。1 本の欠損（曲線未解決・点数不足）で全体を止めず、
	// その軸だけスキップする（CLAUDE.md「エラーハンドリング」）。
	std::vector<core::GridCommand> buildGridCommands(const Model& model);

	// 同上。共有コンテキストの線分・センタリング中心を使う（parse/Context.h）。
	std::vector<core::GridCommand> buildGridCommands(Context& context);
} // namespace HomeskzIfcImport::parse
