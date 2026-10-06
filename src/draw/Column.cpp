//
//	draw/Column.cpp
//
//	柱描画の実装。命令セット（ColumnCommand）を**構造材ツール（StructuralMember）
//	**の鉛直材として配置する。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	描画手順:
//	  1. **パス**＝柱の建つ平面座標 (x, y)。**高さは持たない**（2 点とも同じ平面座標＝
//	     平面では 1 点に退化した 2 点の曲線。draw/StructuralMember.h 冒頭「パスは 2D で渡す」）。
//	  2. **プロファイル**＝断面の矩形（幅 × せい）を**原点中心**に置いたグループ
//	     （断面基準点は AxisAlign＝中央。draw/DrawUtil の CreateRectangleProfileGroup）。
//	  3. **PIO の生成から各フィールドの設定までは横架材と共通**（draw/StructuralMember）。
//	     ここが受け持つのは柱固有の値——パスの平面座標・断面中心基準の断面矩形・構造用途
//	     （柱／小屋束）・配置先レイヤ——だけ。**プラグインスタイルは適用しない**
//	     （描画属性はクラスに従う。draw/StructuralMember.cpp 冒頭「スタイルを使わない」）。
//	PIO を生成できない場合は断面の矩形にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【柱のパスも 2 点の曲線】横架材・垂木と同じ関数で作る（draw/StructuralMember の CreatePath）。
//	    gSDK->CreateNurbsCurve(平面座標, byCtrlPts=false, degree=1)  ← VS CreateNurbsCurve
//	  ＋ gSDK->Add3DVertex(曲線, 同じ平面座標)                       ← VS AddVertex3D
//	で作る。**`Add3DVertex` が VS の `AddVertex3D` にあたる**（当初 `Insert3DVertex` を
//	使っていたが別物で、頂点が増えず 1 点のままだった＝下記 M8 の 1 周目・2 周目の原因）。
//	**M7 で長さ 0 になった VWPolygon3DObj のパスは使わない。**
//
//	**柱は平面では 1 点に退化するので、始端と終端に同じ座標を渡す。** それでよい——高さも
//	向きもバウンドが決めるので、パスが担うのは「材がどこに立つか」だけである。**2 点で
//	あること**だけは崩さない（1 点のパスは挿入点としてしか読まれず、何も描画されない。下記
//	M8 の 1 周目・2 周目）。
//
//	【高さの与え方】**柱の高さを決めるのはパスではなく上下端のストーリバウンドだけ**で、命令の
//	offset をそのまま渡す——解析側が**どの柱でもバウンドの差＝柱高さ**になるように offset を
//	決めている（parse/Column.h）。上端 offset を下端と同値（差 0）にすると高さ 0 になる。
//
//	【診断を必ず記録する】断面が入ったか・パスの頂点が 2 つになったかを**読み戻して確かめ**、
//	失敗した本数を診断へ返す（draw/Member と同じ。パスが 1 点のままだと何も描画されない）。
//	**スパン（平面投影長）だけは横架材と違って数えない**——鉛直材では 0 が正常なので、
//	同じ数え方をすると全数を誤報する。
//	理由: 実描画はローカルの VectorWorks でしか確認できない。
//
//	【描画できたかを読み戻す】バウンドもパスも命令どおりなのに**実体が無い**ことがある（M27。
//	実機で 46 本発生）。OIP の値は正しいままなので、**画面で確認しない限り気付けない**——そこで
//	**両端の解決済み絶対 Z の差**を読み戻し（spec.expectedLength ＝ 命令のパス長）、実体が
//	無い柱を件数で診断へ載せる。この測定から原因まで追跡できた: 渡したパスの両端の Z に
//	1 ULP の丸めが残った柱だけが `ResetObject` の再構築から外れていた（上下端のバウンドを
//	疑った見立ては誤り。下記「経緯（M27）」／docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。
//
//	【経緯（M8）】M8 のローカル確認 3 周では「鉛直パスとバウンドの差の両方が要る」と読んだが、
//	**パスの側は Z を受け取られていなかった**（M27）。当時の観察を、いまの理解で読み直した
//	ものが次の 3 周である:
//	  1 周目 … パス 1 点 ＋ バウンド差 0 → OIP は「スパン 0 / 長さ 0 / 高さ 0」で何も
//	           描画されず、オブジェクトはレイヤ原点に置かれ、バウンドの offset まで VW が
//	           「レベル Z − オブジェクト Z」で再計算した値に上書きされた。
//	  2 周目 … パス 1 点 ＋ バウンド差＝柱高さ → OIP の高さ・始端／終端オフセットは命令
//	           どおりになったが、**やはり長さ 0 で描画されなかった**。
//	  3 周目 … パス 2 点（Add3DVertex。上記）＋ 管柱はバウンド差＝柱高さ／小屋束は差 0
//	           → **管柱は正しく描画され、小屋束だけが高さ 0** のままだった。
//	つまり VW 2026 の構造材 PIO では**バウンドの差が高さを支配し**、パスは**2 点あること**
//	（＝挿入点としてではなくパスとして読まれること）だけが要る。したがって**どの柱でも
//	「バウンドの差＝柱高さ」**にする（parse/Column.h 参照）。
//
//	【経緯（M27）】**かつては「パスの 2 点の Z の差が柱の高さになる」と書いていたが、それは
//	誤りだった**。`CreateCustomObjectPath` は渡したパスの Z を受け取らず、実体を与えているのは
//	`ResetObject` による解決済みバウンドからの再構築のほうである。**その再構築が働く条件が
//	「両端の Z の差がちょうど 0」だった**ので、Z はもう渡さない（渡せば 1 ULP の丸めで
//	再構築から外れる余地を残すだけ）。詳細は draw/StructuralMember.h 冒頭と
//	docs/DEV-NOTES.md M27。
//

