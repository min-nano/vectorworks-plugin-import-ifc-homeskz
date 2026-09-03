//
//	draw/ColumnMarkPio.h
//
//	**柱・小屋束の記号 PIO が、リセットのたびに実際に描くところ。**
//
//	【なぜ拡張クラスから出してあるか】PIO の**登録**（SParametricDef・パラメータ定義・
//	UUID）は Vectorworks に番地を握られるので殻——起動時に読み込まれるモジュール——に
//	残さなければならない。しかし**絵を描くところは殻に置く必要が無い**ので本体（ペイロード）
//	へ出す。こうしておくと記号の描き方を直したビルドが、**Vectorworks を再起動せずに**
//	反映される（src/PayloadAbi.h / src/PayloadSession.h）。
//
//	記号そのものの意図（なぜ PIO か・何を描くか）は Extensions/ExtColumnMark.h にある。
//

#pragma once

#include "PluginPrefix.h"

namespace HomeskzIfcImport
{
	namespace draw
	{
		// 記号 PIO 1 つぶんのリセット。object は PIO 自身のハンドル（殻の
		// VWParametric_EventSink::fhObject が渡ってくる）。1 本の異常で記号全体を
		// 落とさないので、返るのは実質 kObjectEventNoErr だけ。
		EObjectEvent recalculateColumnMark(MCObjectHandle object);
	} // namespace draw
} // namespace HomeskzIfcImport
