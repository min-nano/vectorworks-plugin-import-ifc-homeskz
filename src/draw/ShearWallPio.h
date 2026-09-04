//
//	draw/ShearWallPio.h
//
//	**耐力壁（筋かい・面材）PIO が、リセットのたびに実際に描くところ。**
//
//	【なぜ拡張クラスから出してあるか】柱記号 PIO と同じ理由——PIO の**登録**は
//	Vectorworks に番地を握られるので殻に残すほかないが、**絵を描くところは本体
//	（ペイロード）へ出せる**。そうしておくと描き方の直しが**Vectorworks の再起動なしに**
//	反映される（src/PayloadAbi.h / src/PayloadSession.h / draw/ColumnMarkPio.h）。
//
//	耐力壁そのものの意図（何を描くか・座標系）は Extensions/ExtShearWall.h にある。
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

	// 耐力壁 PIO 1 枚ぶんのリセット。object は PIO 自身のハンドル（殻の
	// VWParametric_EventSink::fhObject が渡ってくる）。1 枚の異常で全体を落とさない
	// ので、返るのは実質 kObjectEventNoErr だけ。
	EObjectEvent recalculateShearWall(MCObjectHandle object);
} // namespace HomeskzIfcImport::draw
