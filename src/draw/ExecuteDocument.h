//
//	draw/ExecuteDocument.h
//
//	Phase 2（VW 描画）のディスパッチ。命令セット（core::Document）を検証してから、
//	命令ごとに要素の draw モジュール（Grid / Story / Member …）へ振り分けて SDK API で描画する。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため、SDK ビルドでのみコンパイル
//	される（無 SDK の core/parse ライブラリには含めない）。この宣言ヘッダ自体は
//	core::Document しか参照せず、SDK ヘッダを引き込まない。
//
//	現状は story（M3）→ grid（M1）→ wall（M9）→ wallJoin（M10）→ slab（M9。地中梁＝M10 を
//	含む）→ floor（M5）→ member（M7）→ column（M8）→ rafter → roof（M6）→ シンボル置換系
//	（M11: アンカーボルト・床束・火打・仕口）→ columnMark（M12）→ sheet（M13。伏図）→
//	section（M14。軸組図）へディスパッチする。伏図と軸組図は**モデルを映すので必ず最後**。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"
#include "draw/ObjectHandles.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// 実際に**描けた**数（命令数ではない）。定義は core::DrawCounts（core/Document.h）に
	// あり、draw/ 側はこの別名で受ける。**命令セットと対になる「その実行結果」なので、
	// フェーズをつなぐ唯一の境界であるあちらに置いてある**——完了文言の整形を無 SDK 側
	// （parse/Summary）で行うために、parse/ からも同じ型を読める必要があるため
	// （docs/DEV-NOTES.md M15「完了文言の集約」／CLAUDE.md「依存の向きは厳守する」）。
	using DrawCounts = core::DrawCounts;

	// 命令セットを描画する。validateDocument を通してから、命令ごとに要素の draw モジュール
	// （story → grid → wall → wallJoin → slab → floor → member → column → rafter → roof →
	// シンボル置換系 → シート（伏図）→ 軸組図（断面ビューポート）の順）へディスパッチし、
	// 描けた数を返す。
	// 検証を通らなかったときは valid=false で何も描かない。命令が空でも検証は通る（valid=true）。
	DrawCounts executeDocument(const core::Document& document);

	// 取り込みで作った図（伏図・軸組図）を描き直した結果。
	struct ViewportRefresh
	{
		std::size_t total = 0;	   // 作った図の枚数
		std::size_t refreshed = 0; // そのうち描き直せた枚数
	};

	// 取り込みで作った図を**描き直す**（executeDocument が outViewports へ預けたもの）。
	//
	// **必ず「取り込みが終わり切ってから」呼ぶこと**——undo イベントを閉じた後、かつ
	// **進捗ダイアログを閉じた後**（Extensions/ExtMenu の RunImport）。取り込みの途中で
	// 描き直すと、デザインレイヤの重ね順の並べ替えが描画へ届かず、床（"n-FL"）が柱・梁を
	// 覆ったままの絵が焼き付く。実機で分かっているのは次のとおり:
	//   * 並べ替えの位置（伏図の直前／要素を描く前）を変えても直らない。
	//   * out-of-date を立てて（SetDirty）描き直しても直らない。並べ替え後の並びは
	//     読み戻しでも OIP でも正しく、描き直し自体も走っている（IsDirty が下りる）。
	//   * **ユーザーが「更新」を押す・ファイルを開き直す**と正しく描かれる。
	// 共通しているのは「取り込みのコマンドが終わっている」こと。そこで描き直しだけを
	// 取り込みの外へ出し、届く場所まで遅らせる（docs/DEV-NOTES.md「ビューポート（伏図・断面）」）。
	ViewportRefresh refreshImportedViewports(const ObjectHandles& viewports);

	// 進捗を報告しながら描画する。要素ごとに 1 フェーズを開き（進捗バーの配分は命令数の比。
	// core::phaseShare）、1 件描くたびに 1 ステップ進める。**インポートの体感時間はほぼ
	// すべてここ**なので、進捗ダイアログを出すのはこのオーバーロードの役目
	// （core/Progress.h「なぜ要るか」）。
	//
	// 進捗の報告先が中止を返したら、その時点で残りを描かずに戻る（DrawCounts::cancelled）。
	// 上のオーバーロードは NullProgressReporter で呼ぶだけ（＝振る舞いは同じ）。
	//
	// outViewports には作った伏図・軸組図のビューポートを預ける（渡さなければ預けない）。
	// **描き直しは呼び出し側が取り込みの外で行う**（上の refreshImportedViewports）。
	DrawCounts executeDocument(const core::Document& document, core::ProgressReporter& progress,
							   ObjectHandles* outViewports = nullptr);
} // namespace HomeskzIfcImport::draw
