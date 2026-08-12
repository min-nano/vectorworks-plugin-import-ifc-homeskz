//
//	draw/Sheet.h
//
//	Phase 2（VW 描画）のシート（伏図）モジュール。Python 版 vw/sheet.py の execute_sheets /
//	draw_sheet に対応する（ROADMAP.md M13）。命令セット（core::SheetCommand の列）から
//	シートレイヤ 1 枚とその上のビューポート 1 枚を作り、表示するデザインレイヤを絞り込む。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document /
//	core::Progress しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【レイヤの重ね順は per-viewport 上書きで決める（M3 の【決定】の実装箇所）】
//	床・野地板が柱・梁を覆い隠さないようにする、という Python 版の目的は、Python 版では
//	**デザインレイヤ自体の並べ替え**（HMoveForward）で満たしていた。VW 2026 の ISDK には
//	その呼び出しが無く、代わりに**ビューポート単位のレイヤ重ね順オーバーライド**
//	（SetViewportLayerStackingOverride）がある。本移植はそちらを使う:
//	  * デザインレイヤの並びは触らない（ユーザーが並べ替えた順を壊さない）。
//	  * 希望順は core::desiredStoryLayerOrder（SDK 非依存・無 SDK テスト済み）が唯一の定義で、
//	    全ビューポートが同じ 1 本を使う。
//	  * Python 版が抱えていた「並べ替えの結果が既存ビューポートに反映されず、手動更新が
//	    必要」という制約は、ビューポートごとに上書きを持たせるこの方式では起きない。
//
//	実描画（ビューポートの見え方・縮尺・重ね順の向き）はローカルの VectorWorks で目視確認する
//	（ROADMAP.md M13「ローカル確認」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// Document 内の全シート（伏図）を描く。シートごとに
	//   シートレイヤ（無ければ作成・番号がレイヤ名）→ タイトル設定 → ビューポート生成 →
	//   表示レイヤの絞り込み → レイヤ重ね順の上書き → 縮尺 → 図面タイトル・図番 → 更新
	// を行い、**ビューポートまで作れたシートの数**を返す（シートレイヤだけ作れた場合は
	// 数えない。「命令はあるのに図が無い」を件数で切り分けられるようにする）。
	//
	// progress には 1 枚ごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （フェーズの見出しと配分は draw/ExecuteDocument が決める）。描けたところまでは
	// 図面に残る。note には異常（ビューポートを作れなかった等）の説明を入れる（無ければ空）。
	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note = nullptr);
} // namespace HomeskzIfcImport::draw
