//
//	core/Layout.h
//
//	シートレイヤ（用紙）の割り付け——**縮尺の自動調整とビューポートの整列**
//	（docs/DEV-NOTES.md M18）。伏図（draw/Sheet）と軸組図（draw/Section）が「用紙のどこへ・
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
//	  * 伏図は**グラフィック凡例のぶんを差し引いてから**縮尺を決める。用紙いっぱいで
//	    決めてしまうと、建物がギリギリの大きさのときに凡例の置き場所が残らない。
//	    差し引く幅は**描画側が実測した凡例の幅**（planLayout の legendWidth）で、凡例は
//	    図面の内容で伸び縮みするので定数で決め打ちにしない。
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
#include <optional>
#include <string>

namespace HomeskzIfcImport::core
{
	// 使ってよい縮尺の分母。**キリの良い値だけ**を使う（1/200 … 1/5）。昇順＝図が大きく
	// なる順に持ち、fitScale は「収まる中で最初のもの」＝最小の分母を返す。
	inline constexpr std::array<double, 13> kScaleDenominators{
		5.0, 10.0, 15.0, 20.0, 25.0, 30.0, 50.0, 75.0, 100.0, 125.0, 150.0, 175.0, 200.0};

	// **用紙端の余白は定数で持たない**（M18。ローカル確認を経ての結論）。かつては四辺
	// 15mm と決め打ちしていたが、余白は用紙ではなく**印刷の設定**が決めるものなので、
	// 仮定すると実際より狭い（または広い）領域で縮尺を選んでしまう。描画側が
	// シートレイヤから**印刷可能領域そのもの**を読み（draw/DrawUtil の SheetPaperArea。
	// ISDK::GetPageMargins ＋ 用紙の大きさ）、この割り付けへはその矩形を渡す。

	// ビューポート同士・ビューポートと凡例の間隔（用紙 mm）。
	inline constexpr double kViewportGap = 15.0;

	// **凡例の幅は定数で持たない**（M18。実機のローカル確認を経ての結論）。グラフィック凡例
	// の大きさは**その図面に何が並ぶか**で決まる——シンボルの種類が増えれば伸びるし、
	// アンカーボルトを置かない文書では凡例そのものが無い。当初は箱幅の定数（150mm）を
	// そのまま「空けておく幅」にしていたが、実機の凡例は 25mm ほどにしか広がらず、
	// 余らせた 125mm のせいで **1/50 で収まる建物が 1/75 まで落ちて**いた。
	// そこで**描画側が置いた凡例を実測して**（draw/Legend の measureLegendWidth）
	// planLayout へ渡す形にしてある。

	// 用紙の大きさが読めなかったときに使う既定（A3 横。用紙 mm）。**シートレイヤから
	// 読めた値があれば必ずそちらを使う**（draw/DrawUtil の SheetPaperArea）。
	inline constexpr Vec2 kDefaultPaperSize{420.0, 297.0};

	// インチ → mm。用紙まわりの長さは SDK では一貫して**インチで返る**（用紙の大きさ・
	// シートレイヤの大きさ）ので、換算の係数はここに 1 つだけ置く——描画側の実測
	// （draw/DrawUtil の SheetPaperArea）と、下の resolvePageMargins が共有する唯一の定義。
	inline constexpr double kMillimetersPerInch = 25.4;

	// 1 インチの pt 数。文字の大きさ（紙の pt）を mm へ直すのに使う唯一の定義
	// （文字スタイルの大きさを読む draw/DrawUtil・寸法の文字を焼く draw/Dimension・
	// 図面ラベルのタイトル draw/DrawingLabel が共有する）。
	inline constexpr double kPointsPerInch = 72.0;

	// 紙の pt → 紙の mm（10pt → 3.5278mm）。
	constexpr double pointsToMillimeters(double points)
	{
		return points * kMillimetersPerInch / kPointsPerInch;
	}

	// 「用紙 − 余白」とシートレイヤの大きさを突き合わせるときの遊び（用紙 mm）。
	inline constexpr double kPageMarginMatchTol = 0.5;

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

