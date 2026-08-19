//
//	parse/Tag.h
//
//	Phase 1（IFC 解析）の断面寸法データタグモジュール。Python 版 ifc/tag.py に対応する
//	（ROADMAP.md M13）。各横架材の断面寸法（"120×180" 等）を図の上に表示するための
//	データタグ命令（core::TagCommand）を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	【IFC は見ない】タグの位置・向きは**組み立て済みの横架材命令**（core::MemberCommand）の
//	ジオメトリだけから決まる。仕口（parse/Joint）・記号（parse/ColumnMark）と同じで、
//	IFC を読み直さずに命令セットから導出する。
//
//	【Python 版との差異＝軸組図にもタグを載せる】Python 版は**伏図だけ**にタグを置き、
//	tag 命令を文書の直下に平らに持って、描画側が「tag['layer'] がそのビューポートの表示
//	レイヤに含まれるか」で振り分けていた。本移植は**軸組図（断面ビューポート）にも置く**ので
//	その突き合わせが成り立たない——断面に映る横架材は「レイヤ」では選べず、**切断面に乗るか
//	どうか**で決まるからである。そこで振り分けをここ（解析側）で済ませ、タグは
//	**そのビューポートの命令（core::ViewportCommand::tags）の中**に入れる。描画側は
//	「このビューポートの tags を置く」だけになり、伏図と軸組図で 1 つの実装を共有できる。
//
//	【伏図のタグ】表示レイヤに乗る横架材 1 本につき 1 つ。位置は**軸中央から軸直交方向
//	（上または左）へ断面幅/2 だけ寄せた点＝部材の辺の中央**（左右に伸びる梁は上辺の中央、
//	上下に伸びる梁は左辺の中央）。ここにデータタグの下端中央が来る。**余白を足さず面
//	ちょうどに置く**のは、部材から離すと VW が関連付け先へ引出線を描くため（Python 版と同じ）。
//
//	【軸組図のタグ】**切断面に乗る横架材**（＝その通りに沿って走り、芯が切断位置にある材）
//	1 本につき 1 つ。断面では材が長方形の立面として見え、**その上辺＝天端**がそのまま
//	命令の start/end（天端中央線）の投影になるので、位置は投影した天端線の中点にする
//	（伏図で辺の中央へ寄せるのと同じ意図＝タグの下端中央が部材の辺に接する）。切断面に
//	直交して走る材は断面が小さく写るだけなのでタグを置かない（置くと図が読めなくなる）。
//	**大引・母屋は除外しない**——parse/Section が切断位置の判定からそれらを外すのは「その
//	通りを軸組図にするか」を決めるためで、いったん切ると決めた面に写る材はすべて寸法を
//	示したい（軸組図は建物まるごとの断面）。
//
//	【断面の注釈空間】ビューポート注釈は**その図の投影された 2 次元空間**に置かれる。
//	伏図（平面ビューポート）は真上から見た図なので投影＝平面座標そのまま（Python 版が
//	平面座標をそのまま渡して成立しているのが実証）。軸組図は横から見た図なので
//	**x＝画面右方向に測った距離・y＝高さ Z** になる。画面右方向は視線の向きが決める
//	（parse/Section が視線を決めており、X通り＝−X 方向を見る＝画面右は +Y、Y通り＝+Y 方向を
//	見る＝画面右は +X）。したがって
//	  * X通り（定 X の切断面）… 注釈座標 = (材の Y, 天端 Z)
//	  * Y通り（定 Y の切断面）… 注釈座標 = (材の X, 天端 Z)
//	で、**ワールド原点が注釈空間の原点へ写る**とみなす。これは平面ビューポートで平面座標が
//	そのまま通ることの自然な延長だが、**断面ビューポートについては実機で未確認**なので
//	ローカル確認の項目に挙げてある（ずれていた場合の直しは、この投影に定数のオフセットを
//	足すだけで済む形にしてある＝sectionAnnotationPoint）。
//

#pragma once

#include "core/Document.h"

#include <vector>

namespace HomeskzIfcImport::parse
{
	// 適用するデータタグスタイル名（VW 側でユーザーが用意した「断面寸法」スタイル。
	// Python 版 ifc/tag.py TAG_STYLE）。構造材のプラグインスタイルと同じで、**文書に
	// 無ければ描画側がスタイル無しで置く**（タグを失わない）。
	inline constexpr const char* kTagStyle = "断面寸法";

	// 軸方向 (dx, dy) に沿った読みやすい文字角度（度）を返す（Python 版 _tag_angle）。
	// 軸の角度を (-90, 90] に正規化し、文字が上下反転しないようにする。
	double tagAngle(double dx, double dy);

	// 軸 (dx, dy) に直交する単位ベクトルのうち「上または左」を向く側（Python 版 _offset_side）。
	// y が大きい（上）方を選び、y が同等（材が南北向き）なら x が小さい（左）方を選ぶ。
	// 長さが 0 の軸には既定（上＝(0, 1)）を返す。
	core::Vec2 tagOffsetSide(double dx, double dy);

	// 線 (du, dv) の法線のうち**上を向く側**の単位ベクトル。断面（軸組図）でタグを部材の
	// 上辺から逃がす向きに使う。長さが 0 の線には既定（真上＝(0, 1)）を返す。
	core::Vec2 upwardNormal(double du, double dv);

	// 横架材の平面座標＋天端 Z を、断面ビューポートの注釈空間へ投影する
	// （ヘッダ冒頭「断面の注釈空間」）。**投影の定義はここ 1 か所**で、実機確認でずれが
	// 判明したときもここだけを直せばよい。
	core::Vec2 sectionAnnotationPoint(const core::Vec2& plan, double elevation,
									  core::SectionDirection direction);

	// 伏図（平面ビューポート）1 枚ぶんのタグ命令。viewport の表示レイヤに配置先レイヤが
	// 含まれる横架材 1 本につき 1 つを、members の並び順で返す（Python 版 build_tag_commands ＋
	// vw/sheet.py の振り分け）。
	std::vector<core::TagCommand>
	buildPlanTagCommands(const std::vector<core::MemberCommand>& members,
						 const core::ViewportCommand& viewport);

	// 軸組図（断面ビューポート）1 枚ぶんのタグ命令。切断面に乗る横架材 1 本につき 1 つを、
	// members の並び順で返す（ヘッダ冒頭「軸組図のタグ」）。
	std::vector<core::TagCommand>
	buildSectionTagCommands(const std::vector<core::MemberCommand>& members,
							const core::SectionCommand& section);

	// 文書中の全ビューポート（伏図・軸組図）へタグ命令を割り当てる。**sheets / sections が
	// 確定した後**に呼ぶ（parse/BuildDocument の最後）。
	void attachTagCommands(core::Document& document);
} // namespace HomeskzIfcImport::parse
