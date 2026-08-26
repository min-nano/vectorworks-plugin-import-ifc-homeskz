//
//	core/Layout.h
//
//	シートレイヤ（用紙）の割り付け——**縮尺の自動調整とビューポートの整列**
//	（docs/DEV-NOTES.md M16）。伏図（draw/Sheet）と軸組図（draw/Section）が「用紙のどこへ・
//	どの縮尺で置くか」を決めるために使う**純計算**で、SDK も IFC も知らない。
//
//	【なぜ core に置くか】用紙の大きさは**描くときにしか分からない**（シートレイヤから SDK で
//	読む）ので、割り付けを解析フェーズで決めることはできない。一方、決め方そのもの——縮尺の
//	階梯・収まる縮尺の選び方・段組みの数え方——は SDK と無関係な算数なので、描画側から切り
//	離してここへ置き、無 SDK テスト（CoreLayoutTests）で検証する（CLAUDE.md「描画側から
//	切り離せる純計算」。レイヤの希望スタック順・地中梁の呑み込みと同じ扱い）。
//
//	【割り付けの決まりごと】
//	  * 縮尺は**キリの良い分母だけ**（1/200・1/175 … 1/5）を使い、収まる中で最も大きい図
//	    ＝**最小の分母**を選ぶ（kScaleDenominators）。
//	  * 伏図は**全図が同じ縮尺・同じ位置**。用紙をめくっても図が動かないよう、縮尺も中心も
//	    「文書全体の平面の広がり」から 1 回だけ決める（planLayout）。
//	  * 伏図は**用紙いっぱいで最大の縮尺**にする。グラフィック凡例のために縮尺を落とさず、
//	    **置き場所だけで避ける**——図を左へ寄せ、右上の空きへ凡例を置く。避ける幅は
//	    **描画側が実測した凡例の幅**（planLayout の legendWidth）で、凡例は図面の内容で
//	    伸び縮みするので定数で決め打ちにしない。
//	  * 軸組図は 1 枚の用紙に複数並ぶ。**上下 2 段**になるように縮尺を決め（kSectionRows）、
//	    1 段に何枚入るかは用紙の幅が決める。入りきらなければシートレイヤを足す
//	    （sectionSheetCount）。
//
//	【単位】用紙まわりの長さはすべて**用紙座標の mm**、建物の広がり（content）は**実寸の
//	mm**。縮尺は分母（1/100 なら 100.0）で持ち、実寸 ÷ 分母 ＝ 用紙上の長さになる。
//

#pragma once

#include "core/Geometry.h"

#include <array>
#include <cstddef>
#include <string>

namespace HomeskzIfcImport::core
{
	// 使ってよい縮尺の分母。**キリの良い値だけ**を使う（1/200 … 1/5）。昇順＝図が大きく
	// なる順に持ち、fitScale は「収まる中で最初のもの」＝最小の分母を返す。
	inline constexpr std::array<double, 13> kScaleDenominators{
		5.0, 10.0, 15.0, 20.0, 25.0, 30.0, 50.0, 75.0, 100.0, 125.0, 150.0, 175.0, 200.0};

	// 用紙端に残す余白（用紙 mm）。図枠を持たない素の用紙に置くので、印刷の余白ぶんだけ
	// 内側へ寄せる。
	inline constexpr double kSheetMargin = 15.0;

	// ビューポート同士・ビューポートと凡例の間隔（用紙 mm）。
	inline constexpr double kViewportGap = 15.0;

	// **凡例の幅は定数で持たない**（M16。実機のローカル確認を経ての結論）。グラフィック凡例
	// の大きさは**その図面に何が並ぶか**で決まる——シンボルの種類が増えれば伸びるし、
	// アンカーボルトを置かない文書では凡例そのものが無い。当初は箱幅の定数（150mm）を
	// そのまま「空けておく幅」にしていたが、実機の凡例は 25mm ほどにしか広がらず、
	// 余らせた 125mm のせいで **1/50 で収まる建物が 1/75 まで落ちて**いた。
	// そこで**描画側が置いた凡例を実測して**（draw/Legend の measureLegendWidth）
	// planLayout へ渡す形にしてある。

	// 用紙の大きさが読めなかったときに使う既定（A3 横。用紙 mm）。**シートレイヤから
	// 読めた値があれば必ずそちらを使う**（draw/DrawUtil の SheetPageArea）。
	inline constexpr Vec2 kDefaultPaperSize{420.0, 297.0};

	// 軸組図の段数。**上下 2 段**（要件）。1 段に何枚入るかは用紙の幅と縮尺が決める。
	inline constexpr std::size_t kSectionRows = 2;

	// 用紙上の矩形（用紙 mm）。min が左下・max が右上。
	struct PaperArea
	{
		Vec2 min;
		Vec2 max;

		double width() const
		{
			return max.x - min.x;
		}

		double height() const
		{
			return max.y - min.y;
		}

		Vec2 size() const
		{
			return Vec2{width(), height()};
		}

		Vec2 center() const
		{
			return Vec2{(min.x + max.x) / 2.0, (min.y + max.y) / 2.0};
		}
	};

