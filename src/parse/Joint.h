//
//	parse/Joint.h
//
//	Phase 1（IFC 解析）の仕口モジュール（docs/DEV-NOTES.md M11「シンボル置換系」）。**IFC
//	を直接見ない**唯一の解析モジュールで、既に組み立て済みの横架材命令（parse/Member）
//	と柱命令（parse/Column）の平面ジオメトリだけから「受ける材のある横架材端部」を判定し、
//	そこへ仕口シンボルを置く。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。ここは core/Document の
//	命令構造体しか触らないので、STEP グラフにも依存しない純粋なジオメトリ計算になる。
//
//	要件:
//	  * **受ける材のある端部にだけ置く**。横架材の端点が、同じレイヤ・Z 範囲が重なる別の
//	    横架材（受ける材）の footprint（矩形）に載る場合を「受ける材のある端部」とする。
//	    **柱の側面に取り付く端部も同様**で、端点が Z 範囲の重なる柱の断面矩形に入る場合も
//	    仕口を置く。どの横架材・柱にも取り付かない自由端には置かない。
//	  * **基準点は梁端の中央上端**。MemberCommand の start / end は断面の左右中央・上端
//	    （天端中央）が通る線の端点なので、その端点をそのまま基準点にする。
//	  * **回転角は梁端の向き**——梁軸に沿って端部から部材内側へ向かう方向（シンボルが梁と
//	    揃うようにする）。
//	  * **配置先は横架材自身のレイヤ**（横架材天端／最上階は軒高／母屋は母屋／登り梁は登り梁）。
//	  * **高さも梁端の天端に合わせる**（zOffset）。シンボルは既定では配置先レイヤの平面
//	    （＝バインド先レベルの絶対 Z）に載るが、横架材の天端はそのレベルとは限らない——
//	    登り梁は一端が軒桁・他端が棟木で数 m の高低差があり、母屋・棟木・段差梁もレベルから
//	    ずれる。レイヤ平面のままだと**登り梁の仕口が軒高の高さに落ちて**梁から離れてしまう。
//	    そこで各端部の SymbolCommand.zOffset に**その端部のバウンド offset**（レベルの絶対 Z
//	    から天端 Z までの距離。parse/Member が横架材自身の高さバインドに使っているのと同じ値）を
//	    入れ、描画側がレイヤ平面からその分だけ持ち上げる。平らな梁は offset ≈ 0 なので
//	    従来どおりレイヤ平面に載る。
//
//	受ける横架材の判定は、横架材どうしの食い込み調整（parse/Member の
//	resolveMemberInterferences）と同じ方針にそろえる。すなわち平行（同一直線上の継ぎ手・
//	側並びの平行材）は受ける材とみなさず、軸が交差する材だけを対象にする。**ただし登り梁は
//	専用レイヤ（"n-登り梁"）に分離され、端部が軒桁（横架材天端／軒高）・母屋・棟木といった
//	別レイヤの材に取り付く**ため、登り梁を対象材とするときだけ同一レイヤの制約を外す
//	（平行・Z 分離の判定は保つ）。柱は横架材と別レイヤ（span レイヤ）で方向も持たないため、
//	レイヤ一致・平行判定はせず、Z 範囲の重なりと断面矩形への内包だけで判定する。
//
//	判定は命令のジオメトリ（食い込み調整済み）に対して行うので、命令の並び順に依存しない
//	決定的な結果になる。
//

#pragma once

#include "core/Document.h"