	// 用紙の 4 辺の余白。**解釈前は SDK が返した生の値**（単位が分からない）、解釈後は
	// 用紙 mm（resolvePageMargins）。
	struct PageMargins
	{
		double left = 0.0;
		double right = 0.0;
		double bottom = 0.0;
		double top = 0.0;
	};

	// resolvePageMargins の結果。
	//
	//   margins  … 用紙 mm の余白。解釈できなければ 4 辺とも 0（＝用紙いっぱいを使う）
	//   resolved … 意味のある値として解釈できたか。★**四辺 0 も「解釈できた」**
	//   inInches … 生の値をインチとみなしたか（false なら mm）。四辺 0 のときはどちらでも
	//              同じ値になるので意味を持たない（false のまま）
	struct PageMarginsResolution
	{
		PageMargins margins;
		bool resolved = false;
		bool inInches = false;
	};

	// SDK が返した生の余白（raw）を用紙 mm の余白へ解釈する。paper は用紙の外形の大きさ、
	// sheet はシートレイヤの大きさ（＝印刷可能領域。読めなければ 0 を渡す）で、どちらも
	// 用紙 mm。
	//
	// 【なぜ core に置くか】余白を読むのは SDK の仕事だが、**読めた数字をどう解釈するか**
	// は単位の突き合わせという算数でしかないので、描画側から切り離してここで無 SDK テスト
	// する（CLAUDE.md「描画側から切り離せる純計算」）。
	//
	// ★**GetPageMargins だけ単位がヘッダに書かれていない**（M18。実機では図面の単位で
	// 返った）ので、インチと mm のどちらで返ったかを次の順で決める。
	//   1. インチとみなした値で「用紙 − 余白」がシートレイヤの大きさと一致するなら
	//      **インチ**（用紙まわりの長さは SDK では一貫してインチなので、これが本命）。
	//   2. mm とみなした値で一致するなら **mm**。
	//   3. どちらとも一致しないときは**用紙に収まる方**。両方収まるならインチ（1 の理由）。
	// どれでも決まらなければ resolved = false（描画側は用紙いっぱいで割り付け、生の値を
	// 診断へ出す。draw/Sheet）。
	//
	// ★**四辺 0 は「読めなかった」ではない。** 縁なし印刷ができる機種では**余白 0 の用紙
	// 設定が実際に選べる**ので、0 はそのまま「余白なし＝用紙いっぱいが印刷可能領域」として
	// 受け取る（resolved = true）——ここを「読めなかった」に倒すと、正しい設定に警告が出る
	// （M18 の後で実機から上がった誤判定）。例外は**シートレイヤが用紙より小さい**とき
	// ——余白が在るはずなのに 0 が返ったということなので、そのときだけ解釈できなかった側へ
	// 倒して生の値を診断に出させる。
	PageMarginsResolution resolvePageMargins(const PageMargins& raw, const Vec2& paper,
											 const Vec2& sheet);

	// content（実寸 mm）が available（用紙 mm）に収まる最大の図＝**最小の分母**を階梯から
	// 選ぶ。どれにも収まらなければ最大の分母（＝いちばん小さい図）を返す——**図がはみ出す
	// くらいなら小さく描く**。content・available が退化している（0 以下）ときも同じ。
	double fitScale(const Vec2& content, const Vec2& available);

