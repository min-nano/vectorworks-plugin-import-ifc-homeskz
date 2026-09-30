//
//	parse/Splice.h
//
//	Phase 1（IFC 解析）の継手モジュール（docs/DEV-NOTES.md「横架材の継手（M33）」）。仕口
//	（parse/Joint）と同じく横架材の命令（parse/Member）の平面ジオメトリから「同一直線上で
//	材端どうしが突き付く箇所」を判定し、そこへ継手シンボルを置く。向き（どちらが女木か）は
//	アンカーボルト・柱・床束の位置から決める。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。core/Document の命令構造体
//	しか触らないので、STEP グラフにも依存しない純粋なジオメトリ計算になる（入力の
//	アンカーボルト・床束の位置は parse/BuildDocument が集めて渡す）。
//
//	【ホームズ君 IFC での継手の現れ方】横架材は**継手の位置で 2 本に分かれて**出力され、2 本は
//	同一直線上で**材の端（端部オフセットを戻した点）がぴったり一致する**（実フィクスチャで
//	隙間 0.0mm）。これに対し、直交する材が間を通り抜けている箇所（両側が仕口）は、両側の
//	材端がその材の面で止まるので**相手の幅（105mm）ぶん離れる**——命令の端点（芯線の交点）は
//	一致していても、材の端は一致しない。したがって**材の端どうしの距離**で見れば、継手と
//	「直交材を挟んだ突き付け」を取り違えない。
//
//	IFC にも継手は IfcMechanicalFastener "継手:…"（型 "継手:T1" 等）として出ているが、
//	**軸に平行な正方形断面の直方体が継手の中心に置かれるだけ**で向きを持たず、土台の継手は
//	出力されない。そこで位置は横架材の命令から自前で求め、向きは下記の規則で決める。
//
//	要件:
//	  * **1 箇所に 1 つ**置く（継手は 2 本の材が共有する 1 点）。
//	  * 継手とみなすのは、**同じレイヤ**・**平面上で平行**・**Z 範囲が重なる** 2 本の横架材
//	    の、**材の端どうしが許容値以内で一致し**、かつ**互いに反対側へ伸びる**（重なって
//	    いない）組だけ。
//	  * **基準点は材の端の一致点**（天端中央）。**配置先は横架材自身のレイヤ**。
//	  * **高さは 2 本の端部のバウンド offset の大きい方**（天端の高い方。仕口と同じ考え方）。
//	  * **向き: シンボルの +X が女木（めぎ。受ける側）を向く。**
//	      - 土台 … **M12 のアンカーボルトは男木（おぎ）に設ける**のが通例なので、継手に
//	               近い M12（座金付き）アンカーボルトが載っている側を男木、反対側を女木とする。
//	      - 梁・桁・大引・母屋など土台以外 … **支点側が女木**（女木が支点を越えて持ち出し、
//	               男木がそこへ掛かる）。継手に近い支点（下階の柱・小屋束。大引は床束）が
//	               ある側を女木とする。ただし**継手をはさむ 2 つの支点の間隔が短い
//	               （kSpliceShortSpan 以下）**ときは近い方が当てにならないので、スパンの
//	               長い継手で確かめた「継手から支点までの距離」（レイヤごとの中央値。
//	               無ければ全体の中央値）と同じ距離にある側を女木とする。
//	    どちらの側がより近いかで決め、**決まらない（どちらにも無い・同じ距離）ときは材軸を
//	    (−90°, 90°] に正規化した向き**にする（並び順に依存させないため）。
//
//	判定は命令のジオメトリだけで決まり、組 (i, j) を i < j の順に並べるので、並びは members の
//	並び順に従い、**各箇所の可否と向きは並びに依存しない**。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "core/ImportOptions.h"

#include <vector>

