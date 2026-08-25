//
//	draw/Column.cpp
//
//	柱描画の実装。命令セット（ColumnCommand）を**構造材ツール（StructuralMember）
//	**の鉛直材として配置する。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	描画手順:
//	  1. **パス**＝柱下端 (x, y, 下端 Z) から上端 (x, y, 下端 Z + 高さ) へ立つ鉛直曲線。
//	  2. **プロファイル**＝断面の矩形（幅 × せい）を**原点中心**に置いたグループ
//	     （断面基準点は AxisAlign＝中央。draw/DrawUtil の CreateRectangleProfileGroup）。
//	  3. **PIO の生成から各フィールドの設定までは横架材と共通**（draw/StructuralMember）。
//	     ここが受け持つのは柱固有の値——鉛直パス・断面中心基準の断面矩形・構造用途
//	     （柱／小屋束）・スタイル（木質構造材_柱・束）・配置先レイヤ——だけ。
//	  4. 全配置後に UpdateStyledObjects を 1 回（横架材と同じ。draw/Member.cpp 冒頭）。
//	PIO を生成できない場合は断面の矩形にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【高さは「鉛直パス」と「上下端バウンドの差」の両方が要る】M8 のローカル確認 3 周で
//	切り分けた（どれか一方だけでは描かれない）:
//	  1 周目 … パス 1 点 ＋ バウンド差 0 → OIP は「スパン 0 / 長さ 0 / 高さ 0」で何も
//	           描かれず、オブジェクトはレイヤ原点に置かれ、バウンドの offset まで VW が
//	           「レベル Z − オブジェクト Z」で再計算した値に上書きされた。
//	  2 周目 … パス 1 点 ＋ バウンド差＝柱高さ → OIP の高さ・始端／終端オフセットは命令
//	           どおりになったが、**やはり長さ 0 で描かれなかった**。
//	  3 周目 … パス 2 点（Add3DVertex。下記）＋ 管柱はバウンド差＝柱高さ／小屋束は差 0
//	           → **管柱は正しく描かれ、小屋束だけが高さ 0** のままだった。
//	つまり VW 2026 の構造材 PIO では**バウンドの差が高さを支配し**、鉛直パスはその高さで
//	実体を作るために要る。したがって**どの柱でも「バウンドの差＝柱高さ」**にする——上端
//	offset を下端と同値（差 0）にすると高さ 0 になる（parse/Column.h 参照）。
//
//	【柱のパスは鉛直な 2 点の NURBS 曲線】M7 の横架材が使っていた 2D ポリラインでは鉛直材を
//	表せない（平面へ落とすと 1 点に潰れる）。
//	    gSDK->CreateNurbsCurve(下端, byCtrlPts=false, degree=1)   ← VS CreateNurbsCurve
//	  ＋ gSDK->Add3DVertex(曲線, 上端)                            ← VS AddVertex3D
//	で作る。**`Add3DVertex` が VS の `AddVertex3D` にあたる**（当初 `Insert3DVertex` を
//	使っていたが別物で、頂点が増えず 1 点のままだった＝上記 1 周目・2 周目の原因）。M7 の
//	コメントにあった「頂点を足す呼び出しが無い」も同じ取りこぼしで、`VWNURBSCurve` が
//	評価専用（制御点から構築できない）のは事実だが、ISDK 側に頂点を足す呼び出しがある。
//	**M7 で長さ 0 になった VWPolygon3DObj のパスは使わない。**
//
//	この経路は**横架材と共通**（draw/StructuralMember の CreatePath）。水平材も鉛直材も
//	3 次元空間の直線 1 本なので、パスの作り方は分けず、**2 点の Z の置き方**だけが要素の
//	仕様になる（柱＝下端 Z → 上端 Z、横架材＝両端とも天端 Z）。根拠は
//	draw/StructuralMember.h 冒頭。
//
//	【高さの与え方】パスの頂点は**最終位置の絶対 Z**（下端 → 下端＋柱高さ）で作る（ISDK に
//	VectorScript の Move3D が無いため。M6 / M7 と同じ作法）。上下端のストーリバウンドは命令の
//	offset をそのまま渡す——解析側が**どの柱でもバウンドの差＝柱高さ**になるように offset を
//	決めている（parse/Column.h）。
//
//	【診断を必ず持ち帰る】実描画はローカルの VectorWorks でしか確認できない。そこで
//	draw/Member と同じく、断面が入ったか・パスの頂点が 2 つになったかを**読み戻して確かめ**、
//	駄目だった本数を完了ダイアログへ返す（上記のとおり、パスが 1 点のままだと何も描かれない）。
//	**スパン（平面投影長）だけは横架材と違って数えない**——鉛直材では 0 が正常なので、
//	同じ数え方をすると全数を誤報する。
//