	// 伏図 1 枚の割り付け。**全シートで同じ値**になる（同じ内容・同じ用紙から計算する）ので、
	// 用紙をめくってもビューポートの位置が変わらない。
	//
	//   scale          … 縮尺の分母（全伏図で共通）
	//   plan           … 図が占めてよい領域（用紙 mm。＝印刷可能領域から凡例のぶんを引いた残り）
	//   viewportCenter … 図の中心を合わせる点（用紙 mm）。**建物の中心**がここへ来る
	//   legendTopRight … グラフィック凡例の右上を合わせる点（用紙 mm。印刷可能領域の右上）
	//
	// plan を持つのは、**描けた図が本当に用紙へ収まったかを描画側が測って確かめられる**
	// ようにするため（M18）。縮尺は「命令セットから求めた建物の広がり」で決めるが、実際に
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
	// area は**印刷可能領域**（用紙 mm。描画側がシートレイヤから読む）、legendWidth は
	// **実際に置いた凡例の幅**（用紙 mm。いちばん広いもの。凡例が 1 つも無ければ 0）。
	//
	// ★**縮尺は凡例の幅を引いてから決める**（要件）。用紙いっぱいで縮尺を決めると、建物が
	// ギリギリの大きさのときに凡例を置くスペースが無くなる——凡例も図面の一部なので、
	// 置けなくなるくらいなら図を 1 段階小さく描く。図は「凡例のぶんを除いた領域」の中央へ
	// 置き、空けた右の帯の右上へ凡例が載る。
	//
	// **幅を実測で受け取る理由**は上記（凡例は図面の内容で伸び縮みするので、定数で
	// 決め打ちにすると余らせたぶんだけ縮尺が落ちる）。
	//
	// **band は寸法の帯**（M31。用紙 mm・四辺それぞれ）。寸法は図の外へ張り出すので、縮尺は
	// 図の領域から四辺の帯を引いた残りで選ぶ（凡例の幅を引くのと同じ考え方）。寸法を
	// 入れなければ 0（＝従来と同じ割り付け）。値は dimensionBand が決める。
	PlanLayout planLayout(const Vec2& content, const PaperArea& area, double legendWidth,
						  double band = 0.0);

	// 図面枠の枠線と、その内側へ並べる図との間隔（用紙 mm）。
	inline constexpr double kTitleBlockInset = 5.0;

	// 図を並べてよい領域を、図面枠の外形（frame）の内側へ絞る。印刷可能領域（printable）と
	// 「枠を kTitleBlockInset だけ内へ寄せた矩形」の重なりを返す。重なりが潰れる（枠が
	// 測り違いで極端に小さい等）ときは printable をそのまま返す——図を並べる場所を失う
	// くらいなら枠と重なる方がよい（重なりは実機で見れば分かる）。
	PaperArea insetFrameArea(const PaperArea& printable, const PaperArea& frame);

	// 測った図面枠の外形が「用紙を囲む枠」か。幅・高さとも印刷可能領域の
	// kTitleBlockMinCoverage 以上あるときだけ true。そうでなければ**枠線を持たない
	// 表題欄だけの図面枠**とみなす（reserveTitleStrip）。
	//
	// 【なぜ分けるか】実機（PR #176 round 1）の図面枠スタイルは**用紙の右下に表題欄の帯が
	// あるだけ**で枠線を持たず、仮に置いて測ると 235 × 19mm が返った。これを「用紙を囲む
	// 枠」として内側へ絞ると並べる領域が 225 × 9mm に潰れ、軸組図が 1/200 まで落ちた。
	bool frameCoversPaper(const PaperArea& frame, const PaperArea& printable);
	inline constexpr double kTitleBlockMinCoverage = 0.5;

	// 枠線の無い表題欄（strip。大きさだけを使う）のぶんを、印刷可能領域の**下**から空ける。
	// 空けるのは「表題欄の高さ＋用紙端からの離れと図との間隔（kTitleBlockInset の 2 倍）」
	// で、幅いっぱいに取る（表題欄が左右のどこにあっても下段の図と重ならない）。
	// **表題欄は下にある**前提（実機のスタイルは右下。一般の図面枠も下か右下に置く）。
	// 空けると潰れるときは printable をそのまま返す。
	PaperArea reserveTitleStrip(const PaperArea& printable, const PaperArea& strip);

