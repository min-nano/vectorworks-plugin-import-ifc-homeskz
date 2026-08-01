//
//	draw/ExecuteDocument.h
//
//	Phase 2（VW 描画）のディスパッチ。Python 版 vw/__init__.py の execute_document
//	に対応する。命令セット（core::Document）を検証してから、命令ごとに要素の
//	draw モジュール（Grid / Story / Member …）へ振り分けて SDK API で描画する。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため、SDK ビルドでのみコンパイル
//	される（無 SDK の core/parse ライブラリには含めない）。この宣言ヘッダ自体は
//	core::Document しか参照せず、SDK ヘッダを引き込まない。
//
//	現状は story（M3）→ grid（M1）→ floor（M5）→ member（M7）→ rafter → roof（M6）へ
//	ディスパッチする。残りの要素は対応マイルストーンで足していく。
//

#pragma once

#include "core/Document.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// 実際に**描けた**数（命令数ではない）。メニューコマンドの完了ダイアログはこれを出す。
	// 命令はあるのに 0 なら「配置先レイヤが無い」「PIO / オブジェクトを作れなかった」等の
	// 描画側の問題だと分かり、ローカル確認で原因を切り分けられる（命令数は解析側の Document
	// から別途取れる）。valid は validateDocument を通ったか。
	struct DrawCounts
	{
		bool valid = false;
		std::size_t stories = 0;
		std::size_t grids = 0;
		std::size_t floors = 0;
		std::size_t members = 0;
		std::size_t rafters = 0;
		std::size_t roofs = 0;
	};

	// 命令セットを描画する。validateDocument を通してから、命令ごとに要素の draw モジュール
	// （story → grid → floor → member → rafter → roof の順）へディスパッチし、描けた数を返す。
	// 検証を通らなかったときは valid=false で何も描かない。命令が空でも検証は通る（valid=true）。
	//
	// TODO(M8〜): column … と、要素ごとの draw モジュールへのディスパッチを足す。
	DrawCounts executeDocument(const core::Document& document);
} // namespace HomeskzIfcImport::draw
