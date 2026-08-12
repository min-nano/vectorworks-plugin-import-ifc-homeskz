//
//	draw/Story.h
//
//	Phase 2（VW 描画）のストーリモジュール。Python 版 vw/story.py に対応する。
//	命令セット（core::StoryCommand の列）から VectorWorks のストーリ・ストーリレベル・
//	デザインレイヤを生成する。ExecuteDocument から通り芯より先にディスパッチされる
//	（以降の要素はここで作ったレベルに配置される。ROADMAP.md M3）。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document
//	しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【レイヤのスタック順について（Python 版との意図的な差異）】Python 版 vw/story.py は
//	HMoveForward でデザインレイヤのスタック順を並べ替えていたが、VectorWorks 2026 SDK の
//	ISDK にはデザインレイヤの重ね順を変更する呼び出しが無い（HMoveForward 相当が無い）。
//	一方 2026 SDK は Python 版当時に無かった**ビューポート単位のレイヤ重ね順オーバーライド**
//	（SetViewportLayerStackingOverride）を提供する。並べ替えの目的は「伏図ビューポートで
//	床・野地板が柱・梁を覆い隠さないようにする」ことなので、これはビューポート導入（M13）で
//	その per-viewport API を使う方が Python 版のグローバル並べ替えより適切に扱える
//	（CLAUDE.md「C++ SDK でより良く実装できたもの」）。M3 では希望スタック順の**計算**だけを
//	SDK 非依存の core::desiredStoryLayerOrder に用意し（無 SDK テスト済み）、実際の適用は
//	M13 へ委ねる（デザインレイヤ自体は命令の levels の並び順に生成される）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document 内の全ストーリを描く。まず命令に登場するレベル種別を登場順に登録し、各
	// StoryCommand ごとに CreateStory（同名のストーリが既にあればそれを再利用）→
	// SetStoryElevation → 各レベルをレベルテンプレートで生成（レイヤも同時に作成し
	// 意図した名前へリネーム）する。実際に用意できたストーリ数を返す。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描けたところまでは図面に残る。
	std::size_t drawStories(const core::Document& document, core::ProgressReporter& progress);

	// デザインレイヤのスタック順を希望順（core::desiredStoryLayerOrder）へ並べ替える
	// （Python 版 vw/story.py の reorder_story_layers に対応）。動かせたレイヤ数を返す。
	//
	// 【なぜ要るか】伏図ビューポートは**ドキュメントのレイヤ重ね順で描かれる**ので、
	// 床（"n-FL"）・野地板が柱・梁より前面にあると覆い隠してしまう。希望順は
	// 「共通（通り芯）を最前面 → 最上階→最下階 → 床・野地板は最背面」で、計算そのものは
	// SDK 非依存の core::desiredStoryLayerOrder が持つ（無 SDK テスト済み）。
	//
	// 【ヘッダ冒頭の「M3 の【決定】」を差し替える】M3 では「ISDK にデザインレイヤの重ね順を
	// 変える呼び出しが無い」と判断して per-viewport の上書き
	// （SetViewportLayerStackingOverride）に委ねたが、**実機で per-viewport 上書きは効かず**
	// （呼び出しは true を返すのに GetNumViewportLayerStackingOverrides は 0 のまま・OIP も
	// 「順序を上書き: いいえ」）、一方で **ISDK には InsertObjectAfter / InsertObjectBefore が
	// あり、レイヤは図面のオブジェクト列に並んでいる**（VW の HMoveForward 相当がこの名前で
	// 存在する）。したがって Python 版と同じ「ドキュメントの重ね順を並べ替える」方式に戻す。
	//
	// **伏図より前に呼ぶこと。** ビューポートは生成時の重ね順で描かれるので、並べ替えを
	// 後にすると既存のビューポートへ反映されない（Python 版が「手動更新が要る」と諦めていた
	// 現象そのもの）。draw/ExecuteDocument は全要素の描画後・drawSheets の直前に呼ぶ。
	std::size_t reorderStoryLayers(const core::Document& document);
} // namespace HomeskzIfcImport::draw
