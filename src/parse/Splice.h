//
//	parse/Splice.h
//
//	Phase 1（IFC 解析）の継手モジュール（docs/DEV-NOTES.md「横架材の継手（M33）」）。仕口
//	（parse/Joint）と同じく**IFC を直接見ない**解析モジュールで、組み立て済みの横架材命令
//	（parse/Member）の平面ジオメトリだけから「同一直線上で材端どうしが突き付く箇所」を
//	判定し、そこへ継手シンボルを置く。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。core/Document の命令構造体
//	しか触らないので、STEP グラフにも依存しない純粋なジオメトリ計算になる。
//
//	【ホームズ君 IFC での継手の現れ方】継手は IFC に要素として出ない。横架材が**継手の位置で
//	2 本に分かれて**出力され、2 本は同一直線上で**材の端（端部オフセットを戻した点）が
//	ぴったり一致する**（実フィクスチャで隙間 0.0mm）。これに対し、直交する材が間を
//	通り抜けている箇所（両側が仕口）は、両側の材端がその材の面で止まるので**相手の幅
//	（105mm）ぶん離れる**——命令の端点（芯線の交点）は一致していても、材の端は一致しない。
//	したがって**材の端どうしの距離**で見れば、継手と「直交材を挟んだ突き付け」を取り違えない
//	（仕口の MemberGeom と同じく core の memberDrawnStart / memberDrawnEnd を使う）。
//
//	要件:
//	  * **1 箇所に 1 つ**置く（仕口は材端ごとに置くが、継手は 2 本の材が共有する 1 点）。
//	  * 継手とみなすのは、**同じレイヤ**・**平面上で平行**・**Z 範囲が重なる** 2 本の横架材
//	    の、**材の端どうしが許容値以内で一致し**、かつ**互いに反対側へ伸びる**（重なって
//	    いない）組だけ。
//	  * **基準点は材の端の一致点**（天端中央。2 本の端の中点）。
//	  * **回転角は材軸の向き**。継手は 2 本が共有するので「どちらの材の内側か」が決まらない
//	    ——並び順に依存させないよう、**向きを (−90°, 90°] に正規化**する（X 方向の材は
//	    +X、Y 方向の材は +Y を向く）。
//	  * **配置先は横架材自身のレイヤ**（仕口と同じ）。
//	  * **高さも梁の天端に合わせる**（zOffset）。2 本の端部のバウンド offset の大きい方
//	    （天端の高い方）をとる。天端揃えの材なら両者は等しい（仕口と同じ考え方。
//	    parse/Joint.h「高さも梁端の天端に合わせる」）。
//
//	判定は命令のジオメトリだけで決まり、組 (i, j) を i < j の順に並べるので、並びは members の
//	並び順に従い、**各箇所の可否は並びに依存しない**。
//

#pragma once

#include "core/Document.h"
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

	// 横架材の member 命令から継手のシンボル配置命令を組み立てる。同じレイヤ・平行・Z 範囲が
	// 重なる 2 本の材の端が一致する箇所ごとに 1 つ置く。並びは組 (i, j)（i < j）の辞書順。
	//
	// 置換するシンボル名は取り込み設定が持つ（core::SymbolRole::Splice。既定は "継手"）。
	// options を省いた呼び出しは既定の設定になる（単体テスト用）。
	std::vector<core::SymbolCommand>
	buildSpliceCommands(const std::vector<core::MemberCommand>& members,
						const core::ImportOptions& options = core::ImportOptions{});
} // namespace HomeskzIfcImport::parse
