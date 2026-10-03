//
//	parse/Dimension.h
//
//	Phase 1（IFC 解析）の寸法モジュール（docs/DEV-NOTES.md M31）。伏図（基礎伏図・床伏図・
//	小屋伏図・母屋伏図）と軸組図へ自動で入れる**連続寸法の列**（core::DimensionChainCommand）と、
//	軸組図の**レベル記号**（core::LevelMarkCommand。GL・FL・軒高）を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	【IFC は見ない】寸法は**組み立て済みの命令セット**（柱・横架材・立上り・アンカーボルト・
//	通り芯・ストーリ）だけから決まる（parse/Tag・parse/Joint と同じ）。したがってシート・軸組図
//	が確定した後、parse/BuildDocument の最後に attachDimensionCommands を呼ぶ。
//
//	【何を押さえるか】（利用者の要望。過去の物件の図面の押さえ方に倣う）
//	  * 基礎伏図 … **アンカーボルトの位置・立上りの切れ目（端部）・取り合う立上りの芯**。
//	  * 床伏図・小屋伏図（柱梁伏図） … **柱（小屋束を含む）・梁**の位置（下記「床伏図・
//	    小屋伏図も横架材に沿って押さえる」）。
//	  * 母屋伏図 … **母屋・登り梁**の芯と、斜めの登り梁との**交点**（下記「母屋伏図」）。
//	  * 軸組図 … **柱・束の位置**（横）と、**GL・床高・軒高**とそこからの**横架材天端の高さ**
//	    （縦）、**標準の横架材天端と違う高さの横架材**の高さ。GL・FL・軒高にはレベル記号を置く。
//
//	【基礎伏図は立上りに沿って押さえる】アンカーボルトと立上りの切れ目は**その立上りに沿って**
//	押さえる（外周の 1 列へ全部を集めると、どの立上りの話か読めない）。同じ直線に乗る立上り
//	（1 本の「通り」）ごとに、そこへ乗るアンカーボルト・立上りの自由端・**取り合う直交する
//	立上りの芯**を測点にした列を作り、立上りの芯から図の外側へ向けて出す（1 段目＝tier 0）。
//	決めごと（利用者の指定）:
//	  * **隅や T 字で取り合う端は、取り合う立上りの芯で押さえる**（立上りの外面から測らない）。
//	    何とも取り合わない端だけは端そのもの（面）で押さえる。
//	  * **取り合う立上りの無い通り芯は測点にしない**（現場に通り芯の墨は無く、どこが 2 通り
//	    なのかは距離でしか分からない）。通り芯は測点と重なるときに値を貸すだけ。
//	  * **離れた立上りの間はまたがない。** 同じ直線に乗っても、途切れが開口でなければ列を
//	    割る（何も無い区間・部屋の外を測っても意味が無い）。開口＝両側が自由端で終わり、
//	    中を直交する立上りが横切らない途切れ（玄関・人通口など）で、その幅は押さえる。
//	    直交する立上りと 1 つも取り合わない一続きは、位置が決まらないので近いほうの隣とつなぐ。
//	  * **図の外側に面する立上りの列は、図の外に並べる。** 外向きの側に芯の範囲が重なる
//	    同じ向きの立上りが無い一続き（段違いの外周——下の y0・y2・y1 のように）は、図の
//	    外形（min / max）を根元にして外周の 1 段目に並べる。それ以外は立上りの芯から、
//	    図の中心から遠い側へ出す。
//	  * **外周の 2 段目より外は、外周の立上りに乗るものだけ**。四辺とも 2 段目は外側に面する
//	    立上りの「芯の列」（アンカーボルトを除いたもの。1 段目と同じなら出さない）、上と右は
//	    その外に全長（立上りの芯の端から端。内側の列と同じなら出さない）。内部の立上りの
//	    位置は、それが取り合う立上りに沿う列が押さえる（外周の寸法線に芯が乗らないものを
//	    外周へ集めない）。
//	  * **外側の列にある寸法は内部の列に重ねない。** 立上りの芯・端どうしの寸法（アンカー
//	    ボルトが絡まないもの）が外側の列に既にあれば、内部の列からその区間を抜く（外側を
//	    優先。内部の列どうしも外側＝図の中心から遠いほうを優先）。
//	  * **内部の立上りは、芯・端の列とアンカーボルトの列を分ける**（現場では立上りの位置が
//	    決まってからアンカーボルトを置くので、立上りの寸法だけを追えるように）。アンカー
//	    ボルトが乗るものは芯・端だけの列を 2 段目に、アンカーボルトの絡む寸法を 1 段目に
//	    出す（同じ寸法は 2 段目にだけ）。乗らないものは芯・端の列が 1 段目。
//	  * **半島状・独立した立上りは長さも押さえる**（自由端で終わる一続き。芯・端の列の
//	    1 つ外の段。芯・端の列と同じなら出さない）。
//
//	【床伏図・小屋伏図も横架材に沿って押さえる】（利用者の指定。基礎伏図と同様に）立上りを
//	**直交格子に沿う横架材**に、アンカーボルトを**柱（小屋束を含む）**に置き換えて、上の
//	決めごとをそのまま使う（通り・取り合い・外側に面する列・重ねない・開口・半島）。横架材の
//	端点は取り付く相手の芯の上にある（core::MemberCommand）ので、取り合いは芯で押さえられる。
//	違うのは柱の扱いと外周の並べ方:
//	  * **外周に出すのは、外周の横架材に乗る柱と、外周の横架材に取り合う直交する梁だけ。**
//	    それ以外の柱・梁の位置は内部の横架材に沿う列が押さえる。
//	  * **柱と梁の芯が一致しない通りは、柱と横架材を別の列で押さえる。** 柱の列（柱の
//	    位置と通りの両端）を 1 段外に、横架材の列（取り合う梁の芯と端）を 1 段目に出す
//	    （い通りなら柱間 910 / 1365 …と、大引の 650・5 通りまでの 2990）。柱の列に同じ寸法が
//	    ある区間は横架材の列から抜く。柱が全部取り合いの上にある通り・取り合いが全部柱の
//	    上にある通り・柱の無い通り（大引など）は 1 列。
//	  * **横架材の列は、既に書いた列と同じ寸法を重ねない。** 外周の柱の列を四辺とも先に
//	    覚え、外周・内部の横架材の列からそれと同じ区間（同じ 2 点の間）を抜く。内部の柱の
//	    列も、外側の列にある芯・端どうしの寸法は抜く。
//	  * **外周の柱の列は辺ごとに 1 本につなぐ**（段違いの外周で途切れるのは不自然）。全長を
//	    持つ上と右は、全長の端まで延ばす（外側に面する横架材の無い区間も押さえる）。外周の
//	    2 段目より外は上と右の全長だけ（基礎伏図の「芯の列」は無い）。
//	  * 斜めの材は通りを作らない（押さえない）。
//
//	【母屋伏図も材の通りに沿って押さえる】（利用者の指定。「基本は他の伏図と同様」）母屋・
//	登り梁と、**同じ階の軒桁**（登り梁が取り付く相手。母屋伏図には薄く重ねるだけで、表示
//	レイヤではない＝ViewportCommand::grayedLayers の材）を床伏図・小屋伏図と同じ決めごとで
//	押さえる。
//	**材の芯も交点も無い通り芯は押さえない**
//	（取り合いの無い通り芯は値を貸すだけ。従来は外周の列に全部の通り芯を並べていた）。違うのは:
//	  * **斜めの登り梁は芯の交点を押さえる。** 登り梁の芯と、直交格子に沿う材の芯の交点
//	    （芯を延ばして交わる点）を、取り合う梁の芯と同じくその通りの列の測点にする（1 通りの
//	    棟木に取り付く登り梁の位置＝又ろ・又へ など）。**材の端（挿入点）は押さえない**
//	    （利用者の指定）。登り梁の幅の中で終わる通りの材の端は交点へ押さえ直し、自由端とは
//	    しない。芯を延ばすのは相手の芯に届くまで（相手の幅の半分を斜めに横切る長さ）だけで、
//	    浅い角度で遠くを通る材とは交わったことにしない。**交点は立体で見る**——その点で上下の
//	    範囲が重なる（接する）ときだけ交わったとする（平面で重なって見えても離れている材を
//	    拾わない。kDimensionZTol）。
//	  * **母屋・登り梁は受け材の芯で押さえる。** 母屋・登り梁を受ける（横切って支える）直交
//	    する材の芯を、母屋伏図に映らない別のレイヤの材も含めて（立体で接するものだけ）測点に
//	    する。芯で 2 点以上押さえられる通りの材の端（挿入点・跳ね出しの先）は押さえない
//	    （利用者の指定: い通りの登り梁は 5 通り〜又 7 通り）。
//	  * **四辺の列を全長の端まで延ばす**（下と左も。1 通りの棟木の列に、い通り〜棟木の端・
//	    棟木の端〜り通りを出す。利用者の指定）。
//	  * **ほかの材と取り合わない通りの芯は外周へ出す。** 直交する材と 1 つも取り合わず交点も
//	    無い通り（下屋の い通り・又ち通りの材のように）は、その通りに沿う列では位置が決まら
//	    ないので、芯を外周の列（南北の材の X は上・東西の材の Y は左）に足し、全長（上と右）も
//	    そこまで延ばす。
//
//	【軸組図の注釈空間】横＝切断線に沿った距離（断面線の終点からの距離）・縦＝高さ Z。原点
//	合わせは core::sectionAnnotationPoint ただ 1 か所に任せる（タグと寸法が同じ投影を
//	通る。どちらかだけがずれることが無い）。切断面に乗る材の判定も parse/Tag の
//	memberOnCutPlane / columnOnCutPlane を通す。
//

