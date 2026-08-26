//
//	parse/Rafter.h
//
//	Phase 1（IFC 解析）の垂木モジュール。
//
//	【前提】ホームズ君 EX の IFC には**垂木が一切出力されない**（オブジェクト・型・プロパティ
//	のいずれにも垂木の位置や仕様が現れない）。そのため垂木は IFC から抽出できず、
//	要件どおり**屋根版（Name が "屋根版" 始まりの IfcSlab。勾配した平面外形を鉛直に押し出した
//	ソリッド＝押し出しが屋根の厚み）の勾配・外形から導出**する。屋根面が建物形状の一次情報で、
//	部材ではなく面が形を決める（docs/DEV-NOTES.md「実装順序の方針（形状先行）」）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）だけで完結し、
//	通常の C++ ツールチェインでコンパイル・単体テストできる（CLAUDE.md「Phase 1」）。
//
//	導出の要件（docs/DEV-NOTES.md M6）:
//	  * **勾配方向（垂木の流れ方向）**＝屋根面の法線の水平成分（＝最急勾配方向。母屋・
//	    棟木・軒桁に直交する）。垂木は軒側（低い端）から棟側（高い端）へ架かる。
//	  * **配置間隔**＝勾配方向に直交する方向（軒・棟に平行）へ掃引し、各掃引線を屋根面の
//	    外形でクリップした区間を 1 本の垂木にする。**両端の垂木は屋根面の端から垂木幅の
//	    半分だけ内側**（掃引位置＝断面中央なので端に軸を合わせると半幅がはみ出す）。内部は
//	    kRafterInterval（455mm）**以下**で割り付ける（中間は 455mm ちょうど・端数は両端へ等分）。
//	  * **断面**＝IFC に垂木の寸法情報が無いため既定 45×45（要件の決め打ち）。
//	  * **配置先レイヤ**＝屋根版を含むストーリの母屋レイヤの直上に独立させた "n-垂木"。
//	    最上階（屋根）の主屋根だけでなく、中間階に架かる下屋根（下屋）の屋根版も、その階の
//	    "n-垂木" に置く。下屋根は母屋を持たないこともあるため、レイヤの有無・振り分けは
//	    母屋ではなく**屋根版の有無**（storyHasRoofSlab）で判定する（parse/Story が該当階に
//	    "垂木" レベルを作る条件と一致させる）。
//	  * **始点（支持点）・軒の出・差し込み**＝start は屋根面（垂木下面）が横架材天端
//	    （最上階は軒高）の Z レベルと交わる点（＝受ける軒桁の芯線の真上）。差し込み
//	    （embedment）はその軒桁の桁幅の半分で、軒の出（overhang）は支持点→軒先の水平距離
//	    から差し込みを引いた残り（描画側は軒先を 支持点＋差し込み＋軒の出 に置く。
//	    core::rafterEaveEnd）。
//
//	【M7 で精緻化済み】差し込みに使う桁幅は、支持点の真下にある軒桁（parse/Member の
//	横架材命令）の実寸から採る（girderWidthAt）。受ける軒桁が見つからないときだけ既定桁幅
//	kDefaultGirderWidth へフォールバックする（M6 では横架材が未導入で常にこの既定値だった。
//	docs/DEV-NOTES.md M6「依存メモ」/ M7）。
//

#pragma once

#include "core/Document.h"
#include "parse/IfcGeometry.h"
#include "parse/Step.h"

