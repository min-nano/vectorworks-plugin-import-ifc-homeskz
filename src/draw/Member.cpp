//
//	draw/Member.cpp
//
//	横架材描画の実装。命令セット（MemberCommand）を**構造材ツール（StructuralMember）
//	**のオブジェクトとして配置する。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include
//	するため、この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の
//	core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	描画手順:
//	  1. **パス**＝天端中央線の始端→終端を通る 2 点の曲線（柱と共通。draw/StructuralMember
//	     の CreatePath）。頂点は命令のセンタリング済み**平面座標だけ**で、**Z は持たない**
//	     （高さも勾配も下記のストーリバウンドが与える。draw/StructuralMember.h 冒頭
//	     「パスは 2D で渡す」）。
//	  2. **プロファイル**＝断面の矩形（幅 × せい）をグループに入れたもの。
//	  3. **PIO の生成から各フィールドの設定までは柱と共通**（draw/StructuralMember）。
//	     ここが受け持つのは横架材固有の値——パスの平面座標・天端中央基準の断面矩形・
//	     構造用途（横架材）・スタイル（木質構造材_横架材）・配置先レイヤ——だけ。
//	  4. 全配置後に UpdateStyledObjects を 1 回（下記「スタイルは関連付けだけでは効かない」）。
//	PIO を生成できない場合は平面投影の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【パスに高さを持たせない】始端／終端の高さ（傾斜梁の勾配を含む）は **SetObjectStoryBound の
//	offset だけ**で表し、パスは平面（Z=0）に置く。構造材ツールの高さバインドは指定した高さ差を
//	パス由来の部材長に**加算**するため、パスにも傾斜を持たせると傾斜が二重に適用され、終端が
//	実際の 2 倍の高さに描かれる（実機で確認済み）。M27 でその先も分かった——**パスの Z は
//	そもそも PIO に受け取られておらず**、3D のパスは PIO がバウンドの解決結果から自分で作る。
//	だから絶対 Z を入れる意味も無く、入れれば「高さを 2 か所で指定する」ことにしかならない
//	（draw/StructuralMember.h 冒頭「パスは 2D で渡す」）。
//
//	【パスの遍歴（2 頂点の 3D ポリライン → 2D ポリライン → 2 点の NURBS 曲線）】最初は 2 頂点の
//	**3D ポリライン**（VWPolygon3DObj）に絶対 Z を持たせていたが、実機で**構造材が長さ 0 に
//	なり画面に何も描かれなかった**。OIP は「スパン 0 / 長さ 0」（どちらもパスが支配するため
//	グレーアウト）で、X/Y/Z はパスの始点と一致していた——つまり PIO はパスを**挿入点として
//	しか読まず、長さを取れていなかった**。断面（構造材 ID 105×240）とスタイルは正しく入って
//	いたので、原因は**パスの種別**（VWPolygon3DObj）にある。
//
//	M7 の時点では「ISDK には NURBS 曲線へ頂点を足す呼び出しが無い」と誤認しており
//	（`VWNURBSCurve` が評価専用なのは事実だが、ISDK 側に `Add3DVertex` がある）、代わりに
//	**本リポジトリで実績のある 2D ポリライン**を選んだ——draw/Grid が GridAxis のパスにまさに
//	VWPolygon2DObj を渡しており、M1 で実機確認済みだったため。これで M7 のローカル確認は通って
//	いる（OIP のスパン・長さが実寸（例 2170）、勾配 0°・主幅 105・主高さ 120・構造材 ID も
//	命令どおり、高さもバウンド（横架材天端・オフセット 0）だけで正しく決まり、段差梁・傾斜梁の
//	二重加算も起きず、登り梁の屋根面スナップも乗った）。
//
//	M8 の柱で `Add3DVertex`（VS の AddVertex3D）が見つかり、2D ポリラインを選んだ前提が消えた。
//	水平材も鉛直材も本質は直線 1 本なので、**パスは柱と同じ 2 点の曲線に統一した**。
//	**そして M27 で Z も外した**——渡した Z は PIO に受け取られておらず、高さはバウンドだけが
//	決めていたので、パスは**平面（Z=0）の 2 点**になった。これは M7 の 2D ポリライン
//	（高さがバウンドだけで正しく決まっていた）と同じ形へ戻ったということでもある。
//
//	【スタイルは関連付けだけでは効かない】ISDK の SetPluginObjectStyle はスタイルの関連付け
//	（パラメータ）までで、スタイルが決める描画属性（コンポーネントのクラス／マテリアル＝
//	テクスチャ等）はオブジェクトへプッシュされない。そこで全配置後に UpdateStyledObjects を
//	1 回呼び、当該スタイルの全オブジェクトをスタイルから更新する（by-instance の個別フィールド
//	＝寸法・構造材 ID 等は保持したまま by-style の描画属性だけが更新される）。
//