	// 軸組図 1 枚の外周に張り出す注釈の帯（用紙 mm・**辺ごと**）。寸法・レベル記号・図面
	// ラベル・通り芯の符号は辺ごとに出るものが違う（左に高さの寸法とレベル記号、下に柱の
	// 位置の寸法と図面ラベル、上に上階の柱・小屋束の位置の寸法と通り芯の符号、右は右端に
	// 根元のある縦の列とレベルの基準線の越えだけ。parse/Dimension・draw/DrawingLabel。
	// 内訳は core::sectionBands の doc コメント）ので、**辺ごとに要るぶんだけ帯を取る**。
	// かつては最も外の段の帯を四辺すべてに取っており、上と右に図 1 枚あたり数十 mm の
	// 空きが出ていた（それが 2 段 × 列の数だけ効いて縮尺を 1〜2 段落としていた）。
	// 値は core::sectionBands が命令から求める。
	struct SectionBands
	{
		double left = 0.0;
		double right = 0.0;
		double bottom = 0.0;
		double top = 0.0;
	};

	// 軸組図の割り付け（**上下 2 段**・シートレイヤ 1 枚＝用紙 1 枚）。
	//
	//   scale          … 縮尺の分母（全軸組図で共通）
	//   columns        … 1 段に並ぶ枚数（1 以上）
	//   cell           … 1 枚ぶんの大きさ（用紙 mm。間隔は含まない。帯を含む）
	//   area           … 図を並べてよい領域（用紙 mm。渡されたものをそのまま持つ）
	//   viewportOffset … マスの中心から**図（ビューポート）の中心**までのずれ（用紙 mm）。
	//                    帯が辺ごとに違うので、図はマスの中央ではなく帯の広い側の反対へ寄る
	//   alignTop       … 段組みを領域の**上端**へ寄せるか（false なら上下の中央）。図面枠を
	//                    置くときは余りを下へ回す（表題欄は下に在ることが多い。draw/Section）
	//   below          … マスの下端から断面の高さ範囲の下端までの空き（用紙 mm。下の帯の
	//                    うち高さ範囲の余白に収まらなかったぶん）。GL を揃える位置
	//                    （sectionGroundY）に使う
	struct SectionLayout
	{
		double scale = 1.0;
		std::size_t columns = 1;
		Vec2 cell;
		PaperArea area;
		Vec2 viewportOffset;
		bool alignTop = false;
		double below = 0.0;

		// シートレイヤ 1 枚に並ぶ枚数。
		std::size_t perSheet() const
		{
			return columns * kSectionRows;
		}
	};

	// 軸組図の割り付けを決める。content は**軸組図 1 枚ぶん**の広がり（実寸 mm。幅は建物の
	// 平面の広がり、高さは断面の高さ範囲）、area は図を並べてよい領域（用紙 mm。印刷可能
	// 領域、図面枠を置くならその内側）。**2 段が縦に収まること**を条件に縮尺を選ぶので、
	// 1 段しか置かないときも余白は 2 段ぶんのままになる（用紙をまたいで段の位置が揃う）。
	//
	// bands は辺ごとの注釈の帯（用紙 mm。SectionBands）。1 枚のマス（cell）は**帯を含めた
	// 大きさ**になる（寸法も隣の図と重ならないように並べる）。
	//
	// heightMargin は content の高さに**上下それぞれ**含まれている空き（実寸 mm。断面の
	// 高さ範囲の余白 core::kSectionHeightMargin）。上下の帯はまずこの空きに収め、はみ出す
	// ぶんだけをマスに足す——図の下の寸法と図面ラベルは実際にこの余白の中に描かれる
	// （docs/DEV-NOTES.md M32）ので、帯と余白を両方取ると同じ場所を 2 度数えることになる。
	// 空きは縮尺で用紙の上の長さが変わるので、**縮尺ごとに**マスを組み直して収まりを見る。
	//
	// alignTop は SectionLayout::alignTop へそのまま写す。
	SectionLayout sectionLayout(const Vec2& content, const PaperArea& area,
								const SectionBands& bands = {}, double heightMargin = 0.0,
								bool alignTop = false);