#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 屋根面を表す IfcSlab の Name 接頭辞。屋根版は"屋根版:{連番}" のような名前で出力されるた
	// め、前方一致で判定する。
	inline constexpr const char* kRoofSlabPrefix = "屋根版";

	// 垂木レベル・レイヤの名前。配置先レイヤは "{接頭辞}-垂木"。文字列の定義は
	// core/Document.h（命令セットの語彙）にあり、ここはその再公開（レベル種別名の置き場所は
	// parse/Story.h の kLevelFL ほかと同じ流儀）。
	inline constexpr const char* kLevelTaruki = core::kLevelTaruki;

	// 垂木の既定断面（mm）。IFC に垂木の寸法情報が無いため決め打ち（要件どおり 45×45）。
	inline constexpr double kDefaultRafterWidth = 45.0;
	inline constexpr double kDefaultRafterHeight = 45.0;

	// 垂木の配置間隔（mm）。IFC に情報が無いため決め打ち（要件どおり @455）。
	inline constexpr double kRafterInterval = 455.0;

	// 支持部分の差し込み（embedment）に使う桁幅（mm）。受ける軒桁（横架材命令）から桁幅を
	// 参照できないときのフォールバックで、差し込みはこの半分になる。
	inline constexpr double kDefaultGirderWidth = 105.0;

	// 桁幅の探索許容。
	//   Search … 支持点から軒桁の芯線までの直交距離の上限（mm）
	//   Along  … 芯線に沿う射影位置が区間内かの余裕（mm。角部も拾う）
	//   PerpSin… 垂木となす角の sin の下限。これ未満（＝垂木と平行に走る材）は軒桁でない
	inline constexpr double kGirderSearchTol = 100.0;
	inline constexpr double kGirderAlongTol = 1.0;
	inline constexpr double kGirderPerpSin = 0.1;

	// 要素が屋根版（IfcSlab かつ Name が "屋根版" 始まり）か。**垂木（parse/Rafter）と
	// 野地板（parse/Roof）は同じ屋根面を共有するので、判定はここに一本化する**（かつては
	// 両 .cpp に逐語的な複製があり、片方だけ直せば拾う屋根版がズレる形だった）。
	bool isRoofSlab(const Entity& element);

	// 階（#storeyId）が屋根版を含むか。屋根版を含む階は垂木・野地板レイヤ（"n-垂木" /
	// "n-野地板"）を持つため、parse/Story がレベルを足す条件に使う。垂木・野地板を配置する階
	// と一致させる必要があるので、判定はここに一本化する。
	bool storyHasRoofSlab(const Model& model, int storeyId);

	// 同上。共有コンテキストの要素一覧を使う（parse/Context.h）。
	bool storyHasRoofSlab(Context& context, int storeyId);

	// 垂木の仕様ラベル（"45×45@455"）を返す。断面・間隔が決め打ちなので全垂木で共通。
	std::string rafterLabel();

	// 屋根面の掃引方向の広がり [eMin, eMax] に垂木の掃引位置を割り付ける。
	//   * **両端は屋根面の端から inset（＝垂木幅の半分）だけ内側**（端に軸を合わせると
	//     垂木幅の半分が屋根面から外へはみ出すため。半幅内側なら端の垂木の外面が端に揃う）。
	//   * 内部は interval **以下**に分割する（中間は interval ちょうど・端数＝余りは両端の
	//     2 区間へ等分）。実効幅 W = eMax − eMin − 2·inset を interval 以下に割る最小区間数
	//     n = ceil(W / interval) を採り、中間 n−2 区間を interval、残りを両端へ等分する。
	//   * 半幅を差し引くと広がりが極小（屋根が垂木幅程度に狭い）な面は中央 1 本だけ返す。
	// 両端を半幅内側へ寄せることで掃引線が外形頂点に接して退化せず（走査線法の半開判定が
	// 上端で交点を落とす問題も回避）、確実に区間を得られる。
	std::vector<double> sweepPositions(double eMin, double eMax, double interval, double inset);

	// 支持点 (px, py) の真下にある軒桁（横架材命令）の幅を返す。支持点は屋根面（垂木下面）
	// が横架材天端 Z と交わる点で、受ける軒桁の芯線のほぼ真上に来る。members のうち芯線が支持
	// 点に最も近い（直交距離が kGirderSearchTol 以内・射影が芯線区間内）ものの幅を採り、
	// 見つからなければ kDefaultGirderWidth を返す。座標系は member 命令と同じセンタリング済み。
	// (rdx, rdy) は垂木の方向（支持点→棟）で、垂木と平行に走る材（継ぎ手・側並び）
	// を除いて軒桁（垂木に直交）を選ぶために使う。判定は members の並び順に依存しない決定的な
	// 結果になる。
	double girderWidthAt(double px, double py, double rdx, double rdy,
						 const std::vector<core::MemberCommand>& members);

	// 1 つの屋根面（parse/IfcGeometry の RoofPlane）から垂木命令を組み立てる。
	//   plane           … 屋根面（平面外形頂点列＋上向き単位法線。Z はストーリ相対）
	//   layer           … 配置先デザインレイヤ名（"n-垂木"）
	//   storeyElevation … ストーリ高さ（mm）。天端 Z をこれで絶対値にする
	//   center          … グリッド中心オフセット（通り芯・床と同じセンタリング）
	//   beamTopZ        … 支持点が乗る横架材天端（最上階は軒高）の絶対 Z。std::nullopt なら
	//                     支持点を取らず start＝軒先・overhang=0 にする
	//   storyMembers    … 同じ階の横架材命令（差し込みに使う桁幅の参照先。空なら既定桁幅）
	// **start＝軒側（支持点）・end＝棟側（高い端）**。ほぼ水平な面・広がりが極小の面は空
	// （勾配方向が定まらない）。区間の平面投影長が極小（隅木際の極小片等）のものは配置しない。
	std::vector<core::RafterCommand>
	raftersForPlane(const RoofPlane& plane, const std::string& layer, double storeyElevation,
					const core::Vec2& center, std::optional<double> beamTopZ = std::nullopt,
					const std::vector<core::MemberCommand>& storyMembers = {});

	// STEP Model から垂木の描画命令を組み立てる。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、各階に含まれる
	// 屋根版（Name が "屋根版" 始まりの IfcSlab）から垂木を導出する。配置先レイヤはその階の
	// "n-垂木"、支持点が乗る横架材天端は「Elevation ＋ 横架材天端オフセット」（最上階は
	// 軒高＝Elevation）。屋根面を解決できない屋根版はスキップする（1 面の欠損で全体を
	// 止めない。CLAUDE.md「エラーハンドリング」）。
	//
	// 並びは階（Elevation 昇順）→ 階内は要素の出現順 → 面内は掃引位置順で、エンティティ
	// 列挙順に依存しない決定的な結果になる。
	std::vector<core::RafterCommand> buildRafterCommands(const Model& model);

	// 同上。共有コンテキストのストーリ一覧・センタリング中心・屋根面を使う（parse/Context.h）。
	// 屋根面の解決は野地板（parse/Roof）と共有されるので、屋根版 1 枚あたり 1 回で済む。
	// 桁幅の参照先は Context::members（＝登り梁の補正前）。
	std::vector<core::RafterCommand> buildRafterCommands(Context& context);

	// 同上だが、桁幅の参照先の横架材命令を明示的に渡す。parse/BuildDocument は**登り梁の補正
	// 後**の命令を渡す。
	std::vector<core::RafterCommand>
	buildRafterCommands(Context& context, const std::vector<core::MemberCommand>& members);
} // namespace HomeskzIfcImport::parse