#pragma once

#include "core/Document.h"

#include <vector>

namespace HomeskzIfcImport::parse
{
	// 測点をまとめる許容（mm）。これより近い 2 点は同じ点として扱う（長さ 0 に近い寸法を
	// 作らない）。通り芯と部材の芯のずれは IFC の丸め程度なので小さくてよい。
	inline constexpr double kDimensionMergeTol = 1.0;

	// 材・立上り・通り芯が直交格子に沿うとみなす許容（mm。始点と終点の、直交する座標の差）。
	inline constexpr double kDimensionAxisTol = 1.0;

	// 母屋伏図で 2 つの材が立体的に取り合うとみなす、上下の範囲の隙間の許容（mm）。登り梁は
	// 屋根面へ合わせ直してある（parse/Noboribari）ので、受け材の天端とちょうどは接しない。
	inline constexpr double kDimensionZTol = 10.0;

	// 軸組図のレベル記号の表示名。FL は "{n}FL"（n は 1 始まりの階）。
	inline constexpr const char* kLevelMarkGL = "GL";
	inline constexpr const char* kLevelMarkFLSuffix = "FL";
	inline constexpr const char* kLevelMarkEaves = "軒高";

	// 値を昇順に並べ、kDimensionMergeTol 以内で隣り合うものを 1 つにまとめる（まとめた群の
	// 最初＝最小の値を残す。列挙順に依らない）。
	std::vector<double> mergeStops(std::vector<double> values);

