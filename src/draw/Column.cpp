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
//	【描けたかを読み戻す】バウンドもパスも命令どおりなのに**実体が無い**ことがある（M27。
//	実機で 46 本発生）。OIP の値は正しいままなので、**画面を見ない限り気付けない**——そこで
//	**両端の解決済み絶対 Z の差**を読み戻し（spec.expectedLength ＝ 命令のパス長）、実体が
//	無い柱を件数で診断へ載せる。この測定で原因が分かった: 上下端のバウンドが「階だけ違う
//	同じ記録」になった柱だけ、終端が始端と同じ Z に解決されていた（解析側で潰してある。
//	parse/Column.h ／ docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。
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

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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
		// 診断の集計（drawColumns が完了ダイアログ・診断ログへ載せる件数）。
		struct ColumnFailures
		{
			std::size_t path = 0;	 // 鉛直パスが 2 点にならなかった
			std::size_t section = 0; // 断面（主幅・主せい）が入らなかった
			std::size_t offset = 0;	 // 端部オフセットを書けなかった
			std::string offsetHint; // 端部オフセットのパラメータ名の手掛かり（最初の 1 件）
			std::size_t bound = 0; // 高さ基準を VW が受け取らなかった
			std::size_t collapsed = 0; // 生成できたのに長さ 0 で描かれた（実体が無い）
			std::string lengthHint; // 「長さ」のパラメータ名の手掛かり（最初の 1 件）
			// 潰れた 1 本目の実測（パスの頂点数・OIP の高さと長さ・命令のパス長・図面が
			// 持っている高さ基準・図面のパスの頂点）。**原因をパス側と高さ基準側に分けるのは
			// この 1 行だけ**なので、必ず持ち帰る。
			std::string collapsedProbe;
		};

		// 実測（両端の絶対 Z の差）と命令の食い違いをどこまで許すか（mm）。丸めのぶんだけ。
		constexpr double kExtentTol = 1.0;

		bool DrawOne(const core::ColumnCommand& column, RefNumber style, ColumnFailures& failures,
					 MCObjectHandle& outObject, bool& outCollapsed)
		{
			outCollapsed = false;
			// 断面の矩形（幅 × せい）は**原点中心**に置く（AxisAlign＝中央と一致させる。
			// パスが断面中心を通る）。作れなければ PIO を作らない——断面の無い構造材は
			// 生成できても実体が描かれない（draw/DrawUtil 参照）。
			const MCObjectHandle profile = CreateRectangleProfileGroup(
				-column.width / 2.0, -column.depth / 2.0, column.width / 2.0, column.depth / 2.0);

			// パス＝断面中心を通る鉛直線（下端 → 上端）。横架材と同じ CreatePath で作り、
			// **柱では 2 点の Z が異なる**（＝この差が柱の高さになる）。
			bool pathAppended = false;
			PathProbe probe;
			const MCObjectHandle path =
				profile == nil
					? nil
					: CreatePath(core::Vec3{column.position.x, column.position.y, column.elevation},
								 core::Vec3{column.position.x, column.position.y,
											column.elevation + column.height},
								 pathAppended, &probe);
			if (path != nil && !pathAppended)
				++failures.path;

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
			// 端部オフセット（負値）。上端は受ける横架材の天端＝その芯線に取ってあるので、
			// 梁せいぶんをここで戻す（core/Document.h「端部オフセット」）。
			spec.startOffset = column.startOffset;
			spec.endOffset = column.endOffset;
			// 描き上がりの長さ＝パス長（端部オフセットはこの長さから戻す量なので、潰れて
			// いないかを見るこの検査には要らない）。0 で潰れていたら診断へ持ち帰る。
			spec.expectedLength = column.height;
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
				++failures.section;
			// 高さ基準を図面へ書けなかった本数（`SetObjectStoryBound` の戻り値）。
			// **書けたことは実機で確かめた**——それでも実体が 0 の柱があったので、高さ基準は
			// 犯人ではない（docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。見張りとして
			// 数え続ける。
			if (!result.boundOk)
				++failures.bound;
			// 端部オフセットを書けなかった本数。書けないと柱が受ける梁の天端まで伸びたまま
			// 描かれる（＝梁せいぶん高い）ので、切り分けの手掛かりを 1 件だけ残す。
			if (!result.endOffsetOk)
			{
				++failures.offset;
				if (failures.offsetHint.empty())
					failures.offsetHint = result.offsetParamHint;
			}
			// 長さ 0 で描かれた本数（オブジェクトは在るのに実体が無い）。**これが 0 でない
			// 限り、件数が揃っていても絵は欠けている**ので、必ず診断へ載せる。
			if (result.collapsed)
			{
				++failures.collapsed;
				outCollapsed = true;
				// 1 本目だけ実測を控える（全数ぶん並べても読めない）。
				if (failures.collapsedProbe.empty())
				{
					std::array<char, 192> buffer{};
					std::snprintf(buffer.data(), buffer.size(),
								  "パスの頂点 piece0=%d piece1=%d・作った曲線の Z %s(%g→%g)・"
								  "命令のパス長 %g（Z %g→%g）・OIP ",
								  static_cast<int>(probe.piece0), static_cast<int>(probe.piece1),
								  probe.pointsRead ? "" : "読めない", probe.z0, probe.z1,
								  column.height, column.elevation,
								  column.elevation + column.height);
					failures.collapsedProbe = std::string(buffer.data()) + result.collapsedProbe;
				}
			}
			if (failures.lengthHint.empty())
				failures.lengthHint = result.lengthParamHint;
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
		ColumnFailures failures;
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
			bool collapsed = false;
			if (DrawOne(column, style, failures, object, collapsed))
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
		if (outDiagnostics != nullptr &&
			(failures.path > 0 || failures.section > 0 || failures.offset > 0 ||
			 failures.bound > 0 || failures.collapsed > 0 || !failures.lengthHint.empty() ||
			 style == 0))
		{
			std::string note = "柱の診断: ";
			if (failures.path > 0)
				note +=
					"鉛直パスが 2 点にならなかった柱 " + std::to_string(failures.path) + " 本。";
			if (failures.section > 0)
				note += "断面を設定できなかった柱 " + std::to_string(failures.section) + " 本。";
			if (failures.bound > 0)
				note +=
					"高さ基準を図面へ書けなかった柱 " + std::to_string(failures.bound) + " 本。";
			if (failures.collapsed > 0)
			{
				note += "長さ 0 で描かれた（実体が無い）柱 " + std::to_string(failures.collapsed) +
						" 本。";
				if (!failures.collapsedProbe.empty())
					note += "（1 本目: " + failures.collapsedProbe + "）";
			}
			if (!failures.lengthHint.empty())
				note +=
					"「長さ」パラメータを引けませんでした（候補: " + failures.lengthHint + "）。";
			if (failures.offset > 0)
			{
				note += "端部オフセットを設定できなかった柱 " + std::to_string(failures.offset) +
						" 本。";
				if (!failures.offsetHint.empty())
					note += "（候補: " + failures.offsetHint + "）";
			}
			if (style == 0)
				note += "プラグインスタイル『木質構造材_柱・束』が見つかりません。";
			*outDiagnostics = std::move(note);
		}

		return drawn;
	}

	void recheckColumns(const core::Document& document, const ObjectHandles& handles,
						std::string* outDiagnostics, std::string* outNotes)
	{
		if (document.columns.empty())
			return;

		std::size_t measured = 0;  // 測れた本数（両端の絶対 Z を引けた本数）
		std::size_t collapsed = 0; // そのうち実体が 0 だった本数
		std::size_t differs = 0;   // 実体はあるが命令と食い違う本数
		std::string probe; // 1 本目の実測（どのパラメータが何を返しているか）
		std::string oddProbe; // 食い違った／潰れた 1 本目の実測
		// **同じレイヤの無事な柱**の実測。実体が無い柱と引き比べる相手は、**同じ span
		// レイヤ（＝同じストーリ・同じレベル）の柱**でなければ意味が無い——1 本目の柱は
		// 別のレイヤの通し柱だったりするので、それと比べても差が多すぎて何も言えない。
		std::string peerLayer; // 最初に潰れていた柱のレイヤ
		std::size_t peerIndex = 0; // その相棒（同じレイヤで無事だった柱）の命令インデックス
		bool peerFound = false;

		for (const auto& [index, object] : handles.table().handles)
		{
			if (index >= document.columns.size() || object == nil)
				continue;
			// 1 本目だけ、長さ・高さを含むパラメータを名前と値で控える（全数だと読めない）。
			if (probe.empty())
				probe =
					DescribeSizeParams(object) + "・図面のパス[" + DescribePioPath(object) + "]";

			const DrawnMemberSize size = MeasureDrawnMember(object);
			if (!size.found)
				continue;
			++measured;

			const core::ColumnCommand& column = document.columns[index];
			// 実体がどれだけあれば命令どおりか。パス長そのものか、端部オフセットを戻した
			// 「材の端」までか——**どちらを指すかは実機でしか分からない**ので、どちらかに
			// 合っていれば食い違いとは言わない（core/Document.h「端部オフセット」）。
			const double drawn = column.height + column.startOffset + column.endOffset;
			const bool matches = std::abs(size.extent - column.height) < kExtentTol ||
								 std::abs(size.extent - drawn) < kExtentTol;
			if (size.zero)
				++collapsed;
			else if (!matches)
				++differs;

			// 同じレイヤで**無事だった**柱を 1 本覚える（潰れた柱の相棒。下で実測を採る）。
			if (!size.zero && matches && !peerLayer.empty() && !peerFound &&
				column.layer == peerLayer)
			{
				peerIndex = index;
				peerFound = true;
			}
			if ((size.zero || !matches) && oddProbe.empty())
			{
				peerLayer = column.layer;
				std::array<char, 192> buffer{};
				std::snprintf(buffer.data(), buffer.size(),
							  "命令 %g（端部オフセットを戻して %g）に対し実測 %g（Z %g→%g）・",
							  column.height, drawn, size.extent, size.start, size.end);
				// **命令の高さ基準と、VW が実際に持っている高さ基準を並べる。** 実体が
				// 無い柱で分かれ道になるのはここだけである——同じなら record は入って
				// いて解決の側が違い、違えば書けていない（DrawUtil の DescribeStoryBound）。
				std::array<char, 192> wanted{};
				std::snprintf(wanted.data(), wanted.size(),
							  "・命令の始端[階=%+d レベル=\"%s\" offset=%g]・終端[階=%+d "
							  "レベル=\"%s\" offset=%g]",
							  column.bottomBound.storyOffset, column.bottomBound.level.c_str(),
							  column.bottomBound.offset, column.topBound.storyOffset,
							  column.topBound.level.c_str(), column.topBound.offset);
				oddProbe = std::string(buffer.data()) + DescribeSizeParams(object) +
						   std::string(wanted.data()) + "・図面の始端[" +
						   DescribeStoryBound(object, kStartBoundID) + "]・終端[" +
						   DescribeStoryBound(object, kEndBoundID) + "]・図面のパス[" +
						   DescribePioPath(object) + "]";
			}
		}

		// 相棒は潰れた柱より前に並んでいることもあるので、見つからなければもう一度探す
		// （命令の順に回るので、1 周目では「潰れた柱より後ろ」しか拾えない）。
		if (!peerLayer.empty() && !peerFound)
		{
			for (const auto& [index, object] : handles.table().handles)
			{
				if (index >= document.columns.size() || object == nil)
					continue;
				if (document.columns[index].layer != peerLayer)
					continue;
				const DrawnMemberSize size = MeasureDrawnMember(object);
				if (!size.found || size.zero)
					continue;
				peerIndex = index;
				peerFound = true;
				break;
			}
		}
		// 相棒の実測（命令の値・図面の高さ基準・図面のパス）。**潰れた柱との違いはここに出る。**
		std::string peerProbe;
		if (peerFound)
		{
			const auto entry = handles.table().handles.find(peerIndex);
			if (entry != handles.table().handles.end() && entry->second != nil)
			{
				const core::ColumnCommand& peer = document.columns[peerIndex];
				const DrawnMemberSize size = MeasureDrawnMember(entry->second);
				std::array<char, 256> buffer{};
				std::snprintf(buffer.data(), buffer.size(),
							  "同じレイヤ（%s）で無事だった柱: 命令のパス長 %g・端部オフセット "
							  "%g・実測 %g（Z %g→%g）・命令の終端[階=%+d レベル=\"%s\" "
							  "offset=%g]",
							  peer.layer.c_str(), peer.height, peer.endOffset, size.extent,
							  size.start, size.end, peer.topBound.storyOffset,
							  peer.topBound.level.c_str(), peer.topBound.offset);
				peerProbe = std::string(buffer.data()) + "・図面のパス[" +
							DescribePioPath(entry->second) + "]";
			}
		}

		if (outDiagnostics != nullptr && (collapsed > 0 || differs > 0))
		{
			std::string note = "柱の診断（取り込み後）: ";
			if (collapsed > 0)
				note += "実体が無い柱 " + std::to_string(collapsed) + " 本。";
			if (differs > 0)
				note += "実体が命令と食い違う柱 " + std::to_string(differs) + " 本。";
			if (!oddProbe.empty())
				note += "（1 本目: " + oddProbe + "）";
			if (!peerProbe.empty())
				note += "（" + peerProbe + "）";
			*outDiagnostics = std::move(note);
		}
		if (outNotes != nullptr)
		{
			std::string note = "柱の実測（取り込み後）: 測れた " + std::to_string(measured) +
							   " / " + std::to_string(document.columns.size()) + " 本。";
			note += probe.empty() ? "長さ・高さのパラメータを 1 つも引けませんでした。"
								  : "1 本目 " + probe;
			*outNotes = std::move(note);
		}
	}
} // namespace HomeskzIfcImport::draw
