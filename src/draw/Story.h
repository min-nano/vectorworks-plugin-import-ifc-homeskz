//
//	draw/Story.h
//
//	Phase 2（VW 描画）のストーリモジュール。命令セット（core::StoryCommand の列）から
//	VectorWorks のストーリ・ストーリレベル・デザインレイヤを生成する。ExecuteDocument
//	から通り芯より先にディスパッチされる（以降の要素はここで作ったレベルに配置される。
//	docs/DEV-NOTES.md M3）。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document
//	しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【レイヤのスタック順について】並べ替えの目的は「伏図ビューポートで床・野地板が柱・梁を
//	覆い隠さないようにする」こと。希望順の**計算**は SDK 非依存の
//	core::desiredStoryLayerOrder が持ち（無 SDK テスト済み）、**適用は下記の
//	reorderStoryLayers ただ 1 か所**が担う（デザインレイヤ自体は命令の levels の並び順に
//	生成される）。
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

	// レイヤの重ね順を並べ替えた結果。**「動かせた数」だけでは足りない**——既に希望どおりなら
	// 0 件で正常、並べ替えに失敗しても 0 件なので、両者を取り違えないよう並び自体を読み戻す。
	struct LayerOrderResult
	{
		// 実際に動かしたレイヤの数（既に希望どおりなら 0）。
		std::size_t moved = 0;
		// 呼び出し後の並びが希望順と一致しているか（読み戻して確かめた結果）。
		bool ordered = false;
	};

	// デザインレイヤのスタック順を希望順（core::desiredStoryLayerOrder）へ並べ替える。
	// **既に希望どおりなら 1 つも動かさない**（呼び直しても図面を触らない）。
	//
	// 【なぜ要るか】伏図ビューポートは**ドキュメントのレイヤ重ね順で描かれる**ので、
	// 床（"n-FL"）・野地板が柱・梁より前面にあると覆い隠してしまう。希望順は
	// 「共通（通り芯）を最前面 → 最上階→最下階 → 床・野地板は最背面」で、計算そのものは
	// SDK 非依存の core::desiredStoryLayerOrder が持つ（無 SDK テスト済み）。
	//
	// 【ドキュメントの重ね順を並べ替える（per-viewport の上書きではない）】
	// **ISDK には InsertObjectAfter / InsertObjectBefore があり、レイヤは図面のオブジェクト
	// 列に並んでいる**（VectorScript の HMoveForward に当たるのがこの 2 つ）ので、列そのものを
	// 組み替える。ビューポート単位の上書き（SetViewportLayerStackingOverride）は**実機で
	// 効かない**——呼び出しは true を返すのに GetNumViewportLayerStackingOverrides は 0 のまま
	// で、OIP も「順序を上書き: いいえ」だった（docs/DEV-NOTES.md「打ち切った調査」）。
	//
	// **伏図より前に呼ぶこと。** ビューポートは生成時の重ね順で描かれるので、並べ替えを後にす
	// ると既存のビューポートへ反映されない。draw/ExecuteDocument は全要素の描画後・
	// drawSheets の直前に呼ぶ。
	//
	// **並べ替えは既存ビューポートを out-of-date にしない。** 取り込みで作る図には別途
	// 「更新が要る」印を立てる（draw/ExecuteDocument の markImportedViewportsOutOfDate）。
	//
	// **できるだけ早く——要素を 1 つも描く前に——呼ぶこと。** 取り込み直後の伏図だけが
	// 「床が柱・梁を覆ったまま／ユーザーが更新を 1 回押すと直る」という症状を出していた件で、
	// 図面の並び自体は（OIP のレイヤ一覧でも）並べ替え後の順になっていた。つまり**並べ替えの
	// 結果が、同じ取り込みの中で作るビューポートの描画へ届いていない**。届かない理由は
	// VW の内側なので確かめられないが、届かせる手立ては 1 つある——**並べ替えを描画の前に
	// 済ませてしまう**こと。以後の要素描画（何千という図形の追加）を挟めば、ビューポートを
	// 作る時点では並びはとうに落ち着いている。draw/ExecuteDocument は通り芯の直後
	// （伏図記号レイヤを先に用意したうえ）でこれを呼び、伏図の直前にもう一度呼んで
	// 「まだ希望どおりか」を確かめる（2 度目は普通 moved=0 で、図面を触らない）。
	LayerOrderResult reorderStoryLayers(const core::Document& document);
} // namespace HomeskzIfcImport::draw
