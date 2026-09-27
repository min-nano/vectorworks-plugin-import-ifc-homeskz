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
//	  * 基礎伏図 … **アンカーボルトの位置・立上りの切れ目（端部）・立上りの通り**。
//	  * 床伏図・小屋伏図（柱梁伏図） … **柱（小屋束を含む）・梁**の位置。
//	  * 母屋伏図 … **母屋**の位置。
//	  * 軸組図 … **柱・束の位置**（横）と、**GL・床高・軒高**とそこからの**横架材天端の高さ**
//	    （縦）、**標準の横架材天端と違う高さの横架材**の高さ。GL・FL・軒高にはレベル記号を置く。
//
//	【伏図の外周の列】図の外形（その伏図が映すものを包む矩形＝core::planContentBounds。
//	通り芯は除く）の外へ、内側から順に
//	  * 部材の位置と通り芯を合わせた列（部材の位置が通り芯とすべて重なるときは出さない）、
//	  * 通り芯の間隔の列、
//	  * 両端の通り芯の間の全長（通り芯が 3 本以上のときだけ。2 本なら 1 つ前の列と同じ）、
//	を段（tier）を 1 つずつ外へずらして並べる。**上と左に**この 3 段、**下と右に**は通り芯の間隔
//	だけを置く（部材の位置は片側で押さえれば足り、反対側は通り芯の読みやすさを優先する）。
//	部材の位置は**直交格子に沿う材**だけから採る——東西に走る梁はその Y を、南北に走る梁は
//	その X を押さえる。斜めの材は押さえない（位置が 1 つの座標で言えない）。
//
//	【基礎伏図は立上りに沿う列も持つ】アンカーボルトと立上りの切れ目は**その立上りに沿って**
//	押さえる（外周の 1 列へ全部を集めると、どの立上りの話か読めない）。同じ直線に乗る立上り
//	（1 本の「通り」）ごとに、そこへ乗るアンカーボルト・立上りの端・その通りを横切る通り芯を
//	測点にした列を作り、立上りの芯から図の外側へ向けて出す。**通り芯と重ならない測点が 1 つも
//	無い通りには作らない**（通り芯の間隔を繰り返すだけになる）。外周の立上りの列が 1 段目
//	（tier 0）を占めるので、基礎伏図の外周の列は 2 段目から始める。
//
//	【軸組図の注釈空間】横＝切断線に沿った距離（断面線の終点からの距離）・縦＝高さ Z。原点
//	合わせは parse/Tag の sectionAnnotationPoint ただ 1 か所に任せる（タグと寸法が同じ投影を
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

	// 伏図 1 枚の外周の列（ヘッダ冒頭「伏図の外周の列」）。elementX / elementY は部材の位置
	// （X を測る列・Y を測る列）、gridX / gridY は通り芯の位置、min / max は図の外形、
	// firstTier は最も内側の段。
	std::vector<core::DimensionChainCommand>
	perimeterDimensionChains(const std::vector<double>& elementX,
							 const std::vector<double>& elementY, const std::vector<double>& gridX,
							 const std::vector<double>& gridY, const core::Vec2& min,
							 const core::Vec2& max, int firstTier);

	// 基礎伏図の立上りに沿う列（ヘッダ冒頭「基礎伏図は立上りに沿う列も持つ」）。center は
	// 図の中心（列をどちらへ出すかを決める＝中心から遠い側＝外側へ出す）。
	std::vector<core::DimensionChainCommand>
	foundationWallDimensionChains(const std::vector<core::WallCommand>& walls,
								  const std::vector<core::SymbolCommand>& anchorBolts,
								  const std::vector<core::GridCommand>& grids,
								  const core::Vec2& center);

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
