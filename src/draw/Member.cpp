//
//	draw/Member.cpp
//
//	横架材描画の実装。命令セット（MemberCommand）を**構造材ツール（StructuralMember）**の
//	オブジェクトとして配置する。Python 版 vw/member.py に対応する。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	描画手順（Python 版 draw_member と同じ意図。実現手段は SDK の作法に合わせる）:
//	  1. **パス**＝天端中央線の始端→終端を通る 2 点の NURBS 曲線（柱と共通。
//	     draw/StructuralMember の CreatePath）。頂点は命令のセンタリング済み絶対座標で、
//	     **両端とも同じ Z（始端の天端 Z）**を持つ（傾斜は下記のストーリバウンドが与える）。
//	  2. **プロファイル**＝断面の矩形（幅 × せい）をグループに入れたもの。
//	  3. **PIO の生成から各フィールドの設定までは柱と共通**（draw/StructuralMember）。
//	     ここが受け持つのは横架材固有の値——パスの Z（両端とも天端 Z）・天端中央基準の
//	     断面矩形・構造用途（横架材）・スタイル（木質構造材_横架材）・配置先レイヤ——だけ。
//	  4. 全配置後に UpdateStyledObjects を 1 回（下記「スタイルは関連付けだけでは効かない」）。
//	PIO を生成できない場合は平面投影の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【パスに傾斜を持たせない】始端／終端の高さ（傾斜梁の勾配）は **SetObjectStoryBound の
//	offset 差だけ**で表し、パスの 2 頂点は同じ Z にする。構造材ツールの高さバインドは指定した
//	高さ差をパス由来の部材長に**加算**するため、パスにも傾斜を持たせると傾斜が二重に適用され、
//	終端が実際の 2 倍の高さに描かれる（Python 版が柱の二重加算 #54 と同種の問題として実機で
//	確認済み。水平梁は差が 0 なので顕在化しない）。
//
//	【パスの遍歴（2 頂点の 3D ポリライン → 2D ポリライン → 2 点の NURBS 曲線）】最初は 2 頂点の
//	**3D ポリライン**（VWPolygon3DObj）に絶対 Z を持たせていたが、実機で**構造材が長さ 0 に
//	なり画面に何も描かれなかった**。OIP は「スパン 0 / 長さ 0」（どちらもパスが支配するため
//	グレーアウト）で、X/Y/Z はパスの始点と一致していた——つまり PIO はパスを**挿入点として
//	しか読まず、長さを取れていなかった**。断面（構造材 ID 105×240）とスタイルは正しく入って
//	いたので、原因は**パスの種別**（VWPolygon3DObj）にある。
//
//	Python 版は NURBS 曲線（CreateNurbsCurve ＋ AddVertex3D）を渡しているが、M7 の時点では
//	「ISDK には NURBS 曲線へ頂点を足す呼び出しが無い」と誤認しており（`VWNURBSCurve` が
//	評価専用なのは事実だが、ISDK 側に `Add3DVertex` がある）、代わりに**本リポジトリで実績の
//	ある 2D ポリライン**を選んだ——draw/Grid が GridAxis のパスにまさに VWPolygon2DObj を
//	渡しており、M1 で実機確認済みだったため。これで M7 のローカル確認は通っている（OIP の
//	スパン・長さが実寸（例 2170）、勾配 0°・主幅 105・主高さ 120・構造材 ID も命令どおり、
//	高さもバウンド（横架材天端・オフセット 0）だけで正しく決まり、段差梁・傾斜梁の二重加算も
//	起きず、登り梁の屋根面スナップも乗った）。
//
//	M8 の柱で `Add3DVertex`（VS の AddVertex3D）が見つかり、2D ポリラインを選んだ前提が消えた。
//	水平材も鉛直材も本質は 3 次元空間の直線 1 本なので、**パスは柱と同じ 2 点の NURBS 曲線に
//	統一した**（Python 版と同じ作法へ戻ったことにもなる）。要素ごとに違うのは 2 点の Z だけで、
//	水平材は**両端とも同じ Z**を渡すため、傾斜が二重に適用される心配は無い。**Z を 0 には
//	しない**（3D 座標は絶対 Z として渡るので上階の梁が地面に落ちる。理由は
//	draw/StructuralMember.h 冒頭）。**この統一は M7 の確認項目——スパン・長さが実寸か、
//	段差梁・傾斜梁が二重加算にならないか、登り梁が屋根面に乗るか——の再確認が要る。**
//
//	【スタイルは関連付けだけでは効かない】ISDK の SetPluginObjectStyle はスタイルの関連付け
//	（パラメータ）までで、スタイルが決める描画属性（コンポーネントのクラス／マテリアル＝
//	テクスチャ等）はオブジェクトへプッシュされない（Python 版 #56 と同じ）。そこで全配置後に
//	UpdateStyledObjects を 1 回呼び、当該スタイルの全オブジェクトをスタイルから更新する
//	（by-instance の個別フィールド＝寸法・構造材 ID 等は保持したまま by-style の描画属性だけが
//	更新される）。
//