	// シート内 index 番目（0 起点。左上から右へ、埋まったら下段へ）のマスの中心（用紙 mm）。
	// 段組み全体は領域の左右の中央、上下は中央（alignTop なら上端）に置く。範囲外の index は
	// 最後のマスへ丸める。
	Vec2 sectionSlotCenter(const SectionLayout& layout, std::size_t indexInSheet);

	// そのマスで**図（ビューポート）の中心**を合わせる点（用紙 mm）。マスの中心から
	// viewportOffset だけずらした点で、帯が辺ごとに違っても図と帯がマスにちょうど収まる。
	Vec2 sectionViewportCenter(const SectionLayout& layout, std::size_t indexInSheet);

	// そのマスで**GL（高さ groundZ。注釈空間の y＝絶対 Z）を置く用紙の y**。マスの下端から
	// below（下の帯のはみ出し）と「高さ範囲の下端 rangeStart から GL まで」を上がった高さで、
	// 同じ段のマスでは同じ値になる——全軸組図は同じ高さ範囲・同じ縮尺なので、ここへ GL を
	// 合わせると段ごとに GL が揃う（ご要望）。描画側はビューポートの位置（1025＝GL の
	// 用紙 y。SDK リファレンス Findings「Viewports」#200）をこれへ合わせる。
	double sectionGroundY(const SectionLayout& layout, std::size_t indexInSheet, double rangeStart,
						  double groundZ);

	// viewports 枚の軸組図に要るシートレイヤの枚数（0 枚なら 0）。
	std::size_t sectionSheetCount(const SectionLayout& layout, std::size_t viewports);

	// 軸組図のシートタイトル。1 枚に収まるなら base のまま（"軸組図"）、複数枚に分かれる
	// なら 1 起点の連番を付ける（"軸組図(1)" / "軸組図(2)" …）。
	std::string sectionSheetTitle(const std::string& base, std::size_t page, std::size_t pages);
	// ------------------------------------------------------------------------
	// 寸法線の位置（M31）
	// ------------------------------------------------------------------------

	// 寸法線を補助線の根元（base）から離す長さ（**用紙 mm**）。1 段目までの距離と、
	// 段と段の間隔。用紙の上の長さで持つのは、どの縮尺でも段の間隔が同じに見えるように
	// するため（命令は段の番号だけを持つ。core/Document.h の DimensionChainCommand）。
	// 値は寸法の文字（おおむね 2.5mm 前後）が 1 段に収まり、隣の段と重ならない大きさの
	// 見込みで、実機で見て詰める。
	inline constexpr double kDimensionFirstGap = 8.0;
	inline constexpr double kDimensionTierPitch = 7.0;

	// 寸法線の直交座標（注釈空間・モデル mm）。base から side の向きへ
	// （kDimensionFirstGap + tier × kDimensionTierPitch）× 縮尺の分母 だけ離す。
	// side は ±1（それ以外は符号だけを見る）、tier は 0 以上（負は 0 とみなす）。
	double dimensionLineCoord(double base, int side, int tier, double scale);

	// 寸法の文字が寸法線から外へはみ出す見込み（用紙 mm）。寸法の文字（おおむね 2〜3mm）と
	// 寸法線との隙間のぶん。
	inline constexpr double kDimensionTextAllowance = 4.0;

	// 寸法の帯（用紙 mm）＝最も外の段の寸法線までの距離＋文字の見込み。outermostTier は
	// 図の外周に出る段のうち最も外のもの（core::outermostDimensionTier）。負なら（寸法が
	// 無ければ）0。
	double dimensionBand(int outermostTier);

	// ------------------------------------------------------------------------
	// 軸組図のレベル記号の形と位置（M31 の後のご要望）
	// ------------------------------------------------------------------------
	//
	// 記号は**基準線の起点から右へ**「▽（頂点で線に触れる正三角形）＋名前」を並べ、線は
	// 図の右端を少し越えるまで伸ばす。
	//
	//     ▽1FL ─────────────────────────（図）─────
	//     ↑起点（左の寸法列の文字より外）          ↑右端＋kLevelLineOvershoot
	//
	// 長さは**用紙 mm**（マーカーレイアウトの中身は紙の上の mm で効き、容れ物の縮尺は VW が
	// 掛ける。Findings「Drawing Labels」のレイアウトの文字の大きさ）。名前の文字の大きさは
	// 寸法の文字と同じ（寸法規格の文字スタイルの pt。draw/Dimension が読む）。

