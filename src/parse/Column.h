//
//	parse/Column.h
//
//	Phase 1（IFC 解析）の柱モジュール（docs/DEV-NOTES.md M8「柱」）。管柱・通し柱・
//	小屋束——ホームズ君 IFC の IfcColumn をすべてここで解析し、core::ColumnCommand へ変換する。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・ストーリ（parse/Story）・横架材（parse/Member の断面抽出）・
//	構造クラス（parse/StructuralClass）だけで完結する（CLAUDE.md「Phase 1」）。
//
//	解析の要点:
//	  * **柱も横架材と同じ構造材ツール（StructuralMember）で描く**。拡張パッケージの柱・
//	    間柱ツールはスクリプトからの操作に対して不安定なので、柱も標準の構造材ツールで描く。
//	    種別の違いは**構造用途**（structuralUse）で表し、管柱・通し柱＝柱（"4"）、
//	    小屋束＝小屋束（"5"）。小屋束を柱用途で描くと VW が柱の高さモデルを適用して上端高さが
//	    崩れる。
//	  * **span（またぐレベル区間）レイヤ**: 柱は階のレイヤではなく "{from}to{to}-柱" へ置く
//	    （parse/Story の spanLayerName）。from は柱が立つ床レベル（1 始まり・GL=0）、to は
//	    上端が届く床／屋根面レベル（resolveColumnToLevel）。管柱は次階の整数、屋根束
//	    （小屋束・棟束）は屋根面で止まるので +0.5 の半整数、通し柱は複数階ぶん上。伏図が
//	    切断レベルで表示レイヤを絞れるようにするための分け方で、下屋の小屋束（"2to2.5-柱"）が
//	    上階の小屋伏図へ写り込まない。
//	  * **to レベルの境界は上階横架材の「下端」**（天端ではない）。通常の管柱は梁を下から
//	    受ける高さ＝横架材の下端までしか来ないため、天端を境界にするとホームズ君のモデルで
//	    天端付近まで伸びた管柱を通し柱と誤判定する。ただし**到達階の横架材天端（最上階は
//	    軒高）より上端が高い柱**は、床／軒に下から受けられる管柱・通し柱ではなく屋根面で
//	    止まる屋根束なので、reached + 1 + 0.5 の半整数レベルにする。
//	  * **高さ（height）は上端を受け材の天端に取ったパス長**で、実際に描かれる材の高さは
//	    height + endOffset（＝ IFC の押し出し Depth）になる。
//	  * **高さは「鉛直パス」と「上下端バウンドの差」の両方で表す**（両方が要る。下記）。
//	    柱（管柱・通し柱）は下端を当階・上端を上階（storyOffset=1）の横架材天端（最上階は
//	    軒高）へ、小屋束は上下端とも当階の横架材天端へバインドし、offset にはそれぞれ
//	    **実際の下端／上端の絶対 Z までの距離**を入れる（小屋束の上端 offset ＝ 下端 offset
//	    ＋ 柱高さ）。こうすると**どの柱でもバウンドの差＝柱高さ＝パス長**になる。
//
//	    **上端 offset は「下端 ＋ 柱高さ」にする（小屋束も例外にしない）。** VW 2026 の
//	    構造材 PIO では**バウンドの差が高さを支配する**ので、上下端を同値（差 0）にすると
//	    鉛直パスが正しくても高さ 0 で描かれる——M8 のローカル確認で、同値にした小屋束だけが
//	    高さ 0 になり、差が実高さの管柱は正しく描かれた（docs/DEV-NOTES.md M8）。
//	  * **上端は受ける横架材の天端（＝その芯線）に取る**。横架材の芯線は天端中央を通るので、
//	    柱・束の上端を材が実際に止まる高さ（梁の下端）に置くと、その座標はどの部材の芯線にも
//	    乗らず**座標だけを見て接合を言えない**。そこで上端を受け材の天端へ合わせ、梁せいぶんの
//	    戻り（例: 梁せい 150mm なら −150）を端部オフセット（endOffset）へ入れる
//	    （core/Document.h「端部オフセット」）。受け材は、管柱・通し柱では**上の階の横架材天端
//	    のうち上端以上でいちばん近いもの**（beamSeatAbove）、小屋束・最上階の柱では**直上に
//	    乗る母屋・棟木・登り梁**（memberOnTop）。特定できない柱は上端を動かさず、オフセットも
//	    0 のままにする。下端は元から受け材（土台・梁）の天端に乗るので動かさない。
//	  * **小屋束の断面はホームズ君 IFC の値が当てにならない**（適当な値）ので、直上に乗る
//	    横架材（母屋・棟木・登り梁）の断面幅に合わせた正方形へ置き換える（90mm 幅の母屋なら
//	    90mm 角）。上に乗る材が見つからない小屋束は IFC の断面をそのまま使う。
//	  * **柱頭・柱脚金物**（IfcMechanicalFastener）は柱と同じ平面座標に置かれるので XY で
//	    対応付け、型（IfcMechanicalFastenerType）の名前を**加工せずそのまま**仕様として持つ。
//	    構造材ツールに金物専用フィールドが無いため、構造材 ID（memberId）にも連結する。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 構造材ツールの構造用途（StructuralUse）値。VW の構造用途プルダウンの並び順に対応する
	// （<自動>, 梁="1", 桁, 根太, 柱="4", 小屋束="5", …）。
	//
	// **実体は core/Document.h**（記号 PIO も同じ値で柱／小屋束を見分けるため、両フェーズが
	// 見てよい唯一の置き場に定義がある）。ここは解析側の読みやすい別名で、レベル種別名
	// （kLevelFL 等）を parse/Story.h が再公開しているのと同じ形。
	inline constexpr const char* kStructuralUseColumn = core::kStructuralUseColumn;
	inline constexpr const char* kStructuralUseKoyazuka = core::kStructuralUseKoyazuka;

	// IfcColumn.ObjectType から得る柱種別名（構造材 ID の表記に使う）。ホームズ君 IFC の
	// ObjectType は未設定（管柱）または "STANDCOLUMN"（小屋束）で、未知の値は既定＝管柱。
	inline constexpr const char* kColumnTypeDefault = "管柱";
	inline constexpr const char* kColumnTypeKoyazuka = "小屋束";

	// 柱頭・柱脚金物（IfcMechanicalFastener）を識別する Name のキーワード。
	inline constexpr const char* kHardwareTopKeyword = "柱頭金物";
	inline constexpr const char* kHardwareBottomKeyword = "柱脚金物";

	// 柱の上端が上階の床（次階 FL）をこれだけ超えていれば通し柱とみなす（mm）。管柱の上端は次
	// 階の横架材天端＝次階 FL より梁背ぶん下で止まるため、次階 FL をわずかに超えただけで通し
	// 柱と判定してよい。
	inline constexpr double kThroughColumnTol = 100.0;

	// span の to レベル判定の許容値（mm）。柱上端が上階の横架材下端に達したかの判定に使う。
	inline constexpr double kSpanLevelTol = 1.0;

	// 柱の上端を「受けている横架材の天端」へ合わせるときの上限（mm）。柱上端から上の横架材
	// 天端までの距離＝その梁のせいなので、木造の梁せいとしてありえない距離しか無い柱は
	// 受け材を特定できなかったとみなして上端を動かさない（beamSeatAbove）。
	inline constexpr double kColumnSeatTol = 450.0;

	// 小屋束の断面幅を「上に乗る横架材」へ合わせるときの許容値（mm）。小屋束は材の直下に立つ
	// ため直交距離はほぼ 0 だが、モデリング誤差・傾斜梁の天端中央線ずれを吸収する。
	inline constexpr double kKoyazukaMatchPerpTol = 40.0; // 平面の直交距離（半幅への上乗せ）
	inline constexpr double kKoyazukaMatchAlongTol = 50.0; // 材の軸方向の範囲（棟束は端に立つ）
	inline constexpr double kKoyazukaMatchZTol = 30.0; // 小屋束上端が材の Z 範囲に収まる余裕

	// 要素が柱（IfcColumn）か。**ストーリの横架材天端オフセット（parse/Story）と解析本体で
	// 同じ述語を使う**ため、判定はここに一本化する。
	bool isColumnElement(const Entity& element);

	// IfcColumn.ObjectType を柱種別名へ変換する。未設定（空文字）・未知の値は既定種別（管柱）
	// として扱う。
	std::string resolveColumnType(const std::string& objectType);

	// 金物（IfcMechanicalFastener）の型（IfcMechanicalFastenerType）の名前を返す。型は
	// IfcRelDefinesByType の逆参照から辿る。型が付いていない／名前が無ければ空文字。
	// referrers は #id 昇順なので、複数あっても常に同じものを選ぶ（決定的）。
	//
	// **ここに 1 つだけ置く**: 柱頭・柱脚金物（本モジュール）とアンカーボルト
	// （parse/AnchorBolt）はどちらも IfcMechanicalFastener で、型名から仕様／シンボルを決める。
	std::string fastenerTypeName(const Model& model, const Entity& fastener);

	// 金物の型名を仕様文字列として返す。**加工しない**:ホームズ君側で金物定義をカスタマイズし
	// ていると型名が想定の "柱頭金物:(ろ)" 形式とは限らず、コロン分割等の加工で文字列が失われ
	// る（空欄になる）ため、型名全体を登録する。
	std::string columnHardwareSpec(const std::string& typeName);

	// 柱の構造材 ID を組み立てる。"{幅}×{成} - {種別}" を基本とし、柱頭・柱脚金物の仕様（空で
	// ないもの）を " / " 区切りで連結する。構造材ツールに金物専用フィールドが無いため、
	// 金物仕様は ID に含めて保持する。例: makeColumnMemberId(105, 105, "管柱", "柱頭金物:(ろ)
	// ", "柱脚金物:(い)")→ "105×105 - 管柱 / 柱頭金物:(ろ) / 柱脚金物:(い)"
	std::string makeColumnMemberId(double width, double depth, const std::string& columnType,
								   const std::string& topHardware,
								   const std::string& bottomHardware);

	// 要素のローカル配置から 2D 配置座標を取り出す。ホームズ君 IFC ではストーリの XY 原点が
	// (0, 0) なので、ローカル配置 Location の XY をそのまま平面座標として扱える（横架材と同じ
	// 座標系・同じセンタリングで補正できる）。取得できなければ false（親 PlacementRelTo
	// は辿らない。M2 と同じ規約）。
	bool columnPosition2D(const Model& model, const Entity& element, core::Vec2& out);

	// 小屋束の直上に乗る横架材（母屋・棟木・登り梁）1 本。
	//   width          … 断面幅（小屋束の断面をこれに合わせた正方形へ置き換える）
	//   topElevation   … 小屋束の位置におけるその材の**天端 Z**（＝その材の芯線が通る高さ）。
	//                    小屋束の上端をここへ合わせ、材のせいぶんを端部オフセットへ入れる。
	struct MemberOnTop
	{
		double width = 0.0;
		double topElevation = 0.0;
	};

	// 小屋束の直上に乗る横架材（母屋・棟木・登り梁）を返す。小屋束の平面位置 (px,
	// py)（センタリング済み）と上端の絶対 Z topAbs を横架材命令と突き合わせ、①平面 footprint
	// が小屋束を覆い（中心線からの直交距離が半幅＋許容以内）、②小屋束上端がその材の Z
	// 範囲（下端〜天端）に収まる材を探す。②は材が小屋束に乗る（材下端 ≈ 小屋束上端）場合も、
	// 小屋束が材を貫いて天端付近まで伸びる（棟束）場合も拾う。最も小屋束上端に近い材を返
	// し、見つからなければ nullopt。傾斜梁（登り梁）は天端 Z が軸方向に変化するため小屋束位置
	// で補間する。判定は members の並び順に依存しない決定的な結果になる。
	std::optional<MemberOnTop> memberOnTop(double px, double py, double topAbs,
										   const std::vector<core::MemberCommand>& members);

	// 柱の上端が受けている横架材の天端（＝その材の芯線が通る高さ）の絶対 Z を返す。柱を立てて
	// いる階 baseIndex より上の階の横架材天端（beamTops。最上階は軒高）のうち、**柱の上端
	// topAbs 以上でいちばん近いもの**を採る。管柱なら次階の横架材天端そのもの、通し柱なら
	// 到達した階の横架材天端になる。
	//
	// 差が kColumnSeatTol を超えるもの（＝梁せいとしてありえない距離）しか無ければ「受けて
	// いる材が分からない」とみなし、topAbs をそのまま返す（上端を動かさない）。
	double beamSeatAbove(double topAbs, const std::vector<double>& beamTops, std::size_t baseIndex);

	// 柱の上端が上階の床（次階 FL）を貫いていれば通し柱（true）とみなす。上階が無い（最上階）
	// ときは常に false。
	bool isThroughColumn(double topAbs, const std::optional<double>& nextFloorElevation);

	// 柱上端が届く span の to レベル（1 始まり）を求める。
	//
	// baseIndex は 0 起点のストーリ番号で、柱の下端はその階の床（span では baseIndex + 1）に
	// ある。上端 topAbs を base より上の各階の横架材（床梁）の**下端** beamBottoms と比べ、
	// 達した最上の階 reached を求める:
	//   * どの上階の横架材にも達しない（＝上端が直上階の梁下端未満） → その階には載らない
	//     屋根束扱いで from + 0.5（下屋の小屋束・棟束・主屋根の小屋束等）。
	//   * 直上階の床梁下端に達すれば from + 1（管柱）。さらに上の階の床梁下端まで達すれば
	//     通し柱として到達した階まで伸ばす（1・2 階通し柱なら 3 階床＝from + 2）。
	// ただし**到達階の横架材天端（最上階は軒高）beamTops より上端が高い柱**は屋根束なので
	// reached + 1 + 0.5 にする（ヘッダ冒頭「to レベルの境界」参照）。
	double resolveColumnToLevel(int baseIndex, double topAbs,
								const std::vector<double>& beamBottoms,
								const std::vector<double>& beamTops);

	// column 命令から実在する span 柱レイヤを列挙した 1 件。
	struct ColumnSpan
	{
		double from = 0.0;
		double to = 0.0;
		std::string layer;
	};

	// column 命令から実在する span 柱レイヤを (from, to) 昇順で列挙する。重複は除く。
	// 伏図（M13）が切断レベルで表示レイヤを絞るのに使う。
	std::vector<ColumnSpan> collectColumnSpans(const std::vector<core::ColumnCommand>& columns);

	// span 柱レイヤを base ストーリ（0 起点 index ＝ from − 1）ごとにまとめる。各ストーリの
	// レイヤは (from, to) 昇順。parse/Story が各ストーリへ span レベルを作るのに使う。
	std::map<int, std::vector<std::string>>
	collectColumnLayersByStory(const std::vector<core::ColumnCommand>& columns);

	// STEP Model から柱の描画命令を組み立てる。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、各階に含まれる
	// IfcColumn を解析する。配置先は span レイヤ（"{from}to{to}-柱"）で、座標は通り芯と同じ
	// グリッド中心オフセットで補正する。配置・断面を解決できない柱はスキップする（1 本の
	// 欠損で全体を止めない。CLAUDE.md「エラーハンドリング」）。
	//
	// members は to レベル判定（上階の横架材下端）と小屋束の断面合わせに使う横架材命令
	// （**登り梁の補正前**。補正はレイヤ・幅を変えないので結果は同じ）。省略した
	// オーバーロードは内部で組み立てる。
	//
	// 並びは階（Elevation 昇順）→ 階内は要素の出現順（#id 昇順の rel 由来）で、エンティティ
	// 列挙順に依存しない決定的な結果になる。
	std::vector<core::ColumnCommand> buildColumnCommands(const Model& model);
	std::vector<core::ColumnCommand>
	buildColumnCommands(const Model& model, const std::vector<core::MemberCommand>& members);

	// 同上。共有コンテキストのストーリ一覧・センタリング中心・階の要素・横架材命令を使う
	// （parse/Context.h）。**Context 自身がこの結果をキャッシュする**（Context::columns）ので、
	// ストーリ（span レベル）・Document の columns・登り梁の端部詰めが同じ 1 回の解析結果を
	// 共有する。
	std::vector<core::ColumnCommand> buildColumnCommands(Context& context);
	std::vector<core::ColumnCommand>
	buildColumnCommands(Context& context, const std::vector<core::MemberCommand>& members);
} // namespace HomeskzIfcImport::parse
