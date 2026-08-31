//
//	draw/Legend.cpp
//
//	グラフィック凡例の設置の実装。意図・規約は draw/Legend.h と core/Document.h の
//	LegendCommand を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API:
//	  * gSDK->DefineCustomObject("GraphicLegend", kCustomObjectPrefNever) … 設定ダイアログ抑止
//	  * gSDK->CreateCustomObject("GraphicLegend", 位置, 0, bInsert)      … 凡例 PIO の生成
//	  * gSDK->SetCurrentLayer(sheetLayer)                                … 置き場所（用紙）の指定
//	  * gSDK->SetLineWeight / SetFillPat                                 … 見た目（線の太さ・塗り）
//	  * gSDK->GetObjectInternalIndex(viewport)                           … フィルタ先の参照
//	  * gSDK->TaggedDataCreate / TaggedDataSet          … フィルタとソース定義の書き込み
//	  * gSDK->ResetObject                                                … 反映（中身の計算）
//	  * gSDK->FirstMemberObj / NextObject / GetObjectTypeN … 凡例の中の「凡例イメージ」探し
//	  * VWViewportObj::GetScale / ApplyViewportScale       … 凡例イメージ（＝ビューポート）の縮尺
//	  * gSDK->Get/SetWorksheetImageScaleFactor            … 版が変わって型 56 が現れたとき用
//
//	**スタイルは扱わない**（draw/Legend.h の ★）。`SetPluginObjectStyle` も
//	`UpdateStyledObjects` も呼ばない——スタイル無しで置くので、中身は `ResetObject` の時点で
//	決まる。
//