#include <cstddef>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// 置換するハイブリッドシンボル名。
	inline constexpr const char* kSymbolJoint = "仕口";

	// 受ける材の判定に使う許容値（mm。横架材の食い込み調整と同じ考え方）。
	inline constexpr double kJointParallelTol = 1e-6; // 平行な相手は受ける材にしない
	inline constexpr double kJointAlongTol = 1.0; // 相手材の軸方向の範囲判定の余裕
	inline constexpr double kJointFaceTol = 1.0; // 相手材の面ちょうどに載る端部の余裕
	inline constexpr double kJointZOverlapTol = 1.0; // これ以下の Z 重なりは取り付きでない
	inline constexpr double kJointMinLength = 1.0; // 平面投影長がこれ未満の材はスキップ

	// 受ける材判定用の横架材ジオメトリ。天端中央線の端点・単位軸・平面投影長・半幅・実体の Z
	// 範囲（[天端下端, 天端上端]。傾斜梁も覆う）。valid=false は平面投影長が極小＝端部・
	// 向きが定まらない材（判定から外す）。
	//
	// **端点は「材が実際に占める端」**（端部オフセットを戻した点。core の memberDrawnStart /
	// memberDrawnEnd）。命令の端点は取り合い相手の芯線上にあり、そこは相手の材の中なので、
	// 仕口を置く位置にも受ける材の footprint にもならない（core/Document.h「端部オフセット」）。
	struct MemberGeom
	{
		bool valid = false;
		core::Vec2 start;
		core::Vec2 end;
		core::Vec2 axis; // 単位軸（start → end）
		double length = 0.0;
		double halfWidth = 0.0;
		double zBottom = 0.0;
		double zTop = 0.0;
	};

	// 受ける柱判定用の柱ジオメトリ。配置中心・断面の半幅／半成・ Z 範囲（[下端, 上端]。
	// 上端は命令の上端（受ける横架材の天端）ではなく**材が実際に止まる高さ**）。
	// 柱は断面矩形を配置座標中心に**軸平行**で持つものとして扱う（ColumnCommand
	// に回転情報が無いため。木造柱は正方形断面が主で回転の影響は小さい）。柱は退化しない（平
	// 面投影長 0 にならない）ので valid フラグを持たない。
	struct ColumnGeom
	{
		core::Vec2 center;
		double halfWidth = 0.0;
		double halfDepth = 0.0;
		double zBottom = 0.0;
		double zTop = 0.0;
	};

	// member 命令から受ける材判定用のジオメトリを作る。
	MemberGeom memberGeom(const core::MemberCommand& command);

	// column 命令から受ける柱判定用のジオメトリを作る。
	ColumnGeom columnGeom(const core::ColumnCommand& command);

	// 端点が相手材の footprint（矩形）に入るか。相手の中心線からの軸方向位置が [0,
	// length]（端＝コーナーの余裕を含む）にあり、直交距離が半幅＋余裕以内（面ちょうどに載る端
	// 部も含む）なら取り付き。
	bool pointInMember(const core::Vec2& point, const MemberGeom& other);

	// 端点が柱の断面矩形（軸平行）に入るか。
	bool pointInColumn(const core::Vec2& point, const ColumnGeom& column);

	// 端点に取り付く受ける材（別の横架材または柱）があるか。index は判定対象の横架材の位置で、
	// geoms / members は同じ並び。
	bool endHasReceiver(std::size_t index, const core::Vec2& point,
						const std::vector<MemberGeom>& geoms,
						const std::vector<core::MemberCommand>& members,
						const std::vector<ColumnGeom>& columnGeoms);

	// 横架材の member 命令（と柱の column 命令）から仕口のシンボル配置命令を組み立てる。
	// 横架材の始端・終端ごとに受ける材の有無を判定し、取り付く端部にだけ仕口を置く。並びは
	// members の並び順→端部（始端・終端）の順で、
	// **判定自体は入力の並びに依存しない**（各端部の可否がジオメトリだけで決まるため）。
	// 高さ（zOffset）はその端部のバウンド offset をそのまま写す（上記「高さも梁端の天端に
	// 合わせる」）。
	std::vector<core::SymbolCommand>
	buildJointCommands(const std::vector<core::MemberCommand>& members,
					   const std::vector<core::ColumnCommand>& columns = {});
} // namespace HomeskzIfcImport::parse