	// 用紙の外形から作図可能域（四辺を kSheetMargin だけ内側へ寄せた矩形）を作る。
	// 余白を引くと潰れてしまうほど小さい用紙では、外形をそのまま返す（0 幅の作図域を
	// 作らない）。
	PaperArea drawingArea(const PaperArea& page);

	// content（実寸 mm）が available（用紙 mm）に収まる最大の図＝**最小の分母**を階梯から
	// 選ぶ。どれにも収まらなければ最大の分母（＝いちばん小さい図）を返す——**図がはみ出す
	// くらいなら小さく描く**。content・available が退化している（0 以下）ときも同じ。
	double fitScale(const Vec2& content, const Vec2& available);

	// 伏図 1 枚の割り付け。**全シートで同じ値**になる（同じ内容・同じ用紙から計算する）ので、
	// 用紙をめくってもビューポートの位置が変わらない。
	//
	//   scale          … 縮尺の分母（全伏図で共通）
	//   plan           … 図が占めてよい領域（用紙 mm。＝作図可能域そのもの）
	//   viewportCenter … 図の中心を合わせる点（用紙 mm）。**建物の中心**がここへ来る
	//   legendTopRight … グラフィック凡例の右上を合わせる点（用紙 mm。作図可能域の右上）
	//
	// plan を持つのは、**描けた図が本当に用紙へ収まったかを描画側が測って確かめられる**
	// ようにするため（M16）。縮尺は「命令セットから求めた建物の広がり」で決めるが、実際に
	// 描かれる図はそれより少し大きくなりうる（通り芯の丸のように、命令の座標には現れない
	// ものが図には出る）。はみ出したら診断へ残す——黙って用紙から出ているより、
	// ローカル確認のときに気付ける方がよい。
	struct PlanLayout
	{
		double scale = 1.0;
		PaperArea plan;
		Vec2 viewportCenter;
		Vec2 legendTopRight;
	};

	// 伏図の割り付けを決める。content は**文書全体**の平面の広がり（実寸 mm）、
	// legendWidth は**実際に置いた凡例の幅**（用紙 mm。いちばん広いもの。凡例が 1 つも
	// 無ければ 0）。
	//
	// ★**縮尺は用紙いっぱいで決め、凡例は置き場所だけで避ける**（要件）。凡例のために幅を
	// 先取りすると、そのぶん図が 1 段階小さい縮尺へ落ちる——凡例は用紙の隅の空きに置けば
	// 足りるので、図の大きさを削ってまで場所を確保しない。図は左へ寄せ、右上に凡例のぶんの
	// 空きを残す。図が広くて避けきれないときは左端いっぱいへ寄せる（重なりが最も小さくなる
	// 置き方。重なったかどうかは描画側が実測して診断へ残す）。
	//
	// **幅を実測で受け取る理由**は上記（凡例は図面の内容で伸び縮みする）。
	PlanLayout planLayout(const Vec2& content, const PaperArea& page, double legendWidth);

	// 軸組図の割り付け（**上下 2 段**・シートレイヤ 1 枚＝用紙 1 枚）。
	//
	//   scale   … 縮尺の分母（全軸組図で共通）
	//   columns … 1 段に並ぶ枚数（1 以上）
	//   cell    … 1 枚ぶんの大きさ（用紙 mm。間隔は含まない）
	//   area    … 作図可能域（用紙 mm）
	struct SectionLayout
	{
		double scale = 1.0;
		std::size_t columns = 1;
		Vec2 cell;
		PaperArea area;

		// シートレイヤ 1 枚に並ぶ枚数。
		std::size_t perSheet() const
		{
			return columns * kSectionRows;
		}
	};

	// 軸組図の割り付けを決める。content は**軸組図 1 枚ぶん**の広がり（実寸 mm。幅は建物の
	// 平面の広がり、高さは断面の高さ範囲）。**2 段が縦に収まること**を条件に縮尺を選ぶので、
	// 1 段しか置かないときも余白は 2 段ぶんのままになる（用紙をまたいで段の位置が揃う）。
	SectionLayout sectionLayout(const Vec2& content, const PaperArea& page);

	// シート内 index 番目（0 起点。左上から右へ、埋まったら下段へ）のマスの中心（用紙 mm）。
	// 段組み全体は作図域の中央に置く。範囲外の index は最後のマスへ丸める。
	Vec2 sectionSlotCenter(const SectionLayout& layout, std::size_t indexInSheet);

	// viewports 枚の軸組図に要るシートレイヤの枚数（0 枚なら 0）。
	std::size_t sectionSheetCount(const SectionLayout& layout, std::size_t viewports);

	// 軸組図のシートタイトル。1 枚に収まるなら base のまま（"軸組図"）、複数枚に分かれる
	// なら 1 起点の連番を付ける（"軸組図(1)" / "軸組図(2)" …）。
	std::string sectionSheetTitle(const std::string& base, std::size_t page, std::size_t pages);
} // namespace HomeskzIfcImport::core
