//
//	parse/Sheet.h
//
//	Phase 1（IFC 解析）のシート（伏図）モジュール（docs/DEV-NOTES.md M13）。**IFC
//	からシート構成を読み取るわけではない**——取り込んだ要素の有無（基礎があるか・屋根版を持つ階
//	はどれか・柱の span はどう分かれたか）から「どの伏図を作り、そこに何を映すか」を決める。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	作る伏図は 4 種。並びは 基礎伏図 →（階ごとに）柱梁伏図 → 柱壁伏図 → 母屋伏図 で、番号も
//	この並びに "1" から振る（buildSheetCommands。下の階から積み上げる。M45）:
//	  * **基礎伏図**（buildFoundationSheetCommands）… 基礎要素があるときだけ 1 枚。
//	    表示レイヤは 底盤・立上り・床束・アンカーボルト・通り芯。
//	  * **柱梁伏図**（buildFloorFramingSheetCommands）… **横架材の高さ（伏図レベル）1 つに
//	    つき 1 枚**（parse/PlanLevel。どの階も高さが 1 つなら FL ストーリ 1 つにつき 1 枚）。
//	    表示レイヤは その伏図レベルの横架材（"2±0-横架材" / "2-1-横架材" / "R±0-横架材"。
//	    符号は parse/PlanLevel）・軒桁・登り梁（その高さを跨ぐもの）＋切断レベルを
//	    span が含む柱レイヤ＋（最上階以外は）床＋（最下階かつ基礎ありなら）アンカーボルト＋通り芯。
//	  * **柱壁伏図**（buildColumnWallSheetCommands。M45）… 最上階を除く階ごとに 1 枚（柱も
//	    耐力壁も無い階には作らない）。表示レイヤは その階の伏図レベルの切断を span が含む
//	    柱レイヤ＋その階の伏図レベルに立つ耐力壁＋通り芯。**耐力壁はここにだけ映し、柱梁伏図
//	    には映さない**（ご要望）。その階の柱梁伏図の直後に並ぶ。
//	  * **母屋伏図**（buildMoyaSheetCommands）… 屋根版を持つ階ごとに 1 枚。表示レイヤは
//	    その階の小屋組（母屋・垂木・野地板）＋切断レベルを span が含む柱・登り梁レイヤ＋
//	    通り芯（同じ階の軒桁は薄く重ねる）。**登り梁は上端がその階のいちばん上の伏図レベルに
//	    届くものだけ**——それ以外は跨ぐ高さの柱梁伏図に映してあり、重ねると高さの関係が
//	    直感に反するため（ご要望。parse/PlanLevel の noboribariSpan）。
//
//	【切断レベルという考え方】柱・小屋束は span レイヤ（"2-1_3±0-柱"）に分かれている
//	（parse/Column）。比べるのは符号を引き直した伏図レベルの通し番号（parse/PlanLevel の
//	parseSpanOrdinals。高さが 1 つの階ばかりなら階の番号）。伏図はその図が切る高さ（切断
//	レベル）を span が含む [from ≤ cut ≤ to] レイヤだけを映す。これで
//	  * その階を base とする柱の断面と、下から貫いてこの高さに達する通し柱が出る、
//	  * 下屋の小屋束（"2±0_2+1-柱"、to＝2 階の母屋のレベル）が上階の小屋伏図へ写り込まない、
//	  * 母屋伏図（切断は母屋のレベル + 0.25）には屋根を貫いて立ち上がる主屋の柱だけが出る
//	    （母屋を支える小屋束の断面は出さない＝母屋の上からの見下げ図だから）
//	という 3 つが同時に満たされる。
//
//	【伏図記号レイヤも 1 枚だけ載せる】柱梁伏図・母屋伏図は、切断位置の**直下**（to < 切断で
//	最大の to）の伏図記号レイヤ "{to}-柱伏図記号" も映す（M12）。断面記号は span レイヤに
//	載るので [from ≤ cut ≤ to]、伏図記号は to < cut と**排他**になり、同じ柱が両方の記号で
//	出ることはない。レイヤ名の規約も「直下」の選び方も持つのは parse/ColumnMark 側で、
//	ここはそれを呼ぶだけにしてある（名前の組み立てを 2 か所へ散らさない）。
//
//	【グラフィック凡例も同じ理由でここが決める】伏図のシートレイヤには、その図に出る
//	シンボルの凡例（VW 標準の "GraphicLegend" PIO）を 1 つ載せる（M13）。凡例は**スタイル
//	無しのオブジェクト**として置くので（core/Document.h の LegendCommand）、ここが決めるのは
//	「どの伏図に凡例を載せるか」だけになる:
//	  * 基礎伏図 … **アンカーボルトを 1 本でも置いたときだけ**載せる
//	    （置かなければ凡例に並ぶものが無く、空の箱が図面に残る）。
//	  * 柱梁伏図・母屋伏図 … 常に載せる。凡例は SheetCommand の中に入れ子で持
//	    つので、番号で突き合わせる必要が無い（タグを ViewportCommand の中に持たせたのと同じ考
//	    え方）。
//
//	【設計上の要点】
//	  * **母屋伏図を作る条件は「屋根版を持つ階」だけ**。本移植は屋根版のある階にしか垂木・
//	    野地板レベルを作らないので（parse/Story）、屋根版の無い最上階に伏図を作ると空の
//	    ビューポートが残る。ホームズ君の出力では最上階は必ず主屋根の屋根版を含むので、
//	    実データでは「最上階＋下屋根のある階」に落ち着く。
//	  * **小屋組のレイヤは命令の配置先で判定する**（母屋・登り梁）。parse/Story がレベルを
//	    作る条件と同じ述語（横架材命令の配置先レイヤ）を通すので、「レイヤは無いのに伏図が
//	    映そうとする」という齟齬が構造的に起きない。
//

