//
//	parse/Section.h
//
//	Phase 1（IFC 解析）の断面ビューポート（軸組図）モジュール。Python 版 ifc/section.py に
//	対応する（ROADMAP.md M14）。伏図（parse/Sheet）がモデルを真上から見た図なのに対し、
//	軸組図は建物を**鉛直面で切った図**を通りごとにシートへ並べる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	【Python 版との最大の差異＝断面ビューポートを新規作成する】Python 版（VectorScript）は
//	断面ビューポートを作れないため、シートレイヤ "A" にあらかじめ 40 枚（X1..X20 / Y1..Y20）の
//	断面指示線・ビューポートを**手で用意しておき**、その位置・図番・タイトルだけを差し替えて
//	流用していた（余った既製ぶんは削除）。C++ SDK には ISDK::CreateSectionViewport があるので、
//	本移植は**検出した通りの数だけ新規に作る**。その結果、
//	  * 既製の枚数上限（Python 版 MAX_PER_DIRECTION＝20）が消える——通りが 20 本を超えても
//	    切り捨てない。
//	  * 流用元の図番（Python 版 'source_number'）が消える——命令は「どこを切るか」だけを持つ。
//	  * 図面の下準備（40 枚の手置き）が不要になる——テンプレートに依存しない。
//	代わりに、Python 版では既製ビューポートが持っていた情報のうち**視線の向きと映すレイヤ**を
//	解析側が決めて命令に載せる（下記）。**断面の範囲は命令ごとに変わらない**ので命令には
//	載せず、draw/Section が持つ（奥行き＝無制限・高さ＝建物を包む実寸・長さ＝断面線の長さ。
//	切断面より奥を出さない等の表示の作法も同じ理由でそちら）。
//
//	【何を切るか＝柱梁の芯】柱（ColumnCommand）と梁（MemberCommand）の**両方**が通る通りだけを
//	切断位置にする（柱だけ・梁だけの通りは軸組図にしない）。X通り（定 X）は、ある X 座標に柱が
//	あり、かつ**その通りに平行＝Y 方向に走る梁**がある位置。Y通り（定 Y）はその逆。座標を
//	kClusterTol でクラスタリングし、柱と梁の両方を含むクラスタの平均を切断位置にする。
//	**大引・母屋は「梁」とみなさない**（kNonBeamClasses。それらだけが柱と重なる通りは軸組図に
//	しない。Python 版 _NON_BEAM_CLASSES と同じ）。
//
//	【どう名付けるか】切断位置が名前付き通り芯（IfcGridAxis）に kAxisMatchTol 以内で一致すれば
//	その名前。外れれば**中間の通り**として、直前（座標の小さい側）の通りを基準に、その通りの
//	書式で連番する——数字書式（"X1"）は `'` を足して "X1'" / "X1''"、いろは書式（"い"）は `又` を
//	前置して "又い" / "又又い"（中間の順番ぶんだけ増やす）。
//

#pragma once

