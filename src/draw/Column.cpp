//
//	draw/Column.cpp
//
//	柱描画の実装。命令セット（ColumnCommand）を**構造材ツール（StructuralMember）**の
//	鉛直材として配置する。Python 版 vw/column.py に対応する。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	描画手順（Python 版 draw_column と同じ意図。実現手段は SDK の作法に合わせる）:
//	  1. **パス**＝柱下端 (x, y, 下端 Z) から上端 (x, y, 下端 Z + 高さ) へ立つ鉛直曲線。
//	  2. **プロファイル**＝断面の矩形（幅 × せい）を**原点中心**に置いたグループ
//	     （断面基準点は AxisAlign＝中央。draw/DrawUtil の CreateRectangleProfileGroup）。
//	  3. CreateCustomObjectPath('StructuralMember', path, profile) で PIO を生成する。
//	  4. クラス分け → プラグインスタイル（木質構造材_柱・束）の関連付け → 上下端の
//	     ストーリバウンド → 個別フィールド（構造材 ID・断面・構造用途）→ ResetObject。
//	  5. 全配置後に UpdateStyledObjects を 1 回（横架材と同じ。draw/Member.cpp 冒頭）。
//	PIO を生成できない場合は断面の矩形にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【パスは配置点（XY）だけを与える。高さはストーリバウンドが決める】
//	**ISDK の構造材 PIO はパスを平面（2D）としてしか読まない。** 柱は鉛直材なので平面へ
//	落とすとパスが 1 点に潰れ、長さも Z も渡らない——ローカル確認（M8）で実際にそうなった:
//	OIP は「スパン 0 / 長さ 0」で、オブジェクトはレイヤ原点（Z=0）に置かれ、ストーリバウンドの
//	offset は VW が「レベル Z − オブジェクト Z」で再計算した値に上書きされていた。XY だけは
//	パスどおりだったので、**パスは配置点としてしか効いていない**。
//
//	これは M7 の横架材で「2 頂点の 3D ポリラインを渡したら長さ 0 になった」のと同じ現象で、
//	横架材はパスが平面内に長さを持つ（2D ポリライン）ので解決したが、柱では解決できない。
//	Python 版が使う VS の `CreateNurbsCurve` ＋ `AddVertex3D`（鉛直な 2 点の曲線）に相当する
//	組み立ては ISDK では作れない（`VWNURBSCurve` は評価専用で制御点から構築できず、
//	`CreateNurbsCurve` は 1 点の曲線しか作れない。ci-debug の sdk-grep で確認済み）。
//
//	したがって**柱の高さは上下端のストーリバウンドだけで決まる**。解析側はその前提で
//	offset に実際の下端／上端の絶対 Z までの距離を入れており（小屋束の上端 offset ＝
//	下端 offset ＋ 柱高さ）、パスは長さを持たないので二重加算は起こらない（Python 版 #116 の
//	「上端 offset を下端と同値にする」対策は、パスが柱高さを持つ VS 側の事情。parse/Column.h）。
//	パスは配置点として 1 点の NURBS 曲線を渡す（XY が正しく効くことは上記のローカル確認済み）。
//
//	【診断を必ず持ち帰る】実描画はローカルの VectorWorks でしか確認できない。そこで
//	draw/Member と同じく、断面が入ったかを**読み戻して確かめ**、駄目だった本数を完了
//	ダイアログへ返す。**スパン（部材長）は数えない**——柱ではパスが長さを持たないので 0 が
//	正常であり、横架材と同じ数え方をすると全数を誤報する。
//