#include "PluginPrefix.h"
#include "draw/Column.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"
#include "draw/Verify.h"
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
#if VW_DRAW_VERIFY
		// 実測（両端の絶対 Z の差）と命令の食い違いをどこまで許すか（mm）。丸めのぶんだけ。
		// **取り込み後の再検査**（recheckColumns）が使う——検算そのものなので開発ビルドだけ。
		constexpr double kExtentTol = 1.0;
#endif

		// 柱 1 本を構造材ツールで描画する。PIO を生成できなければ断面の矩形でフォールバックする。
		// 何か 1 つでも配置できたら true。outObject には**構造材ツールで生成できたときだけ**その
		// ハンドルを入れる（取り込み後の測り直し recheckColumns が参照する。フォールバックの
		// 矩形は測定の対象にしない）。失敗の内訳は failures へ数え込む（draw/StructuralMember）。
		bool DrawOne(const core::ColumnCommand& column, StructuralFailures& failures,
					 MCObjectHandle& outObject)
		{
			// 断面の矩形（幅 × せい）は**原点中心**に置く（AxisAlign＝中央と一致させる。
			// パスが断面中心を通る）。作れなければ PIO を作らない——断面の無い構造材は
			// 生成できても実体が描画されない（draw/DrawUtil 参照）。
			const MCObjectHandle profile = CreateRectangleProfileGroup(
				-column.width / 2.0, -column.depth / 2.0, column.width / 2.0, column.depth / 2.0);

			// パス＝柱が建つ平面座標。**Z は渡さない**（2 点とも同じ平面座標）。
			//
			// 構造材ツールは**2D のパスを渡されることを前提に**していて、3D 化は自分で行う。
			// そのため Z は引数ごと無くした（draw/StructuralMember.h 冒頭「パスは 2D で
			// 渡す」）——残差が出る余地が原理的に消え、同じ高さをバウンドとパスの 2 か所へ
			// 書くこともなくなる。高さはストーリバウンドが支配し、その解決は実機で命令どおり
			// だと確認済みである（バウンドの側に問題は無い。SDK リファレンス issue #59）。
			//
			// 【なぜ高さを持たせないか（M27 の結論）】以前はここで下端 Z → 上端 Z を渡し、
			// 「この差が柱の高さになる」と書いていた。**それは事実ではなかった。**実機で
			// 生成直後のパスを読み戻すと、**197 本すべてが 0 長**——`CreateCustomObjectPath`
			// は渡したパスの Z を受け取らず、柱に実体を与えているのはあとで `ResetObject` が
			// **解決済みのストーリバウンドから 3D のパスを再生成する**ほうだった
			// （docs/DEV-NOTES.md M27「実機 round 1 / round 2 の測定」）。
			//
			// そして `ResetObject` が再生成するのは**両端の Z の差がちょうど 0 のパスだけ**
			// らしい。退化したパスに丸め誤差が残って **1 ULP だけ 0 でない**パスは「呼び出し側が
			// 与えた有効なパス」と見なされて温存され、そのまま**長さ 0 の柱**になる——実機で
			// 退化していた 46 本はこれで、残差は `4.54747e-13`（2048〜4096 付近の 1 ULP）
			// だった。
			bool pathAppended = false;
#if VW_DRAW_VERIFY
			// 作成した曲線の観測（退化した 1 本目の証拠に添える。開発ビルドだけ。draw/Verify.h）。
			PathProbe probe;
			const MCObjectHandle path =
				profile == nil ? nil
							   : CreatePath(column.position, column.position, pathAppended, &probe);
#else
			const MCObjectHandle path =
				profile == nil ? nil : CreatePath(column.position, column.position, pathAppended);
#endif
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
			// 描画結果の長さ＝パス長（端部オフセットはこの長さから戻す量なので、退化して
			// いないかを確認するこの検査には要らない）。0 に退化していたら診断へ記録する。
			spec.expectedLength = column.height;
			// 【高さの検算】**描画結果の両端の絶対 Z を読み戻して命令と照合する**
			// （draw/StructuralMember.h の checkElevation）。上端は命令の下端 Z ＋ パス長
			// （core/Document.h の ColumnCommand「高さの持ち方」）。**開発ビルドだけ**
			// （draw/Verify.h）。
			// 理由: パスから Z を除いた以上、柱がどの高さに立つかはバウンドだけが決める。その
			// 解決が意図とずれても**本数には出ない**。
#if VW_DRAW_VERIFY
			spec.checkElevation = true;
			spec.expectedStartZ = column.elevation;
			spec.expectedEndZ = column.elevation + column.height;
#endif
			// **パスを再生成して差し替える対症療法は撤去した。** **退化の検出は残す**——
			// 直ったから監視を削除する、ではなく、再発したら黙って修復せずに報せるため
			// （開発ビルドだけ。draw/Verify.h）。
			// 退化していたのは「渡した 2 点の Z が違うせいで、退化したパスに 1 ULP の丸めが
			// 残り、`ResetObject` の再構築から外れていた」ためで、**Z を渡さなくなった**いま
			// 原理的に起きない（上記 CreatePath・docs/DEV-NOTES.md M27）。M27 の「両端に同じ Z
			// を渡す」でも残差は消えていて、実機 round 3 で再生成が 1 本も走らないことを確認して
			// から削除してある——Z を除いたのはその先で、渡す値そのものを無くした形である。
			// **そもそも修復する手段を残すべきでなかった**ことも後から分かった——差し替えると
			// `ResetObject` がバウンドの `fOffset` を書き換え、利用者が階高を編集した瞬間に
			// 長さとして表に出る（draw/Member ／ docs/DEV-NOTES.md M27）。
			const StructuralMemberResult result = DrawStructuralMember(spec);
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

			// 失敗の内訳を数え込む（診断。drawColumns が診断へ載せる）。
			// **スパン（平面投影長）は数えない**——鉛直材では 0 が正常なので、横架材と同じ
			// 数え方をすると全数を誤報する（冒頭「診断を必ず記録する」）。
			// 高さ基準（bound）は**書けたことを実機で確かめた**うえで実体 0 の柱があったので
			// 原因ではないが、監視として数え続ける（docs/DEV-NOTES.md M27）。
#if VW_DRAW_VERIFY
			const bool firstProbe = failures.collapsedProbe.empty();
#endif
			failures.record(result);
#if VW_DRAW_VERIFY
			// 退化した 1 本目には、**渡した曲線の観測**を先頭へ追加する。
			//
			// **「あるべき高さ」は渡したパスの値ではない。** M27 以降パスは平面（Z=0）の
			// 2 点なので、ここに出す Z 範囲は「命令が意図している高さ」——すなわちバウンドへ
			// 渡した値——であって、パスの入力ではない。読む人が取り違えないよう文言で明記して
			// おく。作成した曲線の Z は**0 が正常**で、0 でなければパスの作り方が冒頭の約束から
			// 外れている。
			if (result.collapsed && firstProbe)
			{
				std::array<char, 288> buffer{};
				std::snprintf(buffer.data(), buffer.size(),
							  "パスの頂点 piece0=%d piece1=%d・作った曲線の Z %s(%g→%g。"
							  "0 が正常)・あるべき高さ %g（バウンドへ渡した Z 範囲 %g→%g。"
							  "パスは平面なので、これは入力そのものではない）・OIP ",
							  static_cast<int>(probe.piece0), static_cast<int>(probe.piece1),
							  probe.pointsRead ? "" : "読めない", probe.z0, probe.z1, column.height,
							  column.elevation, column.elevation + column.height);
				failures.collapsedProbe = std::string(buffer.data()) + failures.collapsedProbe;
			}
#endif

			outObject = result.object;
			return true;
		}
	} // namespace

	std::size_t drawColumns(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics, ObjectHandles* handles)
	{
		if (document.columns.empty())
			return 0;

		std::size_t drawn = 0;
		StructuralFailures failures;
		for (std::size_t index = 0; index < document.columns.size(); ++index)
		{
			const core::ColumnCommand& column = document.columns[index];

			if (!AdvanceProgress(progress))
				break;

			// 配置先の span レイヤ（"1to2-柱" 等）が無い命令はスキップする
			// （規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(column.layer) == nil)
				continue;

			MCObjectHandle object = nil;
			if (DrawOne(column, failures, object))
				++drawn;
			// 取り込み後の測り直し（recheckColumns）が参照できるよう、**構造材ツールで描画できた
			// 柱だけ**を記録する（立上り → 壁結合と同じ受け渡し方式。draw/ObjectHandles.h）。
			if (handles != nullptr && object != nil)
				handles->table().handles.emplace(index, object);
		}

		// 診断: 実描画はローカルの VectorWorks でしか確認できないので、失敗の内訳を件数で
		// 記録する（文言は draw/StructuralMember。柱が表示されないときの切り分け材料になる）。
		const std::string note = DescribeStructuralFailures(failures, "柱");
		if (outDiagnostics != nullptr && !note.empty())
			*outDiagnostics = "柱の診断: " + note;

		return drawn;
	}

