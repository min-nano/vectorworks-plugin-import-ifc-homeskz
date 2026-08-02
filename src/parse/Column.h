//
//	parse/Column.h
//
//	Phase 1（IFC 解析）の柱モジュール。Python 版 ifc/column.py に対応する
//	（ROADMAP.md M8「柱」）。管柱・通し柱・小屋束——ホームズ君 IFC の IfcColumn を
//	すべてここで解析し、core::ColumnCommand へ変換する。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・ストーリ（parse/Story）・横架材（parse/Member の断面抽出）・
//	構造クラス（parse/StructuralClass）だけで完結する（CLAUDE.md「Phase 1」）。
//
//	解析の要点（Python 版 CLAUDE.md「柱」節）:
//	  * **柱も横架材と同じ構造材ツール（StructuralMember）で描く**。拡張パッケージの
//	    柱・間柱ツールはスクリプト操作に対して不安定なため、Python 版が標準の構造材ツールへ
//	    置き換えた判断をそのまま引き継ぐ。種別の違いは**構造用途**（structuralUse）で表し、
//	    管柱・通し柱＝柱（"4"）、小屋束＝小屋束（"5"）。小屋束を柱用途で描くと VW が柱の
//	    高さモデルを適用して上端高さが崩れる。
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
//	  * **高さは上下端のバウンドだけで決まる**。柱（管柱・通し柱）は下端を当階・上端を
//	    上階（storyOffset=1）の横架材天端（最上階は軒高）へ、小屋束は上下端とも当階の
//	    横架材天端へバインドし、offset にはそれぞれ**実際の下端／上端の絶対 Z までの距離**を
//	    入れる（小屋束の上端 offset ＝ 下端 offset ＋ 柱高さ）。
//
//	    ［Python 版との差異・意図的］Python 版は小屋束の上端 offset を下端と**同値**にする
//	    （#116）。VS のパス（鉛直な NURBS 曲線）が柱高さを持ち、offset 差を足すと二重に
//	    なるからである。**ISDK ではパスが柱高さを持てない**——構造材 PIO はパスを平面
//	    （2D）としてしか読まず、鉛直パスは平面へ落とすと 1 点に潰れて長さも Z も渡らない
//	    （draw/Column.cpp 冒頭。ローカル確認で OIP が スパン 0／長さ 0／高さ 0 になった）。
//	    したがって二重加算は起こり得ず、同値にすると高さ 0 の柱になる。
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

	// 構造材ツールの構造用途（StructuralUse）値。VW の構造用途プルダウンの並び順に
	// 対応する（<自動>, 梁="1", 桁, 根太, 柱="4", 小屋束="5", …）。Python 版
	// STRUCTURAL_USE_COLUMN / STRUCTURAL_USE_KOYAZUKA と同値。
	inline constexpr const char* kStructuralUseColumn = "4";   // 柱（管柱・通し柱）
	inline constexpr const char* kStructuralUseKoyazuka = "5"; // 小屋束

	// IfcColumn.ObjectType から得る柱種別名（構造材 ID の表記に使う）。ホームズ君 IFC の
	// ObjectType は未設定（管柱）または "STANDCOLUMN"（小屋束）で、未知の値は既定＝管柱。
	inline constexpr const char* kColumnTypeDefault = "管柱";
	inline constexpr const char* kColumnTypeKoyazuka = "小屋束";

	// 柱頭・柱脚金物（IfcMechanicalFastener）を識別する Name のキーワード。
	inline constexpr const char* kHardwareTopKeyword = "柱頭金物";
	inline constexpr const char* kHardwareBottomKeyword = "柱脚金物";

	// 柱の上端が上階の床（次階 FL）をこれだけ超えていれば通し柱とみなす（mm。Python 版
	// THROUGH_COLUMN_TOL）。管柱の上端は次階の横架材天端＝次階 FL より梁背ぶん下で止まる
	// ため、次階 FL をわずかに超えただけで通し柱と判定してよい。
	inline constexpr double kThroughColumnTol = 100.0;

	// span の to レベル判定の許容値（mm。Python 版 SPAN_LEVEL_TOL）。柱上端が上階の横架材
	// 下端に達したかの判定に使う。
	inline constexpr double kSpanLevelTol = 1.0;

	// 小屋束の断面幅を「上に乗る横架材」へ合わせるときの許容値（mm。Python 版
	// _KOYAZUKA_MATCH_*）。小屋束は材の直下に立つため直交距離はほぼ 0 だが、モデリング
	// 誤差・傾斜梁の天端中央線ずれを吸収する。
	inline constexpr double kKoyazukaMatchPerpTol = 40.0; // 平面の直交距離（半幅への上乗せ）
	inline constexpr double kKoyazukaMatchAlongTol = 50.0; // 材の軸方向の範囲（棟束は端に立つ）
	inline constexpr double kKoyazukaMatchZTol = 30.0; // 小屋束上端が材の Z 範囲に収まる余裕

	// 要素が柱（IfcColumn）か。**ストーリの横架材天端オフセット（parse/Story）と解析本体で
	// 同じ述語を使う**ため、判定はここに一本化する。
	bool isColumnElement(const Entity& element);

	// IfcColumn.ObjectType を柱種別名へ変換する（Python 版 resolve_column_type）。
	// 未設定（空文字）・未知の値は既定種別（管柱）として扱う。
	std::string resolveColumnType(const std::string& objectType);

	// 金物の型名を仕様文字列として返す（Python 版 _hardware_spec）。**加工しない**:
	// ホームズ君側で金物定義をカスタマイズしていると型名が想定の "柱頭金物:(ろ)" 形式とは
	// 限らず、コロン分割等の加工で文字列が失われる（空欄になる）ため、型名全体を登録する。
	std::string columnHardwareSpec(const std::string& typeName);

	// 柱の構造材 ID を組み立てる（Python 版 make_column_member_id）。
	// "{幅}×{成} - {種別}" を基本とし、柱頭・柱脚金物の仕様（空でないもの）を " / " 区切りで
	// 連結する。構造材ツールに金物専用フィールドが無いため、金物仕様は ID に含めて保持する。
	// 例: makeColumnMemberId(105, 105, "管柱", "柱頭金物:(ろ)", "柱脚金物:(い)")
	//       → "105×105 - 管柱 / 柱頭金物:(ろ) / 柱脚金物:(い)"
	std::string makeColumnMemberId(double width, double depth, const std::string& columnType,
								   const std::string& topHardware,
								   const std::string& bottomHardware);

	// 要素のローカル配置から 2D 配置座標を取り出す（Python 版 _get_position_2d）。
	// ホームズ君 IFC ではストーリの XY 原点が (0, 0) なので、ローカル配置 Location の XY を
	// そのまま平面座標として扱える（横架材と同じ座標系・同じセンタリングで補正できる）。
	// 取得できなければ false（親 PlacementRelTo は辿らない。M2 と同じ規約）。
	bool columnPosition2D(const Model& model, const Entity& element, core::Vec2& out);

	// 小屋束の直上に乗る横架材（母屋・棟木・登り梁）の断面幅を返す（Python 版
	// _member_width_on_top）。小屋束の平面位置 (px, py)（センタリング済み）と上端の絶対 Z
	// topAbs を横架材命令と突き合わせ、①平面 footprint が小屋束を覆い（中心線からの直交
	// 距離が半幅＋許容以内）、②小屋束上端がその材の Z 範囲（下端〜天端）に収まる材を探す。
	// ②は材が小屋束に乗る（材下端 ≈ 小屋束上端）場合も、小屋束が材を貫いて天端付近まで
	// 伸びる（棟束）場合も拾う。最も小屋束上端に近い材の幅を返し、見つからなければ nullopt。
	// 傾斜梁（登り梁）は天端 Z が軸方向に変化するため小屋束位置で補間する。判定は members の
	// 並び順に依存しない決定的な結果になる。
	std::optional<double> memberWidthOnTop(double px, double py, double topAbs,
										   const std::vector<core::MemberCommand>& members);

	// 柱の上端が上階の床（次階 FL）を貫いていれば通し柱（true）とみなす（Python 版
	// is_through_column）。上階が無い（最上階）ときは常に false。
	bool isThroughColumn(double topAbs, const std::optional<double>& nextFloorElevation);

	// 柱上端が届く span の to レベル（1 始まり）を求める（Python 版 resolve_column_to_level）。
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

	// column 命令から実在する span 柱レイヤを列挙した 1 件（Python 版 collect_column_spans の
	// タプル (from, to, layer)）。
	struct ColumnSpan
	{
		double from = 0.0;
		double to = 0.0;
		std::string layer;
	};

	// column 命令から実在する span 柱レイヤを (from, to) 昇順で列挙する（Python 版
	// collect_column_spans）。重複は除く。伏図（M13）が切断レベルで表示レイヤを絞るのに使う。
	std::vector<ColumnSpan> collectColumnSpans(const std::vector<core::ColumnCommand>& columns);

	// span 柱レイヤを base ストーリ（0 起点 index ＝ from − 1）ごとにまとめる（Python 版
	// collect_column_layers_by_story）。各ストーリのレイヤは (from, to) 昇順。parse/Story が
	// 各ストーリへ span レベルを作るのに使う。
	std::map<int, std::vector<std::string>>
	collectColumnLayersByStory(const std::vector<core::ColumnCommand>& columns);

	// STEP Model から柱の描画命令を組み立てる（Python 版 build_column_commands）。
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
