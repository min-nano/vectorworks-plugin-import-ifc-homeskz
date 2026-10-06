//
//	draw/ColumnMarkPio.h
//
//	**柱・小屋束の記号 PIO が、リセットのたびに実際に描画する処理。**
//
//	【なぜ拡張クラスから分離してあるか】PIO の**登録**（SParametricDef・パラメータ定義・
//	UUID）は Vectorworks がその番地を保持するので殻——起動時に読み込まれるモジュール——に
//	残さなければならない。しかし**描画する処理は殻に置く必要が無い**ので本体（ペイロード）
//	へ移す。こうしておくと記号の描画方法を修正したビルドが、**Vectorworks を再起動せずに**
//	反映される（src/PayloadAbi.h / src/PayloadSession.h）。
//
//	記号そのものの意図（なぜ PIO か・何を描画するか）は Extensions/ExtColumnMark.h にある。
//

#pragma once

#include "PluginPrefix.h"

namespace HomeskzIfcImport::draw
{
	// EObjectEvent / kObjectEvent* は **VWFC::PluginSupport** にある。SDK のアンブレラ
	// （PluginPrefix.h）はこの名前空間を開かないので、**このヘッダ自身で開く**
	// （Extensions/ExtColumnMark.h と同じ作法）。これが無いと、Ext*.h を先に include して
	// いない翻訳単位——本体の入口 src/payload/PayloadMain.cpp——でだけ
	// 「unknown type name 'EObjectEvent'」になる。
	using namespace VWFC::PluginSupport;

	// 記号 PIO 1 つぶんのリセット。object は PIO 自身のハンドル（殻の
	// VWParametric_EventSink::fhObject が渡ってくる）。1 本の異常で記号全体を
	// 失敗させないので、返るのは実質 kObjectEventNoErr だけ。
	EObjectEvent recalculateColumnMark(MCObjectHandle object);
} // namespace HomeskzIfcImport::draw