	// 三角の高さ／名前の文字の大きさ（「文字より一回り小さい」。ご要望）。
	inline constexpr double kLevelMarkTriangleRatio = 0.8;
	// 三角と名前の隙間・線と名前の下端の隙間（用紙 mm）。
	inline constexpr double kLevelMarkTextGap = 0.5;
	// 名前の右端と寸法の文字の隙間（用紙 mm）。
	inline constexpr double kLevelMarkClearance = 1.0;
	// 基準線が図の右端を越える長さ（用紙 mm。「建物幅を少し超えたくらい」。ご要望）。
	inline constexpr double kLevelLineOvershoot = 3.0;
	// 軸組図の寸法の帯に足す、レベル記号が寸法より外へ張り出す見込み（用紙 mm）。名前の幅は
	// 描くまで分からないので、三角と 3 文字ほどの名前（"1FL"・"軒高"）が収まる量で見込む。
	inline constexpr double kLevelMarkBandAllowance = 10.0;

	// 軸組図の下の帯に足す、図面ラベル（紙の 10pt のタイトル＋下線）の高さの見込み（用紙 mm。
	// docs/DEV-NOTES.md M32 で「用紙 5mm 前後」）。ラベルの上の間隔は core::kSectionLabelGap。
	// 当初 6mm としたが、実機（PR #176 round 1・1/200）で下の注釈が見込みより 2.4mm 深く、
	// 19 枚がマスから縦にはみ出したので 9mm にした。
	inline constexpr double kSectionLabelAllowance = 9.0;

	// 軸組図の上の帯に取る、通り芯の符号（円）が建物の上へ出る見込み（用紙 mm）。符号は
	// 用紙基準で、実機（PR #176 round 1）の画面で直径 8mm ほど。円と少しの隙間のぶん。
	// 上の帯は高さ範囲の余白（1/100 で用紙 10mm）に先に収めるので、1/100 以上の大きい図では
	// マスを広げない。
	inline constexpr double kSectionGridBubbleAllowance = 10.0;

	// 断面ビューポートの注釈に VW が出すグリッド線（通り芯）の符号（ラベル枠）の高さと、
	// 符号の下端と上の寸法の文字との隙間（どちらも用紙 mm）。符号は「映っているモデルの
	// 上端 ＋ 水平線の長さ（先端）＋ ラベル枠」に描かれ、既定（水平線 5mm）で上端から
	// 12.35〜12.45mm（1/100・1/50 の実測。SDK リファレンス Findings「Viewports」#189）
	// なので、ラベル枠は 7.4mm 前後。少し大きめに見込む。上の帯（kSectionGridBubbleAllowance）
	// は「隙間＋ラベル枠」を覆う。
	inline constexpr double kGridBubbleHeight = 7.5;
	inline constexpr double kGridBubbleClearance = 1.0;

	// 断面の注釈のグリッド線の「水平線の長さ（先端）」（ShoulderLengthAtStart。用紙 mm）を、
	// 符号の下端が上の寸法の文字（dimensionTop）より kGridBubbleClearance 上に来るように
	// 決める。shoulder はいまの値、gridTop はいまの符号の上端（注釈空間の y・モデル mm。
	// 測ったグリッド線の外接の上端）、dimensionTop は上の寸法の文字の上端（注釈空間の y。
	// sectionTopDimensionReach）、scale は縮尺の分母。**下げはしない**（既に上にあれば
	// shoulder のまま）。scale が 0 以下なら shoulder のまま。
	double gridShoulderAboveDimensions(double shoulder, double gridTop, double dimensionTop,
									   double scale);

