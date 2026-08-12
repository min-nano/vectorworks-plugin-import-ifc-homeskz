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
//	現状は story（M3）→ grid（M1）→ wall（M9）→ wallJoin（M10）→ slab（M9。地中梁＝M10 を
//	含む）→ floor（M5）→ member（M7）→ column（M8）→ rafter → roof（M6）→ シンボル置換系
//	（M11: アンカーボルト・床束・火打・仕口）→ sheet（M13。伏図。**モデルを映すので必ず
//	最後**）へディスパッチする。残りの要素は対応マイルストーンで足していく。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>

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
		std::size_t columns = 0;
		std::size_t rafters = 0;
		std::size_t roofs = 0;
		std::size_t walls = 0;
		std::size_t wallJoins = 0;
		std::size_t slabs = 0;
		// M11 シンボル置換系。アンカーボルト・床束の配置先（"F-アンカーボルト" / "F-床束"）は
		// 基礎ストーリのレイヤなので、基礎の無いモデルでは命令自体が出ない（parse 側で空になる）。
		std::size_t anchorBolts = 0;
		std::size_t floorPosts = 0;
		std::size_t fireBraces = 0;
		std::size_t joints = 0;

		// M12 断面記号・伏図記号。**span 柱レイヤごとに置いた記号 PIO の数**（記号そのものの
		// 個数ではない——1 つの PIO がそのレイヤの柱すべてに記号を描く）。
		std::size_t columnMarks = 0;

		// M13 シート（伏図）。**ビューポートまで作れた枚数**（シートレイヤだけできた場合は
		// 数えない）。命令はあるのに 0 なら、原因は診断行に出る（draw/Sheet）。
		std::size_t sheets = 0;

		// 進捗ダイアログの「キャンセル」で途中打ち切りになったか。true のときは各要素の
		// 件数が命令数に届かないのが正常で、描けたところまでは図面に残る（Undo の一括化は
		// M15。ROADMAP.md）。完了ダイアログはこれを見て中止を明示する。
		bool cancelled = false;

		// 描画側で起きた異常の説明（無ければ空）。要素ごとに 1 行を改行で連ねる。実描画は
		// ローカルの VectorWorks でしか確認できないので、「命令はあるのに見えない」ときに
		// 原因を解析側と描画側で切り分ける手掛かりをメニューコマンドの完了ダイアログへ持ち帰る。
		std::string diagnostics;
	};

	// 命令セットを描画する。validateDocument を通してから、命令ごとに要素の draw モジュール
	// （story → grid → wall → wallJoin → slab → floor → member → column → rafter → roof →
	// シンボル置換系 → シート（伏図）の順）へディスパッチし、描けた数を返す。
	// 検証を通らなかったときは valid=false で何も描かない。命令が空でも検証は通る（valid=true）。
	//
	// TODO(M11〜): anchorBolt … と、要素ごとの draw モジュールへのディスパッチを足す。
	DrawCounts executeDocument(const core::Document& document);

	// 進捗を報告しながら描画する。要素ごとに 1 フェーズを開き（進捗バーの配分は命令数の比。
	// core::phaseShare）、1 件描くたびに 1 ステップ進める。**インポートの体感時間はほぼ
	// すべてここ**なので、進捗ダイアログを出すのはこのオーバーロードの役目
	// （core/Progress.h「なぜ要るか」）。
	//
	// 進捗の報告先が中止を返したら、その時点で残りを描かずに戻る（DrawCounts::cancelled）。
	// 上のオーバーロードは NullProgressReporter で呼ぶだけ（＝振る舞いは同じ）。
	DrawCounts executeDocument(const core::Document& document, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::draw