#if VW_DRAW_VERIFY
	void recheckColumns(const core::Document& document, const ObjectHandles& handles,
						std::string* outDiagnostics, std::string* outNotes)
	{
		if (document.columns.empty())
			return;

		std::size_t measured = 0;  // 測れた本数（両端の絶対 Z を取得できた本数）
		std::size_t collapsed = 0; // そのうち実体が 0 だった本数
		std::size_t differs = 0;   // 実体はあるが命令と食い違う本数
		std::string probe; // 1 本目の実測（どのパラメータが何を返しているか）
		std::string oddProbe; // 食い違った／退化した 1 本目の実測
		// **同じレイヤの正常な柱**の実測。実体が無い柱と比較する相手は、**同じ span
		// レイヤ（＝同じストーリ・同じレベル）の柱**でなければ意味が無い——1 本目の柱は
		// 別のレイヤの通し柱だったりするので、それと比べても差が多すぎて何も言えない。
		std::string peerLayer; // 最初に退化していた柱のレイヤ
		std::size_t peerIndex = 0; // その比較相手（同じレイヤで正常だった柱）の命令インデックス
		bool peerFound = false;

		for (const auto& [index, object] : handles.table().handles)
		{
			if (index >= document.columns.size() || object == nil)
				continue;
			// 1 本目だけ、長さ・高さを含むパラメータを名前と値で控える（全数だと読めない）。
			if (probe.empty())
				probe =
					DescribeSizeParams(object) + "・図面のパス[" + DescribePioPath(object) + "]";

			const DrawnMemberSize size = MeasureDrawnMember(object, StructuralExtentKind::Vertical);
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

			// 同じレイヤで**正常だった**柱を 1 本覚える（退化した柱の比較相手。下で実測を採る）。
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
				// 無い柱で原因の分岐点になるのはここだけである——同じなら record は入って
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
						   DescribeStoryBound(object, StoryBoundSlot::Start) + "]・終端[" +
						   DescribeStoryBound(object, StoryBoundSlot::End) + "]・図面のパス[" +
						   DescribePioPath(object) + "]";
			}
		}

		// 比較相手は退化した柱より前に並んでいることもあるので、見つからなければもう一度探す
		// （命令の順に回るので、1 巡目では「退化した柱より後ろ」しか検出できない）。
		if (!peerLayer.empty() && !peerFound)
		{
			for (const auto& [index, object] : handles.table().handles)
			{
				if (index >= document.columns.size() || object == nil)
					continue;
				if (document.columns[index].layer != peerLayer)
					continue;
				const DrawnMemberSize size =
					MeasureDrawnMember(object, StructuralExtentKind::Vertical);
				if (!size.found || size.zero)
					continue;
				peerIndex = index;
				peerFound = true;
				break;
			}
		}
		// 比較相手の実測（命令の値・図面の高さ基準・図面のパス）。
		// **退化した柱との違いはここに出る。**
		std::string peerProbe;
		if (peerFound)
		{
			const auto entry = handles.table().handles.find(peerIndex);
			if (entry != handles.table().handles.end() && entry->second != nil)
			{
				const core::ColumnCommand& peer = document.columns[peerIndex];
				const DrawnMemberSize size =
					MeasureDrawnMember(entry->second, StructuralExtentKind::Vertical);
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
			AppendCount(note, "実体が無い柱", collapsed, "本");
			AppendCount(note, "実体が命令と食い違う柱", differs, "本");
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
#endif // VW_DRAW_VERIFY
} // namespace HomeskzIfcImport::draw