	// 記号のレイアウトの中の配置（用紙 mm・起点＝(0, 0)・y は上が +）。
	//   triangleHeight / triangleHalfWidth … ▽ の高さと底辺（上辺）の半分。頂点は
	//                                        (triangleHalfWidth, 0)
	//   textLeft / textBottom              … 名前の外形の左下
	//   width                              … 起点から名前の右端まで
	struct LevelMarkShape
	{
		double triangleHeight = 0.0;
		double triangleHalfWidth = 0.0;
		double textLeft = 0.0;
		double textBottom = 0.0;
		double width = 0.0;
	};

	// textSize は名前の文字の大きさ、textWidth は描いた名前の幅（どちらも用紙 mm。負は 0）。
	LevelMarkShape levelMarkShape(double textSize, double textWidth);

	// 記号の起点（注釈空間の x・モデル mm）。left は図の左端（高さの寸法列の根元）、
	// dimensionTier は左に出る寸法列の最も外の段（無ければ負）、markWidth は
	// LevelMarkShape::width。dimensionScale は寸法線までの距離に使う縮尺の分母
	// （dimensionLineCoord と同じもの）、markScale は記号を描くビューポートの縮尺の分母。
	// 名前の右端が寸法の文字（寸法線から kDimensionTextAllowance）より kLevelMarkClearance
	// だけ外に来る位置を返す。
	double levelMarkStartX(double left, int dimensionTier, double markWidth, double dimensionScale,
						   double markScale);

	// 基準線の長さ（用紙 mm）。起点 startX から図の右端 right を kLevelLineOvershoot だけ
	// 越えるまで。scale は記号を描くビューポートの縮尺の分母。
	double levelLineLength(double startX, double right, double scale);

	// --- 回転して置いた注釈（データタグ）の大きさ ----------------------------
	//
	// 回転して置いた矩形（データタグ）の**自身の高さ**（文字の向きに直交する差し渡し）を、
	// 実測できる外接矩形（軸に平行。GetObjectBounds）の幅・高さと回転角から戻す。
	//
	// 【なぜ要るか】タグは部材の辺に下端を接させたいので、辺から「タグ自身の高さの半分」だけ
	// 法線の向きへ逃がす（draw/Tag）。水平・鉛直のタグなら外接矩形の高さ／幅がそのまま
	// タグの高さだが、**傾斜材（登り梁・隅木）のタグは傾いて置く**ので外接矩形の高さには文字の
	// 長さの sin 成分が混ざり、逃がし量が文字の長さに比例して膨らむ——軸組図で傾斜材の
	// 断面寸法が材から大きく離れて表示された原因（docs/DEV-NOTES.md M13）。
	//
	// 【解き方】自身の長さ l・高さ h の矩形を角度 θ で置くと、外接矩形は
	//   W = l·|cosθ| + h·|sinθ|、H = l·|sinθ| + h·|cosθ|
	// なので h = (H·|cosθ| − W·|sinθ|) / (cos²θ − sin²θ)。**45 度の近くでは分母が 0 へ
	// 寄って解けない**（W と H が l と h の和しか語らない）ので、そのときは nullopt を返し、
	// 呼び出し側が同じ図のほかのタグから高さを借りる（タグはどれも同じレイアウトの 1 行なので
	// 高さは揃う）。
	//
	// angleDegrees は回転角（度）、boundsWidth / boundsHeight は外接矩形の幅・高さ（負は 0）。
	// 解けないとき・解いた高さが 0 以下のときは nullopt。
	std::optional<double> rotatedRectHeight(double angleDegrees, double boundsWidth,
											double boundsHeight);

	// rotatedRectHeight が「解けない」とみなす |cos²θ − sin²θ|（＝|cos2θ|）の下限。
	// 0.25 は 45 度から ±7.2 度ほど。実測の誤差がこの倍率（1/0.25＝4 倍）までで収まる範囲に
	// 留める。
	inline constexpr double kRotatedRectMinConditioning = 0.25;

} // namespace HomeskzIfcImport::core