	// primary（通り芯など、残したい側）に、secondary のうち primary のどれとも
	// kDimensionMergeTol より離れた値だけを足して mergeStops する。**部材が通り芯の上に
	// あるときは通り芯の値を残す**（寸法の数字が通り芯の間隔ちょうどになる）。
	std::vector<double> unionStops(const std::vector<double>& primary,
								   const std::vector<double>& secondary);

	// 通り芯の位置（センタリング済みの平面座標）。axis=Horizontal なら南北に走る通り芯の X、
	// Vertical なら東西に走る通り芯の Y を mergeStops して返す。
	std::vector<double> gridStops(const std::vector<core::GridCommand>& grids,
								  core::DimensionAxis axis);

	// 基礎伏図 1 枚ぶんの寸法の列（ヘッダ冒頭「基礎伏図は立上りに沿って押さえる」）。
	// min / max は図の外形（外側に面する列・外周の列の補助線の根元。列を出す向きは
	// 図の中心から遠い側）。外周の列（2 段目より外。上→左→下→右）→ 立上りに沿う列
	// （1 段目）の順。
	std::vector<core::DimensionChainCommand>
	foundationDimensionChains(const std::vector<core::WallCommand>& walls,
							  const std::vector<core::SymbolCommand>& anchorBolts,
							  const std::vector<core::GridCommand>& grids, const core::Vec2& min,
							  const core::Vec2& max);

