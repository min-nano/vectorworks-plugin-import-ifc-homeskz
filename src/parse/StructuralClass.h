//
//	parse/StructuralClass.h
//
//	Phase 1（IFC 解析）の構造クラス判定モジュール。Python 版 ifc/structural_class.py に
//	対応する（ROADMAP.md M4「構造クラス判定（純ロジック）」）。柱・横架材へ割り当てる
//	VectorWorks クラス名（04構造-02木造-… 階層の葉クラス）を定義し、部材種別を判定する。
//
//	ホームズ君 IFC の Name フィールドには部材種別が埋め込まれている
//	（例: "木梁:土台:1" / "木梁:軒桁:1_1" / "木梁:母屋:1_2" / "小屋束:1_1"）。
//	種別が判別できる場合はその IFC 記録を信用してクラスを決め、判別できない場合
//	（火打・隅木谷木・無名等）は階と高さの状況からクラスを推定する。
//
//	【SDK 非依存・幾何非依存】このモジュールは純粋な文字列／整数ロジックだけで完結し、
//	STEP エンティティグラフ（parse/Step）にも VectorWorks SDK にも依存しない。描画は
//	持たず（M7 横架材・M8 柱がここで決まったクラスを SetClass する）、無 SDK で単体
//	テストできる（CLAUDE.md「テスト方針」）。
//
//	クラス階層（VW のクラス名は "-" 区切りで全パスを連結する）:
//	  04構造
//	    02木造
//	      01土台   / 01土台
//	      02床組   / 01大引・02根太
//	      03柱     / 01通し柱・02管柱
//	      04梁桁   / 01小屋梁・02軒桁・03床梁・04胴差
//	      05小屋組 / 02小屋束・03母屋・04棟木・05垂木・06登り梁
//	      06耐力面材 / 01壁・02床・03屋根
//

#pragma once

#include <optional>
#include <string>

namespace HomeskzIfcImport::parse
{
	// 構造材（柱・横架材）と面材（床板・野地板）へ割り当てる VW クラス名。Python 版
	// ifc/structural_class.py の CLASS_* 定数と一字一句合わせる（04構造-02木造 を共通
	// 接頭辞に持つ葉クラス。番号と名称の間にスペースを入れず、全パスを "-" で連結する）。
	// constexpr const char* なので
	// 動的初期化を持たず、std::string / std::optional<std::string> と直接比較できる。
	inline constexpr const char* CLASS_DODAI = "04構造-02木造-01土台-01土台";
	inline constexpr const char* CLASS_OOBIKI = "04構造-02木造-02床組-01大引";
	inline constexpr const char* CLASS_NEDA = "04構造-02木造-02床組-02根太";
	// 床板（床合板。IfcSlab "床版"）は耐力面材（壁・床・屋根）の床に置く。
	inline constexpr const char* CLASS_FLOOR = "04構造-02木造-06耐力面材-02床";
	// 野地板（屋根の下地合板。IfcSlab "屋根版"）は耐力面材の屋根に置く。
	inline constexpr const char* CLASS_ROOF_SHEATHING = "04構造-02木造-06耐力面材-03屋根";
	inline constexpr const char* CLASS_TOSHIBASHIRA = "04構造-02木造-03柱-01通し柱";
	inline constexpr const char* CLASS_KUDABASHIRA = "04構造-02木造-03柱-02管柱";
	inline constexpr const char* CLASS_KOYABARI = "04構造-02木造-04梁桁-01小屋梁";
	inline constexpr const char* CLASS_NOKIGETA = "04構造-02木造-04梁桁-02軒桁";
	inline constexpr const char* CLASS_YUKABARI = "04構造-02木造-04梁桁-03床梁";
	inline constexpr const char* CLASS_DOUSASHI = "04構造-02木造-04梁桁-04胴差";
	inline constexpr const char* CLASS_KOYAZUKA = "04構造-02木造-05小屋組-02小屋束";
	inline constexpr const char* CLASS_MOYA = "04構造-02木造-05小屋組-03母屋";
	inline constexpr const char* CLASS_MUNAGI = "04構造-02木造-05小屋組-04棟木";
	inline constexpr const char* CLASS_TARUKI = "04構造-02木造-05小屋組-05垂木";
	// 登り梁（傾斜梁）。屋根の勾配に沿って軒から棟へ架かる小屋組の梁。母屋・棟木と
	// 同じく梁（小屋梁・軒桁）と重なって見にくいため専用レイヤ（n-登り梁）に分離する。
	inline constexpr const char* CLASS_NOBORIBARI = "04構造-02木造-05小屋組-06登り梁";

	// IFC Name から部材種別トークンを取り出す（Python 版 member_type_of_name 相当）。
	// "木梁:{種別}:{連番}" は中央の種別（例 "土台"・"軒桁"）を、"火打:0_1" /
	// "筋かい:1FL_1" のような 2 要素名は接頭辞を返す。未設定（空文字）は空文字を返す。
	std::string memberTypeOfName(const std::string& name);

	// IFC Name の種別から横架材クラスを返す（Python 版 member_class_from_name 相当）。
	// 直接対応が無ければ std::nullopt（未知種別＝火打・隅木谷木・無名等）。
	std::optional<std::string> memberClassFromName(const std::string& name);

	// 横架材のクラスを決定する（Python 版 resolve_member_class 相当）。
	//
	// IFC Name の種別で判別できればそれを信用する。判別できない部材（火打・隅木谷木・
	// 無名等）は階と高さの状況から、次の**優先順**で推定する（Python 版と同じ順序）:
	//   1. 最上階（index >= topIndex）の地廻り（軒）高さの横架材   → 小屋梁
	//   2. 最上階のそれより高い横架材（aboveEaves）               → 母屋
	//   3. 最下階（index <= 0）の横架材                          → 土台
	//   4. 中間階の横架材                                       → 床梁
	// 最上階の判定が先なので、単層（topIndex == 0）では 3 ではなく 1/2 が効く。
	std::string resolveMemberClass(const std::string& name, int index, int topIndex,
								   bool aboveEaves);

	// 小屋束を識別する IfcColumn.ObjectType（Python 版 COLUMN_STANDCOLUMN_OBJECT_TYPE）。
	// **この定義が唯一**で、クラス判定（resolveColumnClass）と柱種別名の変換
	// （parse/Column の resolveColumnType）が同じ文字列を見る（片方だけ直すと、
	// クラスは小屋束なのに構造材 ID の種別が管柱のまま、といったズレが起こる）。
	inline constexpr const char* kStandColumnObjectType = "STANDCOLUMN";

	// 柱のクラスを決定する（Python 版 resolve_column_class 相当）。
	//
	// 小屋束は IFC 記録（objectType == "STANDCOLUMN" または name が "小屋束" で始まる）で
	// 判別する。記録が無くても最上階（屋根＝index >= topIndex）の柱は小屋束として扱う。
	// 一般階の柱は上下端の高さ（複数階を貫くか＝isThrough）で通し柱／管柱を判別する。
	std::string resolveColumnClass(const std::string& objectType, const std::string& name,
								   int index, int topIndex, bool isThrough);
} // namespace HomeskzIfcImport::parse
