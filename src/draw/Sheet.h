//
//	draw/Sheet.h
//
//	Phase 2（VW 描画）のシート（伏図）モジュール（docs/DEV-NOTES.md M13）。命令セット（core::
//	SheetCommand の列）からシートレイヤ 1 枚とその上のビューポート 1 枚を生成し、表示する
//	デザインレイヤを絞り込む。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document /
//	core::Progress しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【レイヤの重ね順はここでは決めない】床・野地板が柱・梁を覆い隠さないようにする件は、
//	**デザインレイヤ自体の並べ替え**（draw/Story の reorderStoryLayers）が担う。当初は
//	ビューポート単位の重ね順オーバーライド（SetViewportLayerStackingOverride）へ委ねたが、
//	**実機で反映されなかった**（呼び出しは true を返すのに上書き件数は 0 のまま）ので、
//	ドキュメントのレイヤ重ね順そのものを並べ替える（経緯は draw/Story.h の reorderStoryLayers）。
//	ビューポートは**生成時の重ね順で描画される**ので、並べ替えは drawSheets より前に済ませる（順
//	序は draw/ExecuteDocument が持つ）。ここが持つのは**表示レイヤの絞り込みとクラス表示**だけ
//	になる。
//
//	実描画（ビューポートの表示・縮尺・表示レイヤ）はローカルの VectorWorks で目視確認する
//	（docs/DEV-NOTES.md M13）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"
#include "draw/ObjectHandles.h"

#include <cstddef>
#include <functional>
#include <string>

namespace HomeskzIfcImport::draw
{
	// Document 内の全シート（伏図）を描画する。シートごとに
	//   シートレイヤ（無ければ作成・番号がレイヤ名）→ タイトル設定 → ビューポート生成 →
	//   表示レイヤの絞り込み → クラス表示 → 縮尺 → 図面タイトル・図番 → 更新 →
	//   断面寸法データタグ（注釈）→ グラフィック凡例（シートレイヤの上）
	// を行い、**ビューポートまで生成できたシートの数**を返す（シートレイヤだけ生成できた場合は
	// 数えない。「命令はあるのに図が無い」を件数で切り分けられるようにする）。
	//
	// progress には 1 枚ごとに 1 ステップ報告し、**ループの先頭で中止要求を確認して
	// 抜ける**（フェーズの見出しと配分は draw/ExecuteDocument が決める）。描画済みの分は
	// 図面に残る。
	//
	// beginFinishing は 1 巡目（生成）を終えて 2 巡目（縮尺の確定・タグ・寸法・位置合わせ）に
	// 入るときに 1 度呼ぶ。進捗の別フェーズを開けたら true を返し、そのときだけ 2 巡目も
	// 1 枚ごとに 1 ステップ報告する。2 巡目は伏図の所要の 7 割を占めるので、刻まないと
	// バーが数秒止まって見える（core::DrawPhase::SheetsFinish）。2 巡目そのものは中止されても
	// 行う（1 巡目で生成した図を仕上げずに残さないため）。note には異常（ビューポートを生成できなかった等）の説明を入れる（無ければ空）。
	//
	// memberHandles には drawMembers が記録した「命令インデックス → 横架材ハンドル」の
	// 対応表を渡す。**断面寸法データタグの関連付け先**で、渡さない（nullptr）とタグは
	// 置かれるが寸法が空になる（draw/Tag.h）。
	//
	// outInfo には**異常ではない内訳**（用紙の割り付け——用紙・印刷可能領域・凡例の幅・
	// 建物の広がり・選んだ縮尺——と、寸法・図面枠の記録）を入れる。平常でも必ず出力されるので
	// note とは出力先を分ける: 完了ダイアログは note が空かどうかで「問題あり」を判断し、
	// outInfo は診断ログにだけ出力される（core::DrawCounts の diagnostics / notes）。
	//
	// outCounts には**寸法の列とレベル記号の描画できた数**を加算する（M31。core::DrawCounts の
	// dimensions / levelMarks。件数は完了文言の表 parse/Summary の kElements が読む）。
	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note = nullptr,
						   const ObjectHandles* memberHandles = nullptr,
						   std::string* outInfo = nullptr, core::DrawCounts* outCounts = nullptr,
						   const std::function<bool()>& beginFinishing = {});
} // namespace HomeskzIfcImport::draw