#include "PluginPrefix.h"
#include "draw/Member.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cmath>
#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// プラグインスタイル名（VW 実機の登録名に一致させる）。
		const TXString kMemberStyle("木質構造材_横架材");

		// 構造用途（横架材）。ポップアップはキーで保持されるため数値文字列。
		constexpr const char* kStructuralUseBeam = "1";

		// パスから取れた部材長（OIP「スパン」）。**書くためではなく読み戻して確かめるため**の
		// 名前で、0 のままなら PIO がパスの長さを取れていない＝画面に何も描かれない。
		constexpr const char* kFieldSpan = "Span";
		constexpr const char* kLocalizedSpan = "スパン";
		// 部材長を「取れていない」とみなす閾値（mm）。読み戻した スパン がこれ以下なら 0 扱い。
		constexpr double kZeroLengthTol = 1e-6;

		// 横架材 1 本を構造材ツールで描く。PIO を作れなければ平面投影の直線でフォールバック
		// する。何か 1 つでも配置できたら true。
		bool DrawOne(const core::MemberCommand& member, RefNumber style,
					 std::size_t& outPathFailures, std::size_t& outSectionFailures,
					 std::size_t& outLengthFailures)
		{
			// 断面（プロファイルグループ）を**先に**用意する。作れなければ PIO を作らない
			// ——断面の無い構造材は生成できても実体が描かれず、「オブジェクトはあるのに
			// 画面に出ない」状態になるだけなので、直線のフォールバックの方が有用。
			// 矩形の置き方は M7 のローカル確認で通った [0, 0]〜[幅, せい] のまま
			// （断面基準点は AxisAlign＝天端中央が決める。矩形の座標そのものは変えない）。
			const MCObjectHandle profile =
				CreateRectangleProfileGroup(0.0, 0.0, member.width, member.height);

			// パス＝天端中央線の始端→終端を通る 2 点の NURBS 曲線（柱と共通。
			// draw/StructuralMember の CreatePath）。**両端とも同じ Z（始端の天端 Z）**を
			// 渡し、傾斜梁の勾配はストーリバウンドの offset 差だけで表す（冒頭「パスに
			// 傾斜を持たせない」）。Z を 0 にはしない——3D 座標は絶対 Z として渡るので、
			// 0 を渡すと上階の梁が地面に置かれる（draw/StructuralMember.h 冒頭）。
			bool pathAppended = false;
			const MCObjectHandle path =
				profile == nil
					? nil
					: CreatePath(core::Vec3{member.start.x, member.start.y, member.elevation},
								 core::Vec3{member.end.x, member.end.y, member.elevation},
								 pathAppended);
			if (path != nil && !pathAppended)
				++outPathFailures;

			StructuralMemberSpec spec;
			spec.path = path;
			spec.profile = profile;
			spec.memberId = member.memberId;
			spec.drawClass = member.drawClass;
			spec.structuralUse = kStructuralUseBeam;
			spec.width = member.width;
			spec.depth = member.height;
			spec.axisAlign = StructuralAxisAlign::TopCentre; // 命令の基準点（天端中央）と一致
			spec.startBound = member.startBound;
			spec.endBound = member.endBound;

			const StructuralMemberResult result = DrawStructuralMember(spec, style);
			if (result.object == nil)
			{
				// フォールバック: 平面投影の直線（クラス付き）を残す。
				VWPolygon2DObj line({VWPoint2D(member.start.x, member.start.y),
									 VWPoint2D(member.end.x, member.end.y)});
				line.SetClosed(false);
				const MCObjectHandle lineHandle = line.GetThisObject();
				if (lineHandle == nil)
					return false;
				SetClassByName(lineHandle, member.drawClass);
				return true;
			}

			// 断面が入らなかった本数を数える（診断。drawMembers が完了ダイアログへ載せる）。
			if (!result.sectionOk)
				++outSectionFailures;

			// パスから部材長を取れたかを読み戻す。0 のままなら実体が無く画面に描かれない
			// （冒頭「パスの遍歴」の 3D ポリラインで起きた症状そのもの）。**鉛直材（柱）では
			// スパン 0 が正常**なので、この数え方は水平材だけのもの（draw/Column.cpp 参照）。
			//
			// **パラメータが実在するときだけ数える。** ResolveParamName は見つからなくても
			// universal 名をそのまま返し、GetParamReal は存在しない名前に対して 0 を返すので、
			// 存在確認をしないと「スパン」という名前が違うだけで**パスは正常なのに全数を
			// 長さ 0 と誤報**してしまう（診断が嘘をつくと切り分けが逆に遠のく）。
			VWParametricObj pio(result.object);
			const TXString span = ResolveParamName(pio, kFieldSpan, kLocalizedSpan);
			if (pio.GetParamIndex(span) != static_cast<size_t>(-1) &&
				std::abs(pio.GetParamReal(span)) <= kZeroLengthTol)
				++outLengthFailures;
			return true;
		}
	} // namespace

	std::size_t drawMembers(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics)
	{
		if (document.members.empty())
			return 0;

		const RefNumber style = ResolvePluginStyle(kMemberStyle);

		std::size_t drawn = 0;
		std::size_t pathFailures = 0;
		std::size_t sectionFailures = 0;
		std::size_t lengthFailures = 0;
		for (const core::MemberCommand& member : document.members)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は本数で報告し、
			// 描画の前に 1 件進める（＝「いま何本目を描いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤ（"n-横架材天端" / "R-軒高" / "n-母屋" / "n-登り梁"）が無い命令は
			// スキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(member.layer) == nil)
				continue;

			if (DrawOne(member, style, pathFailures, sectionFailures, lengthFailures))
				++drawn;
		}

		// 全配置後に 1 回だけスタイル更新を掛けて、by-style の描画属性を反映する
		// （冒頭「スタイルは関連付けだけでは効かない」）。
		if (drawn > 0 && style != 0)
			gSDK->UpdateStyledObjects(style);

		// 診断: 実描画はローカルの VectorWorks でしか確認できないので、「作れたが断面が
		// 入らなかった」「スタイルが見つからなかった」を件数で持ち帰る。横架材が 1 本も
		// 見えないときに、原因が命令側（解析）か PIO のパラメータ側かを切り分けられる。
		if (outDiagnostics != nullptr &&
			(pathFailures > 0 || sectionFailures > 0 || lengthFailures > 0 || style == 0))
		{
			std::string note = "横架材の診断: ";
			if (pathFailures > 0)
				note += "パスが 2 点にならなかった材 " + std::to_string(pathFailures) + " 本。";
			if (sectionFailures > 0)
				note += "断面を設定できなかった材 " + std::to_string(sectionFailures) + " 本。";
			if (lengthFailures > 0)
				note += "パスから長さを取れなかった材 " + std::to_string(lengthFailures) + " 本。";
			if (style == 0)
				note += "プラグインスタイル『木質構造材_横架材』が見つかりません。";
			*outDiagnostics = std::move(note);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