#include "PluginPrefix.h"
#include "draw/Column.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 構造材ツールの PIO 名とプラグインスタイル名（VW 実機の登録名に一致させる）。
		// PIO は横架材と同じ（柱・間柱ツールはスクリプト操作に対して不安定なため、
		// Python 版が標準の構造材ツールへ置き換えた判断をそのまま引き継ぐ）。
		const TXString kStructuralMember("StructuralMember");
		const TXString kColumnStyle("木質構造材_柱・束");

		// SetObjectStoryBound に渡すバウンド ID（下端＝0・上端＝1。横架材の始端／終端と
		// 同じ規約で、柱では下から順に対応する）。
		constexpr Sint32 kBottomBoundID = 0;
		constexpr Sint32 kTopBoundID = 1;

		// 配置点として渡すパス（1 点の NURBS 曲線）の次数。直線 1 本なので 1。
		// byCtrlPts=false ＝ 通過点で定義する（Python 版 CreateNurbsCurve と同じ引数）。
		constexpr short kPathDegree = 1;

		// 構造材ツールのフィールド名（Python 版 vw/column.py の SetRField と同じ universal 名）。
		// **名前が 1 つ違うだけで setter は黙って無視される**ので、寸法は読み戻して確かめる
		// （draw/DrawUtil の ResolveParamName / SetParamRealChecked。draw/Member.cpp 冒頭）。
		constexpr const char* kFieldMemberID = "MemberID";			   // 構造材 ID
		constexpr const char* kFieldProfileShape = "ProfileShape";	   // 断面形状
		constexpr const char* kFieldMajorBreadth = "MajorBreadth";	   // 断面幅
		constexpr const char* kFieldMajorDepth = "MajorDepth";		   // 断面せい
		constexpr const char* kFieldB = "B";						   // 幅（矩形断面）
		constexpr const char* kFieldD = "D";						   // せい（矩形断面）
		constexpr const char* kFieldMemberType = "MemberType";		   // 部材種別
		constexpr const char* kFieldStructuralUse = "StructuralUse";   // 構造用途
		constexpr const char* kFieldAxisAlign = "AxisAlign";		   // 軸の配置基準
		constexpr const char* kFieldStartCondition = "StartCondition"; // 始端の端部条件
		constexpr const char* kFieldEndCondition = "EndCondition";	   // 終端の端部条件
		constexpr const char* kFieldProfileSeries = "ProfileSeries";   // 断面シリーズ

		// universal 名で引けなかったときに使う OIP のローカライズ名（ResolveParamName）。
		constexpr const char* kLocalizedBreadth = "幅";
		constexpr const char* kLocalizedDepth = "せい";
		constexpr const char* kLocalizedProfileShape = "断面形状";

		// フィールドに渡す値（Python 版と同じ。ポップアップはキーで保持されるため数値文字列）。
		constexpr const char* kProfileShapeRectangle = "Rectangle";
		constexpr const char* kMemberTypeColumn = "2";
		constexpr const char* kAxisAlignCentre = "4"; // 中央（3×3 グリッドの 0 始まり中央）
		constexpr const char* kEndConditionSquare = "3"; // 直切り
		constexpr const char* kProfileSeriesDefault = "AISC (Inch)";

		// プラグインスタイル（木質構造材_柱・束）の RefNumber。文書に無ければ 0（＝スタイル
		// 無しで描く）。名前から RefNumber を引く呼び出しが ISDK に無いため、名前付き
		// オブジェクトの InternalIndex を使う（draw/Member.cpp 冒頭「スタイルの RefNumber」）。
		RefNumber ResolveColumnStyle()
		{
			MCObjectHandle style = gSDK->GetNamedObject(kColumnStyle);
			if (style == nil || !gSDK->IsPluginStyle(style))
				return 0;
			return static_cast<RefNumber>(gSDK->GetObjectInternalIndex(style));
		}

		// 命令の高さ基準（StoryBoundCommand）を SDK の構造体へ写す（draw/Member と同じ）。
		VectorWorks::SStoryObjectData StoryBoundOf(const core::StoryBoundCommand& bound)
		{
			VectorWorks::SStoryObjectData data;
			data.fBound = VectorWorks::eStoryObjectBound_Story;
			data.fBoundStory = static_cast<Sint8>(bound.storyOffset);
			data.fLayerLevelType = TXString(bound.level.c_str());
			data.fOffset = bound.offset;
			return data;
		}

		// 柱の**配置点**を与えるパス（1 点の NURBS 曲線）を作る。作れなければ nil。
		// 冒頭「パスは配置点（XY）だけを与える」のとおり、構造材 PIO はパスを平面としてしか
		// 読まないので、ここで鉛直な線分を作っても長さ・Z は渡らない（高さはストーリ
		// バウンドが決める）。Z は配置点として素直に柱下端の絶対値を渡す。
		MCObjectHandle CreateColumnPath(const core::ColumnCommand& column)
		{
			return gSDK->CreateNurbsCurve(
				WorldPt3(column.position.x, column.position.y, column.elevation), false,
				kPathDegree);
		}

		// 柱 1 本を構造材ツールで描く。PIO を作れなければ断面の矩形でフォールバックする
		// （Python 版 draw_column と同じフォールバック）。何か 1 つでも配置できたら true。
		bool DrawOne(const core::ColumnCommand& column, RefNumber style,
					 std::size_t& outSectionFailures)
		{
			// 断面の矩形（幅 × せい）は**原点中心**に置く（AxisAlign＝中央と一致させる。
			// パスが断面中心を通る）。作れなければ PIO を作らない——断面の無い構造材は
			// 生成できても実体が描かれない（draw/DrawUtil 参照）。
			const MCObjectHandle profileGroup = CreateRectangleProfileGroup(
				-column.width / 2.0, -column.depth / 2.0, column.width / 2.0, column.depth / 2.0);

			const MCObjectHandle pathHandle = profileGroup == nil ? nil : CreateColumnPath(column);

			MCObjectHandle object =
				pathHandle == nil ? nil
								  : gSDK->CreateCustomObjectPath(kStructuralMember, pathHandle,
																 profileGroup, true);
			if (object == nil)
			{
				// フォールバック: 断面の矩形（クラス付き）を平面に残す。
				VWPolygon2DObj rect({VWPoint2D(column.position.x - (column.width / 2.0),
											   column.position.y - (column.depth / 2.0)),
									 VWPoint2D(column.position.x - (column.width / 2.0),
											   column.position.y + (column.depth / 2.0)),
									 VWPoint2D(column.position.x + (column.width / 2.0),
											   column.position.y + (column.depth / 2.0)),
									 VWPoint2D(column.position.x + (column.width / 2.0),
											   column.position.y - (column.depth / 2.0))});
				rect.SetClosed(true);
				const MCObjectHandle rectHandle = rect.GetThisObject();
				if (rectHandle == nil)
					return false;
				SetClassByName(rectHandle, column.drawClass);
				return true;
			}

			SetClassByName(object, column.drawClass);
			SetAllAttributesByClass(object);
			// スタイルは個別フィールドより**先に**関連付ける（後に設定する実測値で
			// スタイル既定のパラメータを上書きするため）。
			if (style != 0)
				gSDK->SetPluginObjectStyle(object, style);

			// 高さ基準を下端（0）・上端（1）それぞれのストーリレベルへバインドする。
			// **柱の高さはこの 2 つが決める**（冒頭「パスは配置点（XY）だけを与える」）。
			gSDK->SetObjectStoryBound(object, kBottomBoundID, StoryBoundOf(column.bottomBound));
			gSDK->SetObjectStoryBound(object, kTopBoundID, StoryBoundOf(column.topBound));

			VWParametricObj pio(object);
			const TXString breadth = ResolveParamName(pio, kFieldMajorBreadth, kLocalizedBreadth);
			const TXString depth = ResolveParamName(pio, kFieldMajorDepth, kLocalizedDepth);

			pio.SetParamAsString(ResolveParamName(pio, kFieldProfileShape, kLocalizedProfileShape),
								 kProfileShapeRectangle);
			pio.SetParamAsString(kFieldProfileSeries, kProfileSeriesDefault);
			const bool breadthOk = SetParamRealChecked(pio, breadth, column.width);
			const bool depthOk = SetParamRealChecked(pio, depth, column.depth);
			// B / D は矩形断面のときの別名。上と同じ値を入れる（存在しなければ無視される）。
			SetParamRealChecked(pio, ResolveParamName(pio, kFieldB, kLocalizedBreadth),
								column.width);
			SetParamRealChecked(pio, ResolveParamName(pio, kFieldD, kLocalizedDepth), column.depth);
			pio.SetParamAsString(kFieldMemberID, TXString(column.memberId.c_str()));
			pio.SetParamAsString(kFieldMemberType, kMemberTypeColumn);
			// 構造用途は命令が持つ値（柱="4" / 小屋束="5"）をそのまま入れる。小屋束を柱用途に
			// すると VW の柱高さモデルで上端高さが崩れる（parse/Column.h）。
			pio.SetParamAsString(kFieldStructuralUse, TXString(column.structuralUse.c_str()));
			pio.SetParamAsString(kFieldAxisAlign, kAxisAlignCentre);
			pio.SetParamAsString(kFieldStartCondition, kEndConditionSquare);
			pio.SetParamAsString(kFieldEndCondition, kEndConditionSquare);
			gSDK->ResetObject(object);

			// 断面が入らなかった本数を数える（診断。drawColumns が完了ダイアログへ載せる）。
			// **スパン（部材長）は数えない**——柱ではパスが長さを持たず 0 が正常なので、
			// 横架材と同じ数え方をすると全数を誤報する（冒頭「診断を必ず持ち帰る」）。
			if (!breadthOk || !depthOk)
				++outSectionFailures;
			return true;
		}
	} // namespace

	std::size_t drawColumns(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics)
	{
		if (document.columns.empty())
			return 0;

		const RefNumber style = ResolveColumnStyle();

		std::size_t drawn = 0;
		std::size_t sectionFailures = 0;
		for (const core::ColumnCommand& column : document.columns)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は本数で報告し、
			// 描画の前に 1 件進める（＝「いま何本目を描いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先の span レイヤ（"1to2-柱" 等）が無い命令はスキップする
			// （規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(column.layer) == nil)
				continue;

			if (DrawOne(column, style, sectionFailures))
				++drawn;
		}

		// 全配置後に 1 回だけスタイル更新を掛けて、by-style の描画属性を反映する
		// （SetPluginObjectStyle は関連付けまでで描画属性をプッシュしない。Python 版 #56）。
		if (drawn > 0 && style != 0)
			gSDK->UpdateStyledObjects(style);

		// 診断: 実描画はローカルの VectorWorks でしか確認できないので、「断面が入らなかった」
		// 「スタイルが見つからなかった」を件数で持ち帰る（柱が見えないときの切り分け材料）。
		if (outDiagnostics != nullptr && (sectionFailures > 0 || style == 0))
		{
			std::string note = "柱の診断: ";
			if (sectionFailures > 0)
				note += "断面を設定できなかった柱 " + std::to_string(sectionFailures) + " 本。";
			if (style == 0)
				note += "プラグインスタイル『木質構造材_柱・束』が見つかりません。";
			*outDiagnostics = std::move(note);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
