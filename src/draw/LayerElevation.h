//
//	draw/LayerElevation.h
//
//	デザインレイヤの基準高さ（レイヤ平面の Z）を読む小さな共有ヘルパー。
//
//	命令セット（core::Document）の高さは**絶対 Z**（ストーリの Elevation を足した値）だが、
//	VW のオブジェクトの Z は**レイヤ座標**（レイヤ平面が Z=0）で表される。したがって描画側は
//	どの要素でも「絶対 Z − レイヤの基準高さ」でレイヤ相対に直してから渡す必要がある。
//	この変換規約が要素ごとに散らばると片方だけ直し忘れて高さがずれるので、読み取りは
//	ここ 1 か所に集約する（CLAUDE.md「変換規約を分散させない」）。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）を include する。このヘッダは
//	SDK 型を宣言に使うため、SDK ビルドの翻訳単位からのみ include する。
//

#pragma once

#include "PluginPrefix.h"

namespace HomeskzIfcImport::draw
{
	// デザインレイヤの基準高さ。読めなければ 0（＝補正なし）を返す。
	//
	// ovLayerHeightInCurrUnits は「現在の単位でのレイヤの基準高さ」（Kernel/API/
	// ObjectVariables.h）。本プラグインが扱う図面は mm 単位なので WorldCoord と一致する。
	double layerElevation(MCObjectHandle layer);
} // namespace HomeskzIfcImport::draw
