//
//	draw/ShearWallPio.h
//
//	**耐力壁（筋かい・面材）PIO が、リセットのたびに実際に描画する処理。**
//
//	【なぜ拡張クラスから出してあるか】柱記号 PIO と同じ理由——PIO の**登録**は
//	Vectorworks にアドレスを保持されるので殻に残すほかないが、**描画処理は本体
//	（ペイロード）へ移せる**。そうしておくと描画方法の修正が**Vectorworks の再起動なしに**
//	反映される（src/PayloadAbi.h / src/PayloadSession.h / draw/ColumnMarkPio.h）。
//
//	耐力壁そのものの意図（何を描画するか・座標系）は Extensions/ExtShearWall.h にある。
//

#pragma once

#include "PluginPrefix.h"
#include "draw/Verify.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	// EObjectEvent / kObjectEvent* は **VWFC::PluginSupport** にある。SDK のアンブレラ
	// （PluginPrefix.h）はこの名前空間を開かないので、**このヘッダ自身で開く**
	// （Extensions/ExtColumnMark.h と同じ方法）。これが無いと、Ext*.h を先に include して
	// いない翻訳単位——本体の入口 src/payload/PayloadMain.cpp——でだけ
	// 「unknown type name 'EObjectEvent'」になる。
	using namespace VWFC::PluginSupport;

	// 耐力壁 PIO 1 枚分のリセット。object は PIO 自身のハンドル（殻の
	// VWParametric_EventSink::fhObject が渡ってくる）。1 枚の異常で全体を停止させない
	// ので、返るのは実質 kObjectEventNoErr だけ。
	EObjectEvent recalculateShearWall(MCObjectHandle object);

#if VW_DRAW_VERIFY
	// **描画せずに**、リセットと同じ決め方で軸組内法を再計算して 1 行にする（開発ビルドの
	// 再測定。draw/ShearWall の recheckShearWalls が取り込みの最後に呼ぶ）。
	//
	// 【なぜ要るか】「取り込み後に OIP で 1 度編集すると、耐力壁が柱幅の半分ほど始端側へ
	// ずれ、2 度目以降はずれない」不具合を追跡するために追加した（docs/DEV-NOTES.md M19。#161 で
	// 修正し、現在は再発の監視）。ずれ幅は
	// 「柱から求めた内法」と「控えの内法」の差にちょうど一致するので、**取り込みの後の
	// どこかで柱が見つからなくなっている**疑いが強い。ところが利用者の編集で実行されるリセットは
	// 診断ログが閉じた後なので、何が起きたかが残らない。取り込みの最後（伏図・軸組図・
	// レイヤ縮尺の変更まで済んだ後）に同じ決め方を実行すれば、「その時点で既に柱が
	// 見つからないのか」「編集のときに限るのか」を分けられる。
	//
	// 数えるのは状態（kind）で、文言（text）は診断ログに載せるだけ——文言の先頭で
	// 分類すると、言い回しを修正しただけで数え方が変わる。
	struct ShearWallProbe
	{
		enum class Kind
		{
			FromColumns, // 両端の柱から内法を求められた
			Fallback,	 // 柱を検出できず、控えの内法で描画する
			Undecided,	 // 内法が決まらない（描画しない）
		};
		Kind kind = Kind::Undecided;
		std::string text; // 内法の決まり方の 1 行（診断ログ用）
	};
	ShearWallProbe probeShearWall(MCObjectHandle object);
#endif
} // namespace HomeskzIfcImport::draw