	// 床伏図・小屋伏図 1 枚ぶんの寸法の列（ヘッダ冒頭「床伏図・小屋伏図も横架材に沿って
	// 押さえる」）。members / columns はその伏図に映る横架材と柱（小屋束を含む）。min / max・
	// 列の並びは foundationDimensionChains と同じ。
	std::vector<core::DimensionChainCommand>
	framingDimensionChains(const std::vector<core::MemberCommand>& members,
						   const std::vector<core::ColumnCommand>& columns,
						   const std::vector<core::GridCommand>& grids, const core::Vec2& min,
						   const core::Vec2& max);

	// 母屋伏図 1 枚ぶんの寸法の列（ヘッダ冒頭「母屋伏図も材の通りに沿って押さえる」）。
	// members はその伏図に映る母屋・登り梁、eavesGirders は同じ階の軒桁、receivers は
	// 母屋・登り梁を受ける材を探す先（文書の全横架材）、columns は柱。min / max・列の並びは
	// framingDimensionChains と同じ。
	std::vector<core::DimensionChainCommand>
	moyaDimensionChains(const std::vector<core::MemberCommand>& members,
						const std::vector<core::MemberCommand>& eavesGirders,
						const std::vector<core::MemberCommand>& receivers,
						const std::vector<core::ColumnCommand>& columns,
						const std::vector<core::GridCommand>& grids, const core::Vec2& min,
						const core::Vec2& max);

	// 伏図 1 枚ぶんの寸法の列。sheet.kind で押さえるものを選ぶ（ヘッダ冒頭「何を押さえるか」）。
	// 映すものが 1 つも無い（外形が決まらない）伏図には空を返す。
	std::vector<core::DimensionChainCommand>
	buildPlanDimensionCommands(const core::Document& document, const core::SheetCommand& sheet);

	// 軸組図 1 枚ぶんの寸法の列。切断面に柱も横架材も無ければ空。
	std::vector<core::DimensionChainCommand>
	buildSectionDimensionCommands(const core::Document& document,
								  const core::SectionCommand& section);

	// 軸組図 1 枚ぶんのレベル記号（GL＝基礎があるときだけ・各階の FL・軒高）。高さの昇順。
	// 切断面に柱も横架材も無ければ空（記号を置く横の位置が決まらない）。
	std::vector<core::LevelMarkCommand> buildSectionLevelMarks(const core::Document& document,
															   const core::SectionCommand& section);

	// 文書中の全ビューポート（伏図・軸組図）へ寸法の列とレベル記号を割り当てる。
	// **sheets / sections が確定した後**に呼ぶ（parse/BuildDocument の最後。寸法を入れない
	// 設定のときは呼ばない）。
	void attachDimensionCommands(core::Document& document);
} // namespace HomeskzIfcImport::parse