#pragma once

#include "core/Document.h"
#include "parse/Column.h"
#include "parse/Step.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 基礎伏図のシートレイヤ番号・タイトル。番号は文字列で、**シートレイヤ名がそのまま番号を
	// 担う**（描画側 draw/Sheet）。
	inline constexpr const char* kFoundationSheetNumber = "1";
	inline constexpr const char* kFoundationSheetTitle = "基礎伏図";

	// 基礎伏図の後ろの伏図（柱梁伏図・柱壁伏図・母屋伏図）のシートレイヤ番号の開始値。基礎
	// 伏図（番号 1）に続けて 2 から並び順に振る（buildSheetCommands）。
	// **基礎が無い文書でも 2 から始める**（番号が階に対して一定になり、基礎の有無で図番が動か
	// ない）。
	inline constexpr int kFloorPlanStartNumber = 2;

	// 柱梁伏図（床伏図・小屋伏図）の切断レベル＝その伏図レベルの通し番号 + 0.25（通し番号
	// − 1 からの相対なので 1.25）。1FL・2FL・RFL が 1 段ずつなら 1 階床伏図＝1.25 /
	// 2 階床伏図＝3.25 / 2 階小屋伏図＝5.25（通し番号は各階のあとに母屋のレベルを 1 つ空ける。
	// parse/PlanLevel）。
	inline constexpr double kFloorPlanCutOffset = 1.25;

	// 母屋伏図の切断レベル＝その階の母屋のレベル（parse/PlanLevel の moyaOrdinal）+ 0.25。
	// **その階の小屋束（span の to＝母屋のレベル）を超え、上階の床（母屋のレベル + 1）には
	// 届かない高さ**をサンプルするので、屋根を貫く主屋の柱だけが載る。
	inline constexpr double kMoyaPlanCutOffset = 0.25;

	// 伏図タイトルの種別ラベル。階番号と組み合わせて "{n}階床伏図" のように使う。
	inline constexpr const char* kFloorPlanFloorLabel = "床";
	inline constexpr const char* kFloorPlanRoofLabel = "小屋";
	inline constexpr const char* kMoyaPlanLabel = "母屋";
	inline constexpr const char* kColumnWallPlanLabel = "柱壁";

	// 柱梁伏図のタイトル。最上階は主屋根が架かる階番号を付けた "{count-1}階小屋伏図"、
	// それ以外は "{index+1}階床伏図"。小屋組（母屋・垂木・野地板）は専用の母屋伏図へ分けるの
	// で、ここに母屋の表記は付けない。
	std::string floorPlanTitle(std::size_t index, bool isTop, std::size_t count);

	// 母屋伏図のタイトル。屋根が架かる階番号（0 起点のストーリ index をそのまま用いる）
	// を付けた "{index}階母屋伏図"。2 階建てなら主屋根（index=2）＝"2階母屋伏図"、
	// 中間階の下屋根（index=1）＝"1階母屋伏図"。
	std::string moyaPlanTitle(std::size_t index);

	// 柱壁伏図のタイトル。床伏図と同じ階番号を付けた "{index+1}階柱壁伏図"。
	std::string columnWallPlanTitle(std::size_t index);

	// span 柱レイヤのうち切断レベル cut を含む（from ≤ cut ≤ to）ものを (from, to) 昇順で返す。
	// spans は parse/Column の collectColumnSpans。
	std::vector<std::string> spanLayersAtCut(const std::vector<ColumnSpan>& spans, double cut);

	// **凡例の配置点も中身もここでは決めない**（M18・スタイル無し化）。用紙の大きさは
	// 描画するときにシートレイヤから読むもので解析側には分からないため、置き場所は描画側が
	// 用紙の割り付け（core::planLayout の legendTopRight＝ビューポートのために空けた右の
	// 1 列）から決める。何を並べるか（ソース定義）も描画側がタグ付きデータで与える
	// （draw/Legend）。

	// 基礎伏図の sheet 命令（無ければ空）。基礎要素が 1 つも無ければ空を返す——表示すべき"F-底
	// 盤" ほかのレイヤが生成されず、ビューポートが空になるため。
	//
	// グラフィック凡例は**アンカーボルトを 1 本でも置いたときだけ**載せる
	// （凡例に並ぶものが無ければ空の箱が残るため）。
	std::vector<core::SheetCommand> buildFoundationSheetCommands(Context& context);
	std::vector<core::SheetCommand> buildFoundationSheetCommands(const Model& model);

	// 【番号は buildSheetCommands が振る】下の 3 つ（柱梁伏図・母屋伏図・柱壁伏図）は番号
	// （number / drawingNumber）を空のまま返す。番号は並び（階ごとの積み上げ）で決まるので、
	// 並べる buildSheetCommands だけが振る（番号の振り方を 1 か所に置く）。

	// 伏図レベル（横架材の高さ）ごとの柱梁伏図の sheet 命令（ストーリが無ければ空）。
	// 階に伏図が 2 枚以上あればタイトルに高さを添える（"2階床伏図（FL-872）"）。
	// グラフィック凡例を各シートに 1 つ載せる。
	std::vector<core::SheetCommand> buildFloorFramingSheetCommands(Context& context);
	std::vector<core::SheetCommand> buildFloorFramingSheetCommands(const Model& model);

	// 屋根版を持つ階ごとの母屋伏図の sheet 命令（無ければ空）。Elevation 昇順（最下階→
	// 最上階）。柱梁伏図と同じくグラフィック凡例を 1 つ載せる。
	std::vector<core::SheetCommand> buildMoyaSheetCommands(Context& context);
	std::vector<core::SheetCommand> buildMoyaSheetCommands(const Model& model);

	// 階ごとの柱壁伏図の sheet 命令（M45。無ければ空）。最上階を除く階ごとに 1 枚で、柱も
	// 耐力壁も無い階には作らない。Elevation 昇順。グラフィック凡例を 1 つ載せる。
	//
	// 柱は**その階の伏図レベルすべて**の切断（柱梁伏図と同じ。通し番号 + 0.25）を span が
	// 含むものを載せる——スキップフロアで階に伏図レベルが 2 つ以上あっても、柱壁伏図は階に
	// 1 枚なので、どの高さに立つ柱も載せる。耐力壁も同じく、その階の伏図レベルに立つもの。
	// 横架材は載せない（ご要望）。
	std::vector<core::SheetCommand> buildColumnWallSheetCommands(Context& context);
	std::vector<core::SheetCommand> buildColumnWallSheetCommands(const Model& model);

	// 上の 4 種を並べて番号を振った sheet 命令。並びは 基礎伏図 →（階ごとに、その階の
	// 柱梁伏図（伏図レベルの順）→ その階の柱壁伏図）→ 母屋伏図。**下の階から積み上げる**
	// （ご要望。M45）。番号は基礎伏図が 1、その後ろを kFloorPlanStartNumber から並び順に振る。
	std::vector<core::SheetCommand> buildSheetCommands(Context& context);
	std::vector<core::SheetCommand> buildSheetCommands(const Model& model);
} // namespace HomeskzIfcImport::parse