namespace HomeskzIfcImport::parse
{
	// 置換するハイブリッドシンボル名は**取り込み設定が持つ**（core::SymbolRole::Splice。
	// 既定は "継手"）。設定ダイアログで図面の別のシンボルへ差し替えられる
	// （core/ImportOptions.h）。

	// 継手の判定に使う許容値（mm・単位ベクトルの外積）。
	//
	// kSpliceEndTol … 材の端どうしの距離。実データでは継手は 0.0mm、直交材を挟んだ突き付けは
	//                 相手の幅（105mm 以上）離れるので、1mm で両者を取り違えない。
	// kSpliceParallelTol … 単位軸どうしの外積（≈ 角度の正弦）。0.001 ≈ 0.06 度。
	// kSpliceZOverlapTol … これ以下の Z 重なりは同じ高さの材とみなさない（仕口と同じ値）。
	inline constexpr double kSpliceEndTol = 1.0;
	inline constexpr double kSpliceParallelTol = 1e-3;
	inline constexpr double kSpliceZOverlapTol = 1.0;

	// 向きの判定に使う許容値（mm）。
	//
	// kSpliceSideTol … 支点・アンカーボルトが材の上にあるとみなす直交方向の余裕（半幅に足す）。
	// kSpliceAlongTol … 軸方向の余裕。継手からこれ以内の支点・アンカーボルトはどちらの側とも
	//                   決めない（継手の真上・真下にあるものは向きの手掛かりにならない）。
	//                   材の反対の端を越える側の範囲にも同じ余裕を足す。
	// kSpliceSupportZTol … 柱の上端が材の下端からこれ以内なら、その材を受ける支点とみなす
	//                      （柱の上端は受ける横架材の下端に止まる。core の columnDrawnTop）。
	inline constexpr double kSpliceSideTol = 1.0;
	inline constexpr double kSpliceAlongTol = 1.0;
	inline constexpr double kSpliceSupportZTol = 10.0;
	// kSpliceShortSpan … 継手をはさむ 2 つの支点の間隔がこれ以下なら「短いスパン」。継手が
	//                    支点のほぼ中間に来るので「近い方が女木」が当てにならない（実機で
	//                    確かめたご指摘。概ね 500mm 以下）。そのときは他の継手で確かめた
	//                    「継手から支点までの距離」と同じ距離にある側を女木とする。
	inline constexpr double kSpliceShortSpan = 500.0;

	// 向きの手掛かり（すべてセンタリング済みの平面座標）。
	//   columns     … 柱・小屋束の命令（支点。上端が材の下端に止まるものだけを使う）
	//   anchorsM12  … M12（座金付き）アンカーボルトの位置（土台の男木の手掛かり）
	//   floorPosts  … 床束の位置（大引の支点）
	// **取り込み設定に依らない**値を渡すこと（アンカーボルト・床束を「取り込まない」に
	// しても継手の向きは変わらない。parse/AnchorBolt の collectAnchorBolts、parse/FloorPost の
	// floorPostPositions）。
	struct SpliceCues
	{
		std::vector<core::ColumnCommand> columns;
		std::vector<core::Vec2> anchorsM12;
		std::vector<core::Vec2> floorPosts;
	};

	// 横架材の member 命令から継手のシンボル配置命令を組み立てる。同じレイヤ・平行・Z 範囲が
	// 重なる 2 本の材の端が一致する箇所ごとに 1 つ置く。並びは組 (i, j)（i < j）の辞書順。
	//
	// 置換するシンボル名は取り込み設定が持つ（core::SymbolRole::Splice。既定は "継手"）。
	// cues / options を省いた呼び出しは手掛かり無し（向きは正規化した材軸）・既定の設定になる
	// （単体テスト用）。
	std::vector<core::SymbolCommand>
	buildSpliceCommands(const std::vector<core::MemberCommand>& members,
						const SpliceCues& cues = SpliceCues{},
						const core::ImportOptions& options = core::ImportOptions{});
} // namespace HomeskzIfcImport::parse
