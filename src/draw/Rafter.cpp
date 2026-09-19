//
//	draw/Rafter.cpp
//
//	垂木描画の実装。命令セット（RafterCommand）を**構造材ツール（StructuralMember、構造用途
//	＝垂木）**のオブジェクトとして配置する。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を
//	include するため、この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の
//	core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	描画手順（横架材・柱と同じ道具立て。draw/StructuralMember が唯一の実装）:
//	  1. **パス**＝下面中央線の軒先→棟を通る 2 点の曲線。頂点は命令のセンタリング済み
//	     **平面座標だけ**で、**Z は持たない**（高さも勾配も下記のストーリバウンドが与える。
//	     draw/StructuralMember.h 冒頭「パスは 2D で渡す」）。
//	  2. **プロファイル**＝断面の矩形（幅 × せい）を**下辺中央が原点**になるように置いたもの
//	     （断面基準点＝中下と一致させる。draw/StructuralMember.h「断面基準点」）。
//	  3. **PIO の生成から各フィールドの設定までは横架材・柱と共通**（draw/StructuralMember）。
//	     ここが受け持つのは垂木固有の値——軒先まで伸ばしたパス・中下基準の断面矩形・
//	     構造用途（垂木）・配置先レイヤ——だけ。
//	PIO を生成できない場合は平面投影の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【軸組ツール（FramingMember）から構造材ツールへ移した】M6 の垂木は軸組ツールで描いており、
//	勾配・軒の出・差し込み・仕様ラベルを PIO のパラメータが持っていた。要件により**他の木部材
//	（横架材・柱・小屋束）と同じ構造材ツール**へ揃える。クラス（小屋組-垂木）と配置先レイヤ
//	（"n-垂木"）は変えない。移行にあたって効くのは次の 2 点で、どちらも命令セットは変えずに
//	描画側で吸収できない性質のものなので、命令へ高さ基準（startBound / endBound）を足した:
//	  * **構造材ツールに軒の出・差し込みのパラメータが無い。** 軸組ツールは「挿入点＝支持点」
//	    から軒側へ 差し込み＋軒の出 だけ材を伸ばしてくれたが、構造材ツールは**パスがそのまま
//	    材の範囲**になる。したがってパスの始端は支持点ではなく**軒先**（core::rafterEaveEnd が
//	    命令から求める純計算）。
//	  * **高さはストーリバウンドが支配する。** 構造材ツールは両端をストーリレベルへバインド
//	    して高さを決め、offset を「レベル Z − オブジェクト Z」で再計算して上書きすることさえ
//	    ある（draw/StructuralMember.h 冒頭）。軸組ツール時代の「配置行列の絶対 Z」は使えない
//	    ので、垂木レベル（"n-垂木" レイヤのレベル）からの offset を parse が命令に載せる。
//
//	【パスに高さを持たせない】勾配も高さも **SetObjectStoryBound の offset だけ**で表し、パスは
//	平面（Z=0）に置く。構造材ツールの高さバインドは指定した高さ差をパス由来の部材長へ**加算**
//	するため、パスにも傾斜を持たせると二重に適用される（登り梁で実機確認済み。draw/Member.cpp
//	冒頭）。加えて M27 で、**パスの Z はそもそも PIO に受け取られていない**と分かった——3D の
//	パスは PIO がバウンドの解決結果から自分で作るので、絶対 Z を入れる意味も無い
//	（draw/StructuralMember.h 冒頭）。垂木は登り梁と同じ「傾いた線材」なので作法も同じ。
//
//	【スタイルは当てない】横架材（木質構造材_横架材）・柱（木質構造材_柱・束）と違い、垂木は
//	プラグインスタイルを関連付けない。描画属性はクラス（小屋組-垂木）に従わせる——「クラスや
//	レイヤは現状のまま」という要件をそのまま満たし、既存 2 スタイルの意味も汚さないため。
//	スタイルが無いので UpdateStyledObjects も呼ばない。
//

#include "PluginPrefix.h"
#include "draw/Rafter.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"
#include "draw/Verify.h"
#include "core/Document.h"
#include "core/Progress.h"