#include "PluginPrefix.h"
#include "draw/Column.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// プラグインスタイル名（VW 実機の登録名に一致させる）。PIO は横架材と同じ構造材ツール
		// で、スタイルだけが柱・束用に分かれる（柱・間柱ツールはスクリプトからの操作に対して
		// 不安定なので、柱も標準の構造材ツールで描く）。
		const TXString kColumnStyle("木質構造材_柱・束");

		// 柱 1 本を構造材ツールで描く。PIO を作れなければ断面の矩形でフォールバックする。
		// 何か 1 つでも配置できたら true。outObject には**構造材ツールで作れたときだけ**その
		// ハンドルを入れる（伏図記号のデータタグはこれに関連付ける。フォールバックの矩形は
		// タグを付ける相手にしない）。
		bool DrawOne(const core::ColumnCommand& column, RefNumber style,
					 std::size_t& outPathFailures, std::size_t& outSectionFailures,
					 MCObjectHandle& outObject)
		{
			// 断面の矩形（幅 × せい）は**原点中心**に置く（AxisAlign＝中央と一致させる。
			// パスが断面中心を通る）。作れなければ PIO を作らない——断面の無い構造材は
			// 生成できても実体が描かれない（draw/DrawUtil 参照）。
			const MCObjectHandle profile = CreateRectangleProfileGroup(
				-column.width / 2.0, -column.depth / 2.0, column.width / 2.0, column.depth / 2.0);

			// パス＝断面中心を通る鉛直線（下端 → 上端）。横架材と同じ CreatePath で作り、
			// **柱では 2 点の Z が異なる**（＝この差が柱の高さになる）。
			bool pathAppended = false;
			const MCObjectHandle path =
				profile == nil
					? nil
					: CreatePath(core::Vec3{column.position.x, column.position.y, column.elevation},
								 core::Vec3{column.position.x, column.position.y,
											column.elevation + column.height},
								 pathAppended);
			if (path != nil && !pathAppended)
				++outPathFailures;

			StructuralMemberSpec spec;
			spec.path = path;
			spec.profile = profile;
			spec.memberId = column.memberId;
			spec.drawClass = column.drawClass;
			// 構造用途は命令が持つ値（柱="4" / 小屋束="5"）をそのまま入れる。小屋束を柱用途に
			// すると VW の柱高さモデルで上端高さが崩れる（parse/Column.h）。
			spec.structuralUse = column.structuralUse;
			spec.width = column.width;
			spec.depth = column.depth;
			spec.axisAlign = StructuralAxisAlign::Centre; // 断面中心（鉛直パスが通る点）
			spec.startBound = column.bottomBound;		  // 始端＝下端
			spec.endBound = column.topBound;			  // 終端＝上端

			const StructuralMemberResult result = DrawStructuralMember(spec, style);
			if (result.object == nil)
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

			// 断面が入らなかった本数を数える（診断。drawColumns が完了ダイアログへ載せる）。
			// **スパン（平面投影長）は数えない**——鉛直材では 0 が正常なので、横架材と同じ
			// 数え方をすると全数を誤報する（冒頭「診断を必ず持ち帰る」）。
			if (!result.sectionOk)
				++outSectionFailures;
			outObject = result.object;
			return true;
		}
	} // namespace

	std::size_t drawColumns(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics, ObjectHandles* handles)
	{
		if (document.columns.empty())
			return 0;

		const RefNumber style = ResolvePluginStyle(kColumnStyle);

		std::size_t drawn = 0;
		std::size_t pathFailures = 0;
		std::size_t sectionFailures = 0;
		for (std::size_t index = 0; index < document.columns.size(); ++index)
		{
			const core::ColumnCommand& column = document.columns[index];

			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は本数で報告し、
			// 描画の前に 1 件進める（＝「いま何本目を描いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先の span レイヤ（"1to2-柱" 等）が無い命令はスキップする
			// （規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(column.layer) == nil)
				continue;

			MCObjectHandle object = nil;
			if (DrawOne(column, style, pathFailures, sectionFailures, object))
				++drawn;
			// 伏図記号のデータタグが引けるよう、**構造材ツールで描けた柱だけ**を記録する
			// （立上り → 壁結合と同じ受け渡し方式。draw/ObjectHandles.h）。
			if (handles != nullptr && object != nil)
				handles->table().handles.emplace(index, object);
		}

		// 全配置後に 1 回だけスタイル更新を掛けて、by-style の描画属性を反映する
		// （SetPluginObjectStyle は関連付けまでで描画属性をプッシュしない）。
		if (drawn > 0 && style != 0)
			gSDK->UpdateStyledObjects(style);

		// 診断: 実描画はローカルの VectorWorks でしか確認できないので、「鉛直パスが 2 点に
		// ならなかった」「断面が入らなかった」「スタイルが見つからなかった」を件数で持ち帰る
		// （柱が見えないときの切り分け材料）。
		if (outDiagnostics != nullptr && (pathFailures > 0 || sectionFailures > 0 || style == 0))
		{
			std::string note = "柱の診断: ";
			if (pathFailures > 0)
				note += "鉛直パスが 2 点にならなかった柱 " + std::to_string(pathFailures) + " 本。";
			if (sectionFailures > 0)
				note += "断面を設定できなかった柱 " + std::to_string(sectionFailures) + " 本。";
			if (style == 0)
				note += "プラグインスタイル『木質構造材_柱・束』が見つかりません。";
			*outDiagnostics = std::move(note);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
