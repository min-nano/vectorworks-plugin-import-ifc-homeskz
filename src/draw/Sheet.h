//
//	draw/Sheet.h
//
//	Phase 2（VW 描画）のシート（伏図）モジュール（docs/DEV-NOTES.md M13）。命令セット（core::
//	SheetCommand の列）からシートレイヤ 1 枚とその上のビューポート 1 枚を作り、表示する
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
//	**実機で効かなかった**（呼び出しは true を返すのに上書き件数は 0 のまま）ので、
//	ドキュメントのレイヤ重ね順そのものを並べ替える（経緯は draw/Story.h の reorderStoryLayers）。
//	ビューポートは**生成時の重ね順で描かれる**ので、並べ替えは drawSheets より前に済ませる（順
//	序は draw/ExecuteDocument が持つ）。ここが持つのは**表示レイヤの絞り込みとクラス表示**だけ
//	になる。
//
//	実描画（ビューポートの見え方・縮尺・表示レイヤ）はローカルの VectorWorks で目視確認する
//	（docs/DEV-NOTES.md M13「ローカル確認」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"
#include "draw/ObjectHandles.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// Document 内の全シート（伏図）を描く。シートごとに
	//   シートレイヤ（無ければ作成・番号がレイヤ名）→ タイトル設定 → ビューポート生成 →
	//   表示レイヤの絞り込み → クラス表示 → 縮尺 → 図面タイトル・図番 → 更新 →
	//   断面寸法データタグ（注釈）→ グラフィック凡例（シートレイヤの上）
	// を行い、**ビューポートまで作れたシートの数**を返す（シートレイヤだけ作れた場合は
	// 数えない。「命令はあるのに図が無い」を件数で切り分けられるようにする）。
	//
	// progress には 1 枚ごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （フェーズの見出しと配分は draw/ExecuteDocument が決める）。描けたところまでは
	// 図面に残る。note には異常（ビューポートを作れなかった等）の説明を入れる（無ければ空）。
	//
	// memberHandles には drawMembers が記録した「命令インデックス → 横架材ハンドル」の
	// 対応表を渡す。**断面寸法データタグの関連付け先**で、渡さない（nullptr）とタグは
	// 置かれるが寸法が空になる（draw/Tag.h）。
	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note = nullptr,
						   const ObjectHandles* memberHandles = nullptr);
} // namespace HomeskzIfcImport::draw