// フォールバックの直線は draw/Grid.cpp・draw/Member.cpp と同じ VWPolygon2DObj で描く。
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// プラグインスタイルを当てないことを表す RefNumber（DrawStructuralMember の約束。
		// 冒頭「スタイルは当てない」）。
		constexpr RefNumber kNoStyle = 0;

		// 垂木 1 本を構造材ツールで描く。PIO を作れなければ平面投影の直線でフォールバック
		// する。何か 1 つでも配置できたら true。
		bool DrawOne(const core::RafterCommand& rafter, StructuralFailures& failures)
		{
			// 実際の材端は支持点ではなく軒先（冒頭「軸組ツールから構造材ツールへ移した」）。
			const core::RafterEaveEnd eave = core::rafterEaveEnd(rafter);

			// 断面の矩形は**下辺中央が原点**（断面基準点＝中下と一致させる。パスが下面中央線を
			// 通る）。作れなければ PIO を作らない——断面の無い構造材は生成できても実体が
			// 描かれず、「オブジェクトはあるのに画面に出ない」状態になるだけなので、直線の
			// フォールバックの方が有用（draw/DrawUtil 参照）。
			const MCObjectHandle profile = CreateRectangleProfileGroup(
				-rafter.width / 2.0, 0.0, rafter.width / 2.0, rafter.height);

			// パス＝下面中央線の軒先→棟を通る 2 点の曲線（横架材・柱と共通。
			// draw/StructuralMember の CreatePath）。**平面座標だけを渡す**——高さも勾配も
			// ストーリバウンドの offset が決め、構造材 PIO はその解決結果から 3D のパスを
			// 自分で作る（冒頭「パスに傾斜を持たせない」／draw/StructuralMember.h 冒頭
			// 「パスは 2D で渡す」）。以前は両端とも軒先の下面 Z を入れていたが、**その Z は
			// PIO に受け取られていなかった**（M27）。
			bool pathAppended = false;
			const MCObjectHandle path =
				profile == nil ? nil : CreatePath(eave.point, rafter.end, pathAppended);
			if (path != nil && !pathAppended)
				++failures.path;

			StructuralMemberSpec spec;
			spec.path = path;
			spec.profile = profile;
			// 構造材 ID は仕様ラベル（"45×45@455"）。軸組ツール時代にラベルとして OIP へ
			// 出していた文字で、断面と間隔がひと目で分かる。
			spec.memberId = rafter.label;
			spec.drawClass = rafter.drawClass;
			spec.structuralUse = core::kStructuralUseRafter;
			spec.width = rafter.width;
			spec.depth = rafter.height;
			spec.axisAlign = StructuralAxisAlign::BottomCentre; // 中下（断面矩形の置き方と一致）
			// 始端＝軒先（支持点の offset から勾配ぶん下げた値）、終端＝棟側。
			spec.startBound = rafter.startBound;
			spec.startBound.offset = eave.offset;
			spec.endBound = rafter.endBound;
			// 【潰れ検出】描き上がりの長さ＝パスの水平長。**垂木も両端の Z が等しい**（勾配は
			// ストーリバウンドの offset 差が表す。冒頭「パスに傾斜を持たせない」）ので、
			// 横架材と同じく OIP の「スパン」で測る（draw/StructuralMember.h の
			// StructuralExtentKind）。
			spec.expectedLength = core::distance(eave.point, rafter.end);
			spec.extentKind = StructuralExtentKind::Span;
			// 【自己修復】潰れていたらパスを作り直して差し替える。**パスを作る口も PIO 化の口も
			// 柱・横架材と同じもの**なので、同じ事故は垂木でも起きうる（実機で出たのは柱だけ
			// だが、出ていないことの保証にはならない。docs/DEV-NOTES.md M27）。差し替える
			// パスは**挿入点からの相対**で渡す。
			spec.retryWithFreshPath = true;
			spec.pathStart = core::Vec2{0.0, 0.0};
			spec.pathEnd = core::Vec2{rafter.end.x - eave.point.x, rafter.end.y - eave.point.y};
			// 【高さの検算】パスから Z を外した以上、垂木の高さと勾配を決めるのはバウンドの
			// offset 差だけになった。ずれても本数にもスパンにも出ないので、**描き上がった
			// 両端の絶対 Z を読み戻して命令と引き比べる**（draw/StructuralMember.h の
			// checkElevation）。始端は軒先の下面 Z（支持点より勾配ぶん下がる）、終端は棟側の
			// 下面 Z（core/Document.h の RafterCommand）。**開発ビルドだけ**（draw/Verify.h）。
#if VW_DRAW_VERIFY
			spec.checkElevation = true;
			spec.expectedStartZ = eave.z;
			spec.expectedEndZ = rafter.endElevation;
#endif

			// スタイルは当てない（冒頭「スタイルは当てない」）。
			const StructuralMemberResult result = DrawStructuralMember(spec, kNoStyle);
			if (result.object == nil)
			{
				// フォールバック: 平面投影の直線（クラス付き）を残す。
				VWPolygon2DObj line(
					{VWPoint2D(eave.point.x, eave.point.y), VWPoint2D(rafter.end.x, rafter.end.y)});
				line.SetClosed(false);
				const MCObjectHandle lineHandle = line.GetThisObject();
				if (lineHandle == nil)
					return false;
				SetClassByName(lineHandle, rafter.drawClass);
				return true;
			}

			// 失敗の内訳を数え込む（診断。drawRafters が完了ダイアログへ載せる）。
			failures.record(result);
			return true;
		}
	} // namespace

	std::size_t drawRafters(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics)
	{
		if (document.rafters.empty())
			return 0;

		std::size_t drawn = 0;
		StructuralFailures failures;
		for (const core::RafterCommand& rafter : document.rafters)
		{
			if (!AdvanceProgress(progress))
				break;

			// 配置先レイヤ（"n-垂木"）が無い命令はスキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(rafter.layer) == nil)
				continue;

			if (DrawOne(rafter, failures))
				++drawn;
		}

		// 診断: 実描画はローカルの VectorWorks でしか確認できないので、失敗の内訳を件数で
		// 持ち帰る（文言は draw/StructuralMember。垂木が 1 本も見えないときに、原因が命令側
		// （解析）か PIO のパラメータ側かを切り分けられる）。
		const std::string note = DescribeStructuralFailures(failures, "材");
		if (outDiagnostics != nullptr && !note.empty())
			*outDiagnostics = "垂木の診断: " + note;

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