#include "core/Document.h"
#include "parse/Grid.h"
#include "parse/Step.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 軸組図を載せるシートレイヤの番号（＝レイヤ名）とタイトル（Python 版 vw/section.py
	// SECTION_SHEET_LAYER＝"A"）。伏図が "1" / "2" … と数字なので、軸組図は英字にして
	// 番号の連番と衝突させない。**全 section 命令が同じシートに載る**（Python 版と同じ）。
	inline constexpr const char* kSectionSheetNumber = "A";
	inline constexpr const char* kSectionSheetTitle = "軸組図";

	// 図面タイトルの接尾辞（図番 + これ。Python 版 TITLE_SUFFIX）。"X1" → "X1通り"。
	inline constexpr const char* kSectionTitleSuffix = "通り";

	// 柱・梁の中心座標を 1 本の通りにまとめるクラスタ許容（mm。Python 版 CLUSTER_TOL）。
	// 同じ通りに乗る柱・梁は IFC 上ほぼ同一座標なので小さめにし、隣の通り（半モジュール
	// ≈455mm 以上）を巻き込まない値にする。
	inline constexpr double kClusterTol = 100.0;

	// 切断位置（柱梁の芯）が名前付き通り芯に一致するとみなす許容（mm。Python 版 AXIS_MATCH_TOL）。
	inline constexpr double kAxisMatchTol = 50.0;

	// 断面指示線を通り芯 bbox の端からさらに外へ延ばす余白（mm。Python 版 SECTION_LINE_MARGIN
	// は 1000mm）。指示線が建物より短いと切断面が建物を切り抜かないため、必ず外側まで伸ばす。
	//
	// **断面の「長さの範囲」は〈断面線の長さ〉にしかできない**（無限に切り替える API が SDK に
	// 無い。draw/Section.cpp 冒頭）ので、この余白が長さの範囲そのものになる。軒の出・基礎の
	// 張り出しが通り芯より外へ出ても切れないよう、Python 版より広い 3000mm を取る。
	inline constexpr double kSectionLineMargin = 3000.0;

	// 視線の向きを示す点を、指示線の中点から見る側へ離す距離（mm）。値そのものに意味は無く
	// （向きだけを表す）、切断面と同じ点にならない大きさであればよい。
	inline constexpr double kViewPointOffset = 1000.0;

	// 名前付き通り芯 1 本（方向別に見たときの名前と座標）。X通りは座標＝X、Y通りは座標＝Y
	// （いずれもセンタリング済み）。Python 版 _named_axes が返すタプルに対応する。
	struct NamedAxis
	{
		std::string name;
		double coord = 0.0;
	};

	// 名前付き通り芯を方向別に (名前, 座標) の昇順で返す（Python 版 _named_axes）。無名の
	// 通り芯は命名に使えないので除く。同名の通り芯が複数区間に分かれていても 1 本にまとめる
	// （最初に現れた区間の中点を採る。「最初」は入力順で決まるが、collectGridLines が
	// #id 昇順の決定的な並びを返すので結果も決定的になる）。
	std::vector<NamedAxis> namedAxes(const std::vector<GridLine>& lines, const core::Vec2& center,
									 core::SectionDirection direction);

	// 柱と梁の**両方**が通る切断位置（柱梁の芯）を方向別に昇順で返す（Python 版 _cut_positions）。
	// 大引・母屋は梁とみなさない（ヘッダ冒頭「何を切るか」）。
	std::vector<double> sectionCutPositions(const std::vector<core::ColumnCommand>& columns,
											const std::vector<core::MemberCommand>& members,
											core::SectionDirection direction);

	// 切断位置（昇順）を名前付き通り芯に照合して通り名を返す（Python 版 _name_cuts）。
	// 一致すれば通り芯名、外れれば中間の通りとして `'` / `又` で連番する（ヘッダ冒頭）。
	std::vector<std::string> nameSectionCuts(const std::vector<double>& cuts,
											 const std::vector<NamedAxis>& axes);

	// 断面ビューポートが映すデザインレイヤ名（ストーリが作るレイヤすべて）。軸組図は
	// 建物まるごとの断面なので、伏図のように階・切断レベルで絞らない。**伏図記号レイヤ
	// （"{to}-柱伏図記号"）は含まない**——平面用の 2D 記号なので断面には要らない（story 命令が
	// 作るレイヤではないため、この実装では自然に外れる）。並びは stories の順＋通り芯。
	std::vector<std::string> sectionLayers(const std::vector<core::StoryCommand>& stories);

	// 切断面に**平行**な通り芯のクラス名（＝その軸組図では非表示にするクラス）。
	//
	// 【なぜ隠すか】X通りの軸組図（定 X の切断面）を切ると、切断面と**直交**する Y通りの
	// 通り芯は「鉛直の一点鎖線＋基点バブル」として断面に写る（軸組図に通り名を出すのは
	// これが目的で、映すレイヤに "共通" を入れている理由でもある）。一方、切断面と**平行**な
	// X通りの通り芯——なかでも**切断位置そのものに乗っている 1 本**——は、切断面の中に
	// 収まってしまうため「切断面より奥は表示しない」設定では落ちず、**紙面に平行な水平の
	// 一点鎖線**として写り込む（軸組図の足元に、通り芯 "共通" レイヤの高さで 1 本引かれる）。
	// 平面図の線がそのまま断面に寝て出るだけで軸組図としての意味は無いので、その図だけで
	// クラスごと消す。
	//
	// 方向の対応は「その軸組図と同じ方向の通り芯＝平行」。X通りの軸組図では X通りのクラス、
	// Y通りの軸組図では Y通りのクラスを隠す（通り芯のクラス分けと同じ述語 isXAxis で
	// 付いた名前なので、判定が食い違うことはない。parse/Grid.h）。
	const char* gridClassFor(core::SectionDirection direction);

	// 軸組図の section 命令を組み立てる（Python 版 build_section_commands）。X通りの切断位置を
	// 昇順に並べ、続けて Y通りを並べる。通り芯が 1 本も無い（平面の広がりが決まらない）・
	// 映すレイヤが無い・柱梁の芯が 1 つも無いときは空を返す。
	//
	// document は**組み立て済みの命令セット**（stories / members / columns …）を渡す
	// ——切断位置は柱・梁の命令から、映すレイヤはストーリの命令から決まるので、IFC を見直さずに
	// 済む（Python 版が members / columns を受け取るのと同じ考え方を、レイヤまで広げたもの）。
	std::vector<core::SectionCommand> buildSectionCommands(Context& context,
														   const core::Document& document);
	std::vector<core::SectionCommand> buildSectionCommands(const Model& model,
														   const core::Document& document);
} // namespace HomeskzIfcImport::parse
