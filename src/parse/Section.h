//
//	parse/Section.h
//
//	Phase 1（IFC 解析）の断面ビューポート（軸組図）モジュール（docs/DEV-NOTES.md M14）。
//	伏図（parse/Sheet）がモデルを真上から見た図なのに対し、軸組図は建物を**鉛直面で切った図**
//	を通りごとにシートへ並べる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	【断面ビューポートは新規作成する】ISDK::CreateSectionViewport があるので、**検出した通りの
//	数だけその場で作る**。したがって
//	  * 図面テンプレートに断面ビューポートを仕込んでおく必要が無く、枚数の上限も無い
//	    （通りが何本あってもそのぶん作る）。
//	  * 命令は「どこを切るか」に加えて**視線の向きと映すレイヤ**を持つ（新規に作る以上、
//	    既製のビューポートから引き継げないため。下記）。
//	  * **断面の範囲は命令ごとに変わらない**ので命令には載せず、draw/Section が持つ
//	    （奥行き＝無制限・高さ＝建物を包む実寸・長さ＝断面線の長さ。切断面より奥を出さない等の
//	    表示の作法も同じ理由でそちら）。
//
//	【何を切るか＝柱梁の芯】柱（ColumnCommand）と梁（MemberCommand）の**両方**が通る通りだけを
//	切断位置にする（柱だけ・梁だけの通りは軸組図にしない）。X通り（定 X）は、ある X 座標に柱が
//	あり、かつ**その通りに平行＝Y 方向に走る梁**がある位置。Y通り（定 Y）はその逆。座標を
//	kClusterTol でクラスタリングし、柱と梁の両方を含むクラスタの平均を切断位置にする。
//	**大引・母屋は「梁」とみなさない**（kNonBeamClasses。それらだけが柱と重なる通りは軸組図に
//	しない）。
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

	// 軸組図を載せるシートレイヤのタイトルの基。**用紙 1 枚に収まらなければシートレイヤを
	// 足す**ので（M17）、複数枚になるときは "軸組図(1)" … と連番になる
	// （core::sectionSheetTitle）。何枚になるかは用紙の大きさと縮尺が決めるため、ここでは
	// 基の文字列だけを持つ。
	inline constexpr const char* kSectionSheetTitle = "軸組図";

	// 図面タイトルの接尾辞（図番 + これ）。"X1" → "X1通り"。
	inline constexpr const char* kSectionTitleSuffix = "通り";

	// 柱・梁の中心座標を 1 本の通りにまとめるクラスタ許容（mm）。同じ通りに乗る柱・梁は IFC
	// 上ほぼ同一座標なので小さめにし、隣の通り（半モジュール≈455mm 以上）を巻き込まない値にす
	// る。
	inline constexpr double kClusterTol = 100.0;

	// 切断位置（柱梁の芯）が名前付き通り芯に一致するとみなす許容（mm）。
	inline constexpr double kAxisMatchTol = 50.0;

	// 断面指示線を通り芯 bbox の端からさらに外へ延ばす余白（mm）。指示線が建物より短いと切断
	// 面が建物を切り抜かないため、必ず外側まで伸ばす。
	//
	// **断面の「長さの範囲」は〈断面線の長さ〉にしかできない**（無限に切り替える API が SDK
	// に無い。draw/Section.cpp 冒頭）ので、この余白が長さの範囲そのものになる。軒の出・
	// 基礎の張り出しが通り芯より外へ出ても切れないよう、3000mm と広めに取る。
	inline constexpr double kSectionLineMargin = 3000.0;

	// 視線の向きを示す点を、指示線の中点から見る側へ離す距離（mm）。値そのものに意味は無く
	// （向きだけを表す）、切断面と同じ点にならない大きさであればよい。
	inline constexpr double kViewPointOffset = 1000.0;

	// 名前付き通り芯 1 本（方向別に見たときの名前と座標）。X通りは座標＝X、Y通りは座標＝ Y（い
	// ずれもセンタリング済み）。
	struct NamedAxis
	{
		std::string name;
		double coord = 0.0;
	};

	// 名前付き通り芯を方向別に (名前, 座標) の昇順で返す。無名の通り芯は命名に使えないので除
	// く。同名の通り芯が複数区間に分かれていても 1 本にまとめる（最初に現れた区間の中点を採る。
	// 「最初」は入力順で決まるが、collectGridLines が
	// #id 昇順の決定的な並びを返すので結果も決定的になる）。
	std::vector<NamedAxis> namedAxes(const std::vector<GridLine>& lines, const core::Vec2& center,
									 core::SectionDirection direction);

	// 柱と梁の**両方**が通る切断位置（柱梁の芯）を方向別に昇順で返す。大引・母屋は梁とみなさ
	// ない（ヘッダ冒頭「何を切るか」）。
	std::vector<double> sectionCutPositions(const std::vector<core::ColumnCommand>& columns,
											const std::vector<core::MemberCommand>& members,
											core::SectionDirection direction);

	// 切断位置（昇順）を名前付き通り芯に照合して通り名を返す。一致すれば通り芯名、
	// 外れれば中間の通りとして `'` / `又` で連番する（ヘッダ冒頭）。
	std::vector<std::string> nameSectionCuts(const std::vector<double>& cuts,
											 const std::vector<NamedAxis>& axes);

	// 断面ビューポートが映すデザインレイヤ名（ストーリが作るレイヤすべて）。軸組図は
	// 建物まるごとの断面なので、伏図のように階・切断レベルで絞らない。**伏図記号レイヤ
	// （"{to}-柱伏図記号"）は含まない**——平面用の 2D 記号なので断面には要らない（story 命令が
	// 作るレイヤではないため、この実装では自然に外れる）。並びは stories の順＋通り芯。
	std::vector<std::string> sectionLayers(const std::vector<core::StoryCommand>& stories);

	// 軸組図のシートレイヤ番号の始まり＝**伏図の続き**（要件）。伏図の番号（数字の文字列）の
	// 最大値 + 1 を返す。数字でない番号・空の番号は読み飛ばし、伏図が 1 枚も無ければ 1。
	int sectionSheetStartNumber(const std::vector<core::SheetCommand>& sheets);

	// 軸組図のシートレイヤの通し方（番号の始まり・タイトルの基）。**何枚に分かれるかは
	// 用紙の大きさと縮尺が決める**ので、ここでは枚数に依らないこの 2 つだけを決める
	// （core/Document.h の SectionSheetCommand）。
	core::SectionSheetCommand
	buildSectionSheetCommand(const std::vector<core::SheetCommand>& sheets);

	// 軸組図の section 命令を組み立てる。X 通りの切断位置を昇順に並べ、続けて Y通りを並べる。
	// 通り芯が 1 本も無い（平面の広がりが決まらない）・映すレイヤが無い・柱梁の芯が
	// 1 つも無いときは空を返す。
	//
	// document は**組み立て済みの命令セット**（stories / members / columns …）を渡す——切断位
	// 置は柱・梁の命令から、映すレイヤはストーリの命令から決まるので、IFC を見直さずに済む。
	std::vector<core::SectionCommand> buildSectionCommands(Context& context,
														   const core::Document& document);
	std::vector<core::SectionCommand> buildSectionCommands(const Model& model,
														   const core::Document& document);
} // namespace HomeskzIfcImport::parse