#include "PluginPrefix.h"
#include "draw/Legend.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <vector>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// グラフィック凡例の内部プラグイン名。
		// **表示名「グラフィック凡例」とは別物**で、登録名はスペース無しの "GraphicLegend"。
		constexpr const char* kGraphicLegendPlugin = "GraphicLegend";

		// 箱幅パラメータ。凡例は矩形モードの PIO なので、点で生成すると幅 0 のまま潰れる
		// （draw/Legend.h）。ここで与えるのは**生成時の箱の幅**で、用紙をどれだけ空けるかは
		// これではなく**置いた後の実測**が決める（measureLegendWidth）——凡例は図面の内容で
		// 伸び縮みするので、決め打ちの幅を割り付けに使わない。高さは行の内容から自動で
		// 決まるので与えない。
		constexpr const char* kFieldBoxWidth = "BoxWidth";
		// ★**要求した幅がそのまま図の取り分を減らす。** 用紙に空ける幅は実測で決まるので
		// （measureLegendWidth）、ここで広く頼むほど伏図の縮尺が落ちる。実機では並ぶ
		// シンボルが 25mm ほどしか使っておらず、60mm を頼んでいたときは 1/50 に 330mm 要る
		// 建物に対して使える幅が 315mm しか残らず 1/75 へ落ちていた（M18 のローカル確認）。
		// **中身が必要とする幅より少し広い程度**に留める。
		constexpr double kBoxWidth = 40.0;

		// **凡例イメージはビューポート**（`Kernel/API/Objs.TDType.h` の `kViewportNode` ＝ 122）。
		// 実機で凡例の中身の節点型を数えて分かった——`0×102, 3×66, 10×6, 11×36, 86×42,
		// 122×12`、すなわち枠（kBoxNode）・文字（kTextNode）・グループ・PIO と、**凡例 1 つに
		// つき 1 つのビューポート**。`ovGraphicLegendViewType` / `BgRenderMode` /
		// `FgRenderMode`（ObjectVariables.h）がビューポートの属性そのものなのとも符合する。
		// したがって縮率＝**そのビューポートの縮尺**で、伏図のビューポートと同じ
		// `ApplyViewportScale` で与えられる（docs/DEV-NOTES.md「グラフィック凡例」）。
		constexpr short kViewportNode = 122;

		// ワークシートのイメージ（`kWorksheetImageNode` ＝ 56）。**実機の凡例には 1 つも
		// 入っていなかった**が、`Get/SetWorksheetImageScaleFactor` という縮率の API がこの型に
		// 対してだけ用意されているので、版が変われば現れる余地がある。見つけたら扱う。
		constexpr short kWorksheetImageNode = 56;

		// 凡例の中を辿る深さの上限。凡例 → セル → 枠・イメージ・文字、と数段で足りるが、
		// 万一の循環で回り続けないよう歯止めを置く。
		constexpr int kImageSearchDepth = 8;

		// 見た目。凡例 PIO が内部で描く枠線・セルは**クラスでは制御できない**ので、
		// オブジェクトの属性として直接与える（draw/Legend.h）。線の太さの単位はミル（1/1000
		// インチ）で、5 ミル = 0.127mm を VW は 0.13mm と表示する。塗りパターン 0 = なし。
		constexpr short kLineWeightMils = 5;
		constexpr InternalIndex kFillNone = 0;

		// 生成した凡例の箱幅を与える。**例外を外へ出さない**——書けなくても凡例そのものは
		// 図面に残るので、件数だけ counts へ積む。
		//
		// **イメージの縮率はここでは書かない**——レコードの `ImageScale` は書けるのに実描画が
		// 1:50 から動かないので、凡例イメージそのものへ与える（applyLegendImageScale）。
		// 凡例イメージは中身を流し込んで初めて作られるので、置き終えた後でなければ届かない。
		void ApplyBoxWidth(MCObjectHandle object, LegendCounts& counts)
		{
			try
			{
				VWParametricObj pio(object);
				if (!SetParamRealChecked(pio, TXString(kFieldBoxWidth), kBoxWidth))
					++counts.widthLeft;
			}
			catch (...)
			{
				// PIO として開けなかった（＝箱幅を書けていない）。幅 0 のまま潰れるだけで
				// 凡例自体は図面に残るので、続ける。
				++counts.paramsFailed;
			}
		}

		// ビューポートの縮尺。読めなければ 0（診断へ出すだけなので、読めないことは咎めない）。
		double ViewportScale(MCObjectHandle viewport)
		{
			try
			{
				return VWViewportObj(viewport).GetScale();
			}
			catch (...)
			{
				return 0.0;
			}
		}

		// 診断へ持ち帰るのは**最初に見つけた 1 つ**の前後だけ（全部持っても同じ値が並ぶだけで、
		// 効き目は 1 組あれば分かる）。
		void RecordFirstScale(LegendCounts& counts, double before, double target, double after)
		{
			if (counts.imageScaleTarget != 0.0)
				return;
			counts.imageScaleBefore = before;
			counts.imageScaleTarget = target;
			counts.imageScaleAfter = after;
		}

		// container の中のビューポートに scale の縮尺のものが 1 つでもあるか。
		// **作り直しの後に読み直す**ためのもので、図面は変えない。
		//
		// **「最初の 1 つ」を見てはいけない。** 凡例の中にはビューポートが複数あり、凡例
		// イメージ以外を拾うと「戻された」と誤報する（実機で 1 度出した——描画は伏図の縮尺に
		// なっているのに診断は「50 → 75 と書いて 50」だった）。**どれか 1 つでも目標の縮尺で
		// あればよい**ので、全部見て 1 つ当たれば true。
		bool HasViewportAtScale(MCObjectHandle container, double scale, int depth)
		{
			if (container == nil || depth <= 0)
				return false;
			for (MCObjectHandle h = gSDK->FirstMemberObj(container); h != nil;
				 h = gSDK->NextObject(h))
			{
				if (gSDK->GetObjectTypeN(h) == kViewportNode)
				{
					if (std::abs(ViewportScale(h) - scale) <= std::abs(scale) * 1.0e-6)
						return true;
					continue; // ビューポートの中へは潜らない（ScaleImagesIn と同じ理由）
				}
				if (HasViewportAtScale(h, scale, depth - 1))
					return true;
			}
			return false;
		}

		// container の中を辿って凡例イメージ（kViewportNode / kWorksheetImageNode）に縮率を与え、内訳を
		// counts へ積む。**入れ子を辿るのは、凡例イメージがセルの中にある**ため——凡例 →
		// セル → イメージという段数は VW の版で変わりうるので、型で見つけるまで潜る
		// （depth が歯止め）。
		void ScaleImagesIn(MCObjectHandle container, double scale, int depth, LegendCounts& counts)
		{
			if (container == nil || depth <= 0)
				return;

			for (MCObjectHandle h = gSDK->FirstMemberObj(container); h != nil;
				 h = gSDK->NextObject(h))
			{
				// **見えた型は全部数える。** 凡例イメージが 1 つも無かったときに、次に何を
				// 探せばよいかを実機から持ち帰る唯一の手掛かりになる（draw/Legend.h）。
				const short type = gSDK->GetObjectTypeN(h);
				++counts.memberTypes[type];

				if (type == kViewportNode)
				{
					// **中へは潜らない。** ビューポートの中身はそれが映している図そのもので、
					// 潜ると設計レイヤ全体を歩くことになる（1 度やって、数え上げが
					// `86×42` などと膨らんだ）。
					const double before = ViewportScale(h);
					if (ApplyViewportScale(h, scale))
						++counts.imagesScaled;
					else
						++counts.imagesLeft;
					RecordFirstScale(counts, before, scale, ViewportScale(h));
					continue;
				}

				if (type != kWorksheetImageNode)
				{
					ScaleImagesIn(h, scale, depth - 1, counts);
					continue;
				}

				// **単位の取り決めが SDK ヘッダに無いので、いま入っている値から決める**
				// （draw/Legend.h）。1 以上なら「分母」（1:50 なら 50）、1 未満なら「倍率」
				// （0.02）と見なす。既定が 1:50 なのでどちらでも判別がつく。
				const double before = gSDK->GetWorksheetImageScaleFactor(h);
				const double target = (before >= 1.0) ? scale : 1.0 / scale;
				if (gSDK->SetWorksheetImageScaleFactor(h, target, true))
					++counts.imagesScaled;
				else
					++counts.imagesLeft;
				RecordFirstScale(counts, before, target, gSDK->GetWorksheetImageScaleFactor(h));
			}
		}

		// 「ビューポートでフィルタ」の保存先（draw/Legend.h 冒頭・docs/DEV-NOTES.md）。
		//
		// **`'GrLg'` を文字リテラルで書かない**のは、多文字リテラルが処理系定義で警告の
		// 対象になるため。値は 'G'=0x47 / 'r'=0x72 / 'L'=0x4C / 'g'=0x67 を並べたもので、
		// 実機のダンプではデータオブジェクトの中身の +86 にこの並びで入っていた。
		constexpr OSType kFilterContainer = 0x47724C67; // 'GrLg'（Graphic Legend）

		// オブジェクト参照の配列（Kernel/API/MiniCadCallBacks.h の
		// kTaggedDataObjectRefArrayTypeID）と、その中でフィルタが使うタグ。
		constexpr Sint32 kFilterDataType = 15;
		constexpr Sint32 kFilterDataTag = 5;
		constexpr Sint32 kFilterElementCount = 1;

		// 凡例を「そのシートのビューポートに映っているものだけ」に絞る。書けたら true。
		//
		// **UI の「ビューポートでフィルタ…」と同じ状態**を作る。VW はこれを凡例にぶら下がる
		// データオブジェクトのタグ付きデータとして持っており、SDK からは
		// TaggedDataCreate ＋ TaggedDataSet で書ける（読み書きの API はあるが、**凡例の
		// フィルタ専用の呼び出しは SDK にも VectorScript にも無い**——容れ物と型とタグは
		// 実機のバイト列から突き止めた。docs/DEV-NOTES.md「グラフィック凡例」）。
		bool ApplyViewportFilter(MCObjectHandle legend, MCObjectHandle viewport)
		{
			if (legend == nil || viewport == nil)
				return false;

			// フィルタ先は**オブジェクトの内部参照**で持つ。0 は「参照が無い」なので、
			// そのまま書くと「フィルタ無し」と区別が付かない状態を作ってしまう。
			const InternalIndex reference = gSDK->GetObjectInternalIndex(viewport);
			if (reference == 0)
				return false;

			if (!gSDK->TaggedDataCreate(legend, kFilterContainer, kFilterDataType, kFilterDataTag,
										kFilterElementCount))
				return false;
			return gSDK->TaggedDataSet(legend, kFilterContainer, kFilterDataType, kFilterDataTag, 0,
									   &reference) != 0;
		}

		// ソース定義（凡例に何を並べるか）の保存先。フィルタの `'GrLg'` と 1 文字違いの
		// **`'GrLe'`**（'G'=0x47 / 'r'=0x72 / 'L'=0x4C / 'e'=0x65）で、実機のダンプでは
		// **「凡例ソースの定義...」を手で設定した凡例にだけ**ぶら下がっていた。
		constexpr OSType kSourceContainer = 0x47724C65; // 'GrLe'（Graphic Legend の別の容れ物）

		// byte 配列（kTaggedDataByteArrayTypeID = 1）のタグ 0。**16 ビット幅の値も byte 配列に
		// 載る**——タグ付きデータの型は 6 種類しか無く（byte / uint32 / double / matrix /
		// colorref / objectref）、VWFC の `CTaggedDataContainer::CreateTagUint16` も byte 配列を
		// 使う。したがってこの 22 バイトは 11 個の 16 ビット値と読める。
		constexpr Sint32 kSourceDataType = 1;
		constexpr Sint32 kSourceDataTag = 0;

		// **手で設定した凡例からそのまま写した 22 バイト**（実機のダンプ）。検索条件は文字列
		// ではなく **11 個の 16 ビット値のトークン列**で保存されていて（`(INVIEWPORT &
		// (T=SYMBOL))` は ASCII で 25 文字あり、そもそも 22 バイトに入らない）、条件だけが
		// 違う 4 枚を突き合わせると
		// 666 / 662 / 798 / 662 / 777 / **型** / 1607 / 1637 / 1615 / 677 / 1680 と並んで
		// **6 番目だけが変わった**（`T=SYMBOL` の 3 枚が 15、`T=PLUGINOBJECT` の 1 枚が 86）。
		// つまり残りの 10 個は `( INVIEWPORT & ( T = … ) )` の器で、**6 番目にオブジェクトの
		// 型番号を入れれば条件を差し替えられる**（下記 kCriteriaObjectType）。
		//
		// **これは実験である。** 既定のソースが空で、スタイルを当てないと凡例が何も表示しない
		// （実機で確認）以上、ソース定義を per-instance で書き込む以外に道が無い。器の 10 個の
		// 意味は解けていないので、文書や VW の版をまたいで通用するかは**確かめられていない**。
		// 実機で「並ぶかどうか」を見て判断する（docs/DEV-NOTES.md「グラフィック凡例」）。
		constexpr std::array<Uint8, 22> kSourceDefinition{
			0x9a, 0x02, 0x96, 0x02, 0x1e, 0x03, 0x96, 0x02, 0x09, 0x03, 0x00,
			0x00, 0x47, 0x06, 0x65, 0x06, 0x4f, 0x06, 0xa5, 0x02, 0x90, 0x06};

		// 条件が指すオブジェクトの型（`T=…`）と、それが入る位置（6 番目の 16 ビット値＝
		// 先頭から 10 バイト目）。**15 はシンボル**で、凡例を載せる基礎伏図に並べたいアンカー
		// ボルトがこれ。構造材ツールの部材を並べたくなったら **86（プラグインオブジェクト）**
		// にする——実機のダンプでその 1 枚が 86 で、ダンプ上の凡例自身も `type=86` だった。
		constexpr std::size_t kCriteriaTypeOffset = 10;
		constexpr Uint16 kCriteriaObjectType = 15;

		// ソース定義を書き込む（書けたら true）。フィルタと同じく**`ResetObject` より前**に
		// 済ませる（凡例の作り直しでセルが決まるため）。
		bool ApplySourceDefinition(MCObjectHandle legend)
		{
			if (legend == nil)
				return false;
			if (!gSDK->TaggedDataCreate(legend, kSourceContainer, kSourceDataType, kSourceDataTag,
										static_cast<Sint32>(kSourceDefinition.size())))
				return false;

			for (std::size_t i = 0; i < kSourceDefinition.size(); ++i)
			{
				// 6 番目の 16 ビット値だけは、条件が指す型（リトルエンディアン）で埋める。
				Uint8 value = kSourceDefinition[i];
				if (i == kCriteriaTypeOffset)
					value = static_cast<Uint8>(kCriteriaObjectType & 0xFFU);
				else if (i == kCriteriaTypeOffset + 1)
					value = static_cast<Uint8>((kCriteriaObjectType >> 8U) & 0xFFU);
				if (gSDK->TaggedDataSet(legend, kSourceContainer, kSourceDataType, kSourceDataTag,
										static_cast<Sint32>(i), &value) == 0)
					return false;
			}
			return true;
		}
	} // namespace

	void prepareGraphicLegendPlugin()
	{
		gSDK->DefineCustomObject(TXString(kGraphicLegendPlugin), kCustomObjectPrefNever);
	}

	bool drawSheetLegend(MCObjectHandle sheetLayer, const core::Vec2& where,
						 MCObjectHandle filterViewport, LegendCounts& counts)
	{
		if (sheetLayer == nil)
		{
			++counts.failed;
			return false;
		}

		// 凡例は**シートレイヤの上**に置く（用紙に載る）。PIO は bInsert=true でカレント
		// レイヤへ入るので、先にそのシートレイヤをアクティブにする。
		gSDK->SetCurrentLayer(sheetLayer);

		// 生成位置は仮（右上に合わせるのは中身が流し込まれて大きさが定まってから＝
		// placeLegends）。
		const MCObjectHandle object = gSDK->CreateCustomObject(
			TXString(kGraphicLegendPlugin), WorldPt(where.x, where.y), 0.0, true);
		if (object == nil)
		{
			++counts.failed;
			return false;
		}

		// 箱幅。**スタイルは当てない**（draw/Legend.h の ★）ので、凡例の姿を決めるのは
		// このオブジェクト自身の設定だけになる。
		ApplyBoxWidth(object, counts);

		// ソース定義（何を並べるか）。**既定のソースは空**なので、これを書かないと
		// スタイル無しの凡例は 1 セットも表示しない（実機で確認）。
		if (ApplySourceDefinition(object))
			++counts.sourced;
		else
			++counts.sourceLeft;

		// そのシートのビューポートで絞る（**ResetObject より前**——凡例の作り直しで
		// 並ぶセルが決まるため）。書けなくても凡例自体は残るので、件数だけ持ち帰って続ける
		// （文書中の全シンボルが並ぶ状態になる）。
		if (ApplyViewportFilter(object, filterViewport))
			++counts.filtered;
		else
			++counts.filterLeft;

		gSDK->ResetObject(object);

		// 見た目はクラスでは効かないのでオブジェクトの属性として直接与える。**ResetObject
		// の後**に置くと by-instance の属性として保たれる。
		gSDK->SetLineWeight(object, kLineWeightMils);
		gSDK->SetFillPat(object, kFillNone);

		// 位置合わせのために覚えておく（中身を流し込んだ後に placeLegends が動かす）。
		counts.objects.push_back(object);
		++counts.drawn;
		return true;
	}

	void refreshLegends(const LegendCounts& counts)
	{
		// **スタイルは使わない**ので、中身を決めるのは各オブジェクトに書き込んだソース定義と
		// ビューポートのフィルタ（draw/Legend.h の ★）。作り直せばその時点の図の状態で
		// セルが集まり直す。by-instance の箱幅・線の太さ・塗りは保たれる。
		for (const MCObjectHandle object : counts.objects)
			gSDK->ResetObject(object);
	}

	double measureLegendWidth(const LegendCounts& counts)
	{
		// **いちばん広いもの**を採る。伏図は全図が同じ位置・同じ縮尺なので、空ける幅は
		// どのシートの凡例も収まる幅でなければならない（core::planLayout）。
		double widest = 0.0;
		for (const MCObjectHandle object : counts.objects)
		{
			if (WorldRect bounds; gSDK->GetObjectBounds(object, bounds))
				widest = std::max(widest, std::abs(bounds.right - bounds.left));
		}
		return widest;
	}

	void placeLegends(const LegendCounts& counts, const core::Vec2& topRight)
	{
		for (const MCObjectHandle object : counts.objects)
		{
			if (WorldRect bounds; gSDK->GetObjectBounds(object, bounds))
				gSDK->MoveObject(object, topRight.x - bounds.right, topRight.y - bounds.top);
		}
	}

	void applyLegendImageScale(LegendCounts& counts, double scale)
	{
		if (scale <= 0.0)
			return;

		// ★**順序**: 中身 → 作り直し → **中身をもう一度**。以降は作り直さない。
		//
		// 実機で各段階を測って確定した:
		//
		//   開始   中身=[50,50,50]
		//   中身   中身=[75,75,75]   ← 凡例イメージ（ビューポート）の縮尺は書ける
		//   作直   中身=[50,50,50]   ← **作り直しが中身を既定へ戻す**。ただし枠はこのとき
		//                              「その時点の中身」から組み直るので 75 で組まれる
		//   完了   中身=[75,75,75]   ← 戻された中身を入れ直す
		//
		// **最後に作り直さない**ことが肝で、作り直した瞬間に中身が 50 へ戻る。
		//
		// **レコードの `ImageScale`（OIP の「イメージの縮率」）は書かない。** 書いても絵は
		// 変わらず、OIP の表示も変わらない——縮率の本体はレコードの外にあり、UI からの書き込み
		// だけがそこへ届く（docs/DEV-NOTES.md「打ち切った調査」の
		// 「凡例の OIP『イメージの縮率』表示を合わせる」）。実数でも文字列でも結果は同じ。
		//
		// 凡例イメージ（＝ビューポート）へ縮尺を与える。
		//
		// **入口を 1 つに絞らない。** PIO の中身の入口は版によって 3 通りあり（draw/Tag の
		// HeldTagLayout が同じ理由で 2 つ見ている）、加えて凡例は補助オブジェクトを何本も
		// ぶら下げている。どこに凡例イメージが居るか分からないうちは、辿れる先を全部辿る。
		const auto scaleImages = [&counts, scale](MCObjectHandle object)
		{
			ScaleImagesIn(object, scale, kImageSearchDepth, counts);
			ScaleImagesIn(gSDK->GetCustomObjectProfileGroup(object), scale, kImageSearchDepth,
						  counts);
			ScaleImagesIn(gSDK->GetCustomObjectProfileGroupInAux(object), scale, kImageSearchDepth,
						  counts);
			for (MCObjectHandle aux = gSDK->FirstAuxObject(object); aux != nil;
				 aux = gSDK->NextObject(aux))
				ScaleImagesIn(aux, scale, kImageSearchDepth, counts);
		};

		// 1) 中身を伏図の縮尺にする。
		for (const MCObjectHandle object : counts.objects)
			scaleImages(object);
		// 2) 作り直して、枠を「いまの中身」に合わせて組み直させる。**この作り直しで中身は
		//    既定へ戻る**が、枠は 75 で組まれた後なので崩れない。
		//    **縮率を 1 つも与えられていないなら作り直さない。**
		if (counts.imagesScaled == 0)
			return;
		for (const MCObjectHandle object : counts.objects)
			gSDK->ResetObject(object);
		// 4) 作り直しで戻された中身を入れ直す。**いちばん最後。ここから先は作り直さない。**
		for (const MCObjectHandle object : counts.objects)
			scaleImages(object);

		// **作り直しが縮尺を戻していないかを見る。** 戻っていれば枠も中身も既定のままで、
		// 絵からは「何も変わらなかった」としか見えないので、診断で見分けられるようにする。
		// 目標の縮尺のビューポートが**どの凡例にも 1 つも無い**ときだけ異常とみなす。
		counts.imageScaleAfter = 0.0;
		for (const MCObjectHandle object : counts.objects)
		{
			if (HasViewportAtScale(object, scale, kImageSearchDepth))
			{
				counts.imageScaleAfter = scale;
				break;
			}
		}
	}

	std::string legendDiagnostics(const LegendCounts& counts)
	{
		// **縮率は異常のときだけ知らせる。** 効いていれば図を見れば分かるが、「凡例イメージが
		// 1 つも見つからない」「書いたのに読み戻しが違う」は絵からは分からない
		// （draw/Legend.h の applyLegendImageScale）。
		const bool imageScaleLeft = counts.drawn > 0 && counts.imagesScaled == 0;
		// 縮尺を与えたのに、作り直しの後にその縮尺のイメージが 1 つも残っていない
		// ＝作り直しが既定へ戻した（draw/Legend.h の applyLegendImageScale）。
		const bool imageScaleReverted =
			counts.imageScaleTarget != 0.0 && counts.imageScaleAfter == 0.0;
		if (counts.failed == 0 && counts.widthLeft == 0 && counts.paramsFailed == 0 &&
			counts.sourceLeft == 0 && counts.filterLeft == 0 && !imageScaleLeft &&
			counts.imagesLeft == 0 && !imageScaleReverted)
			return {};

		std::string text = "伏図のグラフィック凡例の診断: ";
		if (counts.failed > 0)
			text += "凡例を置けなかった命令 " + std::to_string(counts.failed) + " 件。";
		if (counts.paramsFailed > 0)
			text += "パラメータを書けなかった凡例 " + std::to_string(counts.paramsFailed) +
					" 件（幅 0 に潰れます）。";
		if (counts.widthLeft > 0)
			text += "箱幅を設定できなかった凡例 " + std::to_string(counts.widthLeft) +
					" 件（幅 0 に潰れます）。";
		if (counts.sourceLeft > 0)
			text += "ソース定義を書けなかった凡例 " + std::to_string(counts.sourceLeft) +
					" 件（何も並びません）。";
		if (counts.filterLeft > 0)
			text += "ビューポートで絞れなかった凡例 " + std::to_string(counts.filterLeft) +
					" 件（その図に無いシンボルも並びます）。";
		if (imageScaleLeft)
		{
			text += "凡例イメージが見つからず、イメージの縮率は既定（1:50）のままです";
			if (counts.memberTypes.empty())
			{
				text += "（凡例の中身を 1 つも辿れませんでした）。";
			}
			else
			{
				// 型と件数をそのまま並べる。**多くても十数種類**なので全部出してよい。
				text += "（凡例の中で見えた節点型: ";
				bool first = true;
				for (const auto& [type, count] : counts.memberTypes)
				{
					if (!first)
						text += ", ";
					first = false;
					text += std::to_string(type) + "×" + std::to_string(count);
				}
				text += "）。";
			}
		}
		if (counts.imagesLeft > 0)
			text += "縮率を書けなかった凡例イメージ " + std::to_string(counts.imagesLeft) +
					" 件（そのイメージだけ既定の 1:50 のままです）。";
		if (imageScaleReverted)
		{
			// 与えた縮尺のイメージが作り直しの後に 1 つも残っていない。何を何にしようとしたかを
			// そのまま出せば、実機の OIP の表示と突き合わせて判定できる。
			const auto num = [](double value)
			{
				std::array<char, 32> buffer{};
				std::snprintf(buffer.data(), buffer.size(), "%g", value);
				return std::string(buffer.data());
			};
			text += "凡例イメージの縮率が作り直しで戻されました（" + num(counts.imageScaleBefore) +
					" → " + num(counts.imageScaleTarget) + " と書きました）。";
		}
		return text;
	}
} // namespace HomeskzIfcImport::draw