#include "PluginPrefix.h"
#include "draw/Member.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"
#include "draw/Verify.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// プラグインスタイル名（VW 実機の登録名に一致させる）。
		const TXString kMemberStyle("木質構造材_横架材");

		// 横架材 1 本を構造材ツールで描く。PIO を作れなければ平面投影の直線でフォールバック
		// する。何か 1 つでも配置できたら true。**構造材ツールで描けたときだけ** outObject に
		// そのハンドルを入れる（断面寸法データタグの関連付け先。フォールバックの直線は
		// 断面寸法を持たないのでタグを付ける相手にしない。draw/Column の柱ハンドルと同じ扱い）。
		bool DrawOne(const core::MemberCommand& member, RefNumber style,
					 StructuralFailures& failures, MCObjectHandle& outObject)
		{
			// 断面（プロファイルグループ）を**先に**用意する。作れなければ PIO を作らない
			// ——断面の無い構造材は生成できても実体が描かれず、「オブジェクトはあるのに
			// 画面に出ない」状態になるだけなので、直線のフォールバックの方が有用。
			// 矩形の置き方は M7 のローカル確認で通った [0, 0]〜[幅, せい] のまま
			// （断面基準点は AxisAlign＝天端中央が決める。矩形の座標そのものは変えない）。
			const MCObjectHandle profile =
				CreateRectangleProfileGroup(0.0, 0.0, member.width, member.height);

			// パス＝天端中央線の始端→終端を通る 2 点の曲線（柱・垂木と共通。
			// draw/StructuralMember の CreatePath）。**平面座標だけを渡す**——高さも勾配も
			// 上下端のストーリバウンドが決め、構造材 PIO はその解決結果から 3D のパスを
			// 自分で作る（冒頭「パスに傾斜を持たせない」／draw/StructuralMember.h 冒頭
			// 「パスは 2D で渡す」）。以前は両端とも天端 Z を入れていたが、**その Z は
			// PIO に受け取られていなかった**（M27）ので、同じ高さをバウンドとパスの
			// 2 か所へ書く形をやめた。
			bool pathAppended = false;
			const MCObjectHandle path =
				profile == nil ? nil : CreatePath(member.start, member.end, pathAppended);
			if (path != nil && !pathAppended)
				++failures.path;

			StructuralMemberSpec spec;
			spec.path = path;
			spec.profile = profile;
			spec.memberId = member.memberId;
			spec.drawClass = member.drawClass;
			// 構造用途（横架材）。ポップアップのキーは命令セットの語彙なので core が持つ
			// （柱・小屋束・垂木と同じ置き場所。core/Document.h）。
			spec.structuralUse = core::kStructuralUseBeam;
			spec.width = member.width;
			spec.depth = member.height;
			spec.axisAlign = StructuralAxisAlign::TopCentre; // 命令の基準点（天端中央）と一致
			spec.startBound = member.startBound;
			spec.endBound = member.endBound;
			// 端部オフセット（負値）。命令の端点は取り合い相手の芯線上なので、材が実際に
			// 止まる位置はここで戻す（core/Document.h「端部オフセット」）。
			spec.startOffset = member.startOffset;
			spec.endOffset = member.endOffset;
			// 【潰れ検出】描き上がりの長さ＝パスの水平長（端部オフセットはこの長さから戻す量
			// なので、潰れていないかを見るこの検査には要らない）。**水平材は両端の Z が等しい
			// ので「両端の絶対 Z の差」では測れない**——測るのは**PIO が実際に持っている
			// パスの両端の距離**である（draw/StructuralMember.h の StructuralExtentKind）。
			// 以前はここが OIP の「スパン」で、**そのパラメータは実機に無い**ため
			// 潰れ検出も下の自己修復も一度も動いていなかった（docs/DEV-NOTES.md
			// 「柱が長さ 0 で描かれる（M27）」）。
			spec.expectedLength = core::distance(member.start, member.end);
			spec.extentKind = StructuralExtentKind::Horizontal;
			// 【潰れていても繕わない】潰れていたパスを `SetCustomObjectPath` で作り直す
			// 自己修復は**撤去した**（柱・垂木も同じ）。理由は 2 つある。
			//
			// 1. **水平材は M27 の死角に落ちない。** `ResetObject` が作り直すかどうかは
			//    パスの 3 次元長で決まり、`(0, 1e-7)` の帯だけが作り直されない。水平材は
			//    実寸の水平成分を持ち、しかも作り直しは**水平成分をビット一致で残す**ので、
			//    その帯に入ることが原理的に無い（[Findings「水平成分が 1e-7 以上ある部材は
			//    …」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Parametric%20Objects.md)。
			//    72 ケースで確認済み）。柱のほうは、パスに Z を渡すのをやめたことで帯へ入る
			//    経路そのものが消えている（draw/Column）。
			// 2. **差し替えは対症療法にしかならず、あとで壊れる。** 差し替えたパスに合わせて
			//    `ResetObject` が**バウンドの `fOffset` を書き換える**ので、上端が「上階の
			//    レベルちょうど」から「上階のレベル −2959」のような値に化ける。部材は階に
			//    追従し続けるが**何に揃えていたか**が壊れ、**利用者が階高を編集した瞬間に
			//    長さとして表に出る**（同 Findings「差し替えで `fOffset` を書き換えられた
			//    部材は、階を動かすと長さが変わる」。実測で、階を +100 動かすと本来 3059 に
			//    なるべき部材が 100 になった）。同 Findings も「差し替えを対症療法に
			//    使わない」と明記している。
			//
			// **報せるところは残っている**——潰れは開発ビルドで数え、高さ基準を書けなかった
			// 本数は本番でも出る（`StructuralFailures` の `collapsed` / `bound`）。
			//
			// **残る唯一の 0 長の経路**——高さ基準を 1 本も書けないまま `ResetObject` を
			// 呼ぶ——は向きを問わず起こりうるが、**本番ビルドでも件数が出る**
			// （`StructuralFailures::bound` ＝「高さ基準を図面へ書けなかった材 N 本」。
			// draw/Verify.h の外）ので、黙って見逃すことにはならない。**潰れの検出そのものは
			// 開発ビルドでは引き金に依らず走る**（draw/StructuralMember の measureDrawn）ので、
			// 武装を外しても診断は 1 つも失われない。
			// 【高さの検算】パスから Z を外した以上、高さを決めるのはバウンドだけになった。
			// その解決が意図とずれても本数にもスパンにも出ないので、**描き上がった両端の
			// 絶対 Z を読み戻して命令と引き比べる**（draw/StructuralMember.h の
			// checkElevation）。天端 Z は傾斜梁で両端が違う（elevation / endElevation）。
			// **開発ビルドだけ**（draw/Verify.h）。
#if VW_DRAW_VERIFY
			spec.checkElevation = true;
			spec.expectedStartZ = member.elevation;
			spec.expectedEndZ = member.endElevation;
#endif

			const StructuralMemberResult result = DrawStructuralMember(spec, style);
			if (result.object == nil)
			{
				// フォールバック: 平面投影の直線（クラス付き）を残す。**端部オフセットを戻した
				// 材の端**で引く——PIO が無い以上オフセットを効かせる先が無いので、線の側を
				// 材の範囲に合わせる（core/Document.h「端部オフセット」）。
				const core::Vec2 drawnStart = core::memberDrawnStart(member);
				const core::Vec2 drawnEnd = core::memberDrawnEnd(member);
				VWPolygon2DObj line(
					{VWPoint2D(drawnStart.x, drawnStart.y), VWPoint2D(drawnEnd.x, drawnEnd.y)});
				line.SetClosed(false);
				const MCObjectHandle lineHandle = line.GetThisObject();
				if (lineHandle == nil)
					return false;
				SetClassByName(lineHandle, member.drawClass);
				return true;
			}

			// 断面寸法データタグの関連付け先として記録する（draw/Tag が引く）。
			outObject = result.object;

			// 失敗の内訳を数え込む（診断。drawMembers が完了ダイアログへ載せる）。
			failures.record(result);
			return true;
		}
	} // namespace

	std::size_t drawMembers(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics, ObjectHandles* handles)
	{
		if (document.members.empty())
			return 0;

		const RefNumber style = ResolvePluginStyle(kMemberStyle);

		std::size_t drawn = 0;
		StructuralFailures failures;
		for (std::size_t index = 0; index < document.members.size(); ++index)
		{
			const core::MemberCommand& member = document.members[index];

			if (!AdvanceProgress(progress))
				break;

			// 配置先レイヤ（"n-横架材天端" / "R-軒高" / "n-母屋" / "n-登り梁"）が無い命令は
			// スキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(member.layer) == nil)
				continue;

			MCObjectHandle object = nil;
			if (DrawOne(member, style, failures, object))
				++drawn;

			// **命令インデックス → ハンドル**の対応表へ記録する（断面寸法データタグが
			// 関連付け先として引く。立上り → 壁結合・柱 → 伏図記号と同じ受け渡し方式。
			// draw/ObjectHandles.h）。
			if (handles != nullptr && object != nil)
				handles->table().handles.emplace(index, object);
		}

		// 全配置後に 1 回だけスタイル更新を掛けて、by-style の描画属性を反映する
		// （冒頭「スタイルは関連付けだけでは効かない」）。
		if (drawn > 0 && style != 0)
			gSDK->UpdateStyledObjects(style);

		// 診断: 実描画はローカルの VectorWorks でしか確認できないので、失敗の内訳を件数で
		// 持ち帰る（文言は draw/StructuralMember。横架材が 1 本も見えないときに、原因が命令側
		// （解析）か PIO のパラメータ側かを切り分けられる）。
		std::string note = DescribeStructuralFailures(failures, "材");
		if (style == 0)
			note += "プラグインスタイル『木質構造材_横架材』が見つかりません。";
		if (outDiagnostics != nullptr && !note.empty())
			*outDiagnostics = "横架材の診断: " + note;

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
