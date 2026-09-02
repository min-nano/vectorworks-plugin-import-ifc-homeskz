//
//	draw/ShearWall.h
//
//	Phase 2（VW 描画）の耐力壁モジュール（docs/DEV-NOTES.md M19）。命令 1 つにつき
//	**線分 PIO を 1 つ**置き、両端（柱芯）・種別・断面・内法をパラメータへ書いてリセットする。
//	耐力壁そのものを描くのは PIO 本体（Extensions/ExtShearWall）で、ここはその設置だけを担う。
//
//	【PIO は本プラグインが提供する】柱記号（draw/ColumnMark）と同じく**同じモジュールへ
//	同梱**する（ModuleMain がメニューコマンドと一緒に登録する）。インストールも自動更新も
//	1 つで済み、耐力壁の規約（柱の探し方・クラス名）を解析側と 1 か所で共有できる。
//
//	【両端は柱芯】線分 PIO の 2 点は命令の start / end ＝**柱芯**で、実際に絵を描く範囲
//	（軸組内法）は PIO がリセット時に見つけた柱の断面から引く。したがって柱を動かして
//	リセットすれば、耐力壁が追随して伸縮する。
//
//	【レイヤはストーリが作る】配置先の "n-耐力壁" はストーリのレベルなので**無ければ
//	スキップ**（ActivateExistingLayer の規約）。柱記号の伏図記号レイヤのように自分で
//	作ることはしない。
//
//	**柱の描画後に呼ぶこと。** PIO はリセット時に対象レイヤの柱を検索するので、柱が
//	置かれていないと控えの内法で描かれてしまう。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// 耐力壁（shearWall 命令）を線分 PIO として置く。置けた数を返す。配置先レイヤが
	// 用意できない命令・PIO を作れない命令はスキップし、その件数を outNote に残す
	// （完了ダイアログの診断。draw/ColumnMark と同じ流儀）。
	std::size_t drawShearWalls(const core::Document& document, core::ProgressReporter& progress,
							   std::string* outNote = nullptr);

	// 耐力壁レイヤ（"n-耐力壁"）の縮尺を伏図の縮尺へ揃える。揃えられたレイヤ数を返す。
	//
	// 【なぜ要るか】伏図記号のシンボルは**用紙基準（縮尺無視）**で、用紙基準の大きさは
	// 「シンボル定義の図形（用紙 mm）× そのレイヤの縮尺」で決まる（実機で確認: 図形
	// 300mm・用紙基準のシンボルを縮尺 1/100 のレイヤへ置くと外接 30000mm）。伏図は
	// ビューポート越しに見るので、**レイヤの縮尺を伏図の縮尺に揃えて初めて**紙の上の
	// 大きさが一定になる。揃っていないと記号が桁違いに大きく／小さく出る。
	//
	// **伏図の縮尺は用紙を読まないと決まらない**（core::planLayout）ので、レイヤを作る
	// ときには分からない。したがってこれは**シートを組む側から**、割り付けが確定した
	// あとに呼ぶ（draw/Sheet）。PIO はレイヤ縮尺の変化で描き直す印
	// （kObjXPropHasLayerScaleDeps）を立ててあるので、呼べば記号が付いて回る。
	std::size_t applyShearWallLayerScale(const core::Document& document, double scale);
} // namespace HomeskzIfcImport::draw
