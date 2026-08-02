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
//	  1. **パス**＝天端中央線の始端→終端を頂点に持つ「開いた 3D ポリライン」。頂点の XY は
//	     命令のセンタリング済み絶対座標、**Z は両端とも命令の始端天端 elevation**。
//	  2. **プロファイル**＝断面の矩形（幅 × せい）をグループに入れたもの。**空のグループを
//	     渡してはならない**（断面が無いのと同じで、PIO は生成できても実体が描かれず
//	     「オブジェクトはあるのに画面に何も出ない」状態になる。CreateProfileGroup 参照）。
//	  3. CreateCustomObjectPath('StructuralMember', path, profile) で PIO を生成する。
//	  4. クラス分け → プラグインスタイル（木質構造材_横架材）の関連付け → 個別フィールド
//	     （構造材 ID・断面寸法・種別）の設定 → ResetObject。
//	  5. 全配置後に UpdateStyledObjects を 1 回（下記「スタイルは関連付けだけでは効かない」）。
//	PIO を生成できない場合は平面投影の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【パスに傾斜を持たせない】始端／終端の高さ（傾斜梁の勾配）は **SetObjectStoryBound の
//	offset 差だけ**で表し、パスの 2 頂点は同じ Z にする。構造材ツールの高さバインドは指定した
//	高さ差をパス由来の部材長に**加算**するため、パスにも傾斜を持たせると傾斜が二重に適用され、
//	終端が実際の 2 倍の高さに描かれる（Python 版が柱の二重加算 #54 と同種の問題として実機で
//	確認済み。水平梁は差が 0 なので顕在化しない）。
//
//	【高さの与え方】Python 版は「ローカル原点にパスを作る → Move3D(x1, y1, z1)」の 2 手順だが、
//	ISDK には VectorScript の 3D 変換状態（Move3D）が無い。代わりに**パスの頂点そのものを
//	最終位置（XY は絶対座標・Z は始端天端）で作る**——これは Move3D 後の状態と同じで、
//	draw/Grid が GridAxis のパスを絶対座標で渡しているのと同じ作法。**Z が絶対か
//	レイヤ相対かはローカル確認項目**で、切り替えは下の kPathZ の 1 か所で済む
//	（M6 の垂木・野地板と同じ扱い。ROADMAP.md M6/M7「ローカル確認」）。
//
//	【パラメータは名前解決してから書き、読み戻して確かめる】断面寸法が入らないと材のせいが
//	0 になり、オブジェクトはあるのに画面に出ない。PIO のパラメータは universal 名が 1 つ違う
//	だけで setter が黙って無視され、しかも数値パラメータが実数ではなく文字列で保持されている
//	ことがある（M6 の垂木で両方に遭遇）。そこで draw/DrawUtil の ResolveParamName で名前を
//	解決し、SetParamRealChecked で書いた値を読み戻して確認し、実数で入らなければ文字列で
//	入れ直す。それでも入らなかった本数は診断として完了ダイアログへ返す（drawMembers）。
//
//	【スタイルは関連付けだけでは効かない】ISDK の SetPluginObjectStyle はスタイルの関連付け
//	（パラメータ）までで、スタイルが決める描画属性（コンポーネントのクラス／マテリアル＝
//	テクスチャ等）はオブジェクトへプッシュされない（Python 版 #56 と同じ）。そこで全配置後に
//	UpdateStyledObjects を 1 回呼び、当該スタイルの全オブジェクトをスタイルから更新する
//	（by-instance の個別フィールド＝寸法・構造材 ID 等は保持したまま by-style の描画属性だけが
//	更新される）。個別フィールドはスタイル関連付けの**後**に設定するので、スタイル既定の
//	パラメータは本命令の実測値で上書きされる。
//
//	【スタイルの RefNumber】ISDK はスタイル名から RefNumber を引く呼び出しを持たないので、
//	名前付きオブジェクト（プラグインスタイルはシンボル定義）を GetNamedObject で引き、その
//	InternalIndex を RefNumber として渡す（どちらも SysName を表す Sint32。SDK ヘッダでも
//	InternalIndex と RefNumber は相互に渡し合う形で使われている）。スタイルが文書に無ければ
//	関連付けを飛ばし、素の構造材として描く（スタイルの欠落で梁を失わない）。
//

#include "PluginPrefix.h"
#include "draw/Member.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWGroupObj.h"
#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"
#include "VWFC/VWObjects/VWPolygon3DObj.h"

#include <cmath>
#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 構造材ツールの PIO 名とプラグインスタイル名（VW 実機の登録名に一致させる）。
		const TXString kStructuralMember("StructuralMember");
		const TXString kMemberStyle("木質構造材_横架材");

		// SetObjectStoryBound に渡すバウンド ID。構造材は始端＝0・終端＝1 の 2 つを持つ
		// （Python 版 vw/member.py と同じ規約）。型は SDK の TObjectBoundID（= Sint32）だが、
		// その別名は SDK の名前空間の中にあるため実体の Sint32 で持つ（暗黙変換で同じ）。
		constexpr Sint32 kStartBoundID = 0;
		constexpr Sint32 kEndBoundID = 1;

		// 構造材ツールのフィールド名（Python 版 vw/member.py の SetRField と同じ universal 名）。
		// **名前が 1 つ違うだけで setter は黙って無視される**（M6 の垂木で実証済み。
		// draw/Rafter.cpp 冒頭）ので、実機で反映を確認するのがローカル確認項目。
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
		constexpr const char* kMemberTypeBeam = "2";	// 梁
		constexpr const char* kStructuralUseBeam = "1"; // 横架材
		constexpr const char* kAxisAlignTopCentre = "1"; // 天端中央（命令の基準点と一致）
		constexpr const char* kEndConditionSquare = "3"; // 直切り
		constexpr const char* kProfileSeriesDefault = "AISC (Inch)";

		// プラグインスタイル（木質構造材_横架材）の RefNumber。文書に無ければ 0（＝スタイル
		// 無しで描く）。冒頭「スタイルの RefNumber」参照。
		RefNumber ResolveMemberStyle()
		{
			MCObjectHandle style = gSDK->GetNamedObject(kMemberStyle);
			if (style == nil || !gSDK->IsPluginStyle(style))
				return 0;
			return static_cast<RefNumber>(gSDK->GetObjectInternalIndex(style));
		}

		// 命令の高さ基準（StoryBoundCommand）を SDK の構造体へ写す。
		VectorWorks::SStoryObjectData StoryBoundOf(const core::StoryBoundCommand& bound)
		{
			VectorWorks::SStoryObjectData data;
			data.fBound = VectorWorks::eStoryObjectBound_Story;
			data.fBoundStory = static_cast<Sint8>(bound.storyOffset);
			data.fLayerLevelType = TXString(bound.level.c_str());
			data.fOffset = bound.offset;
			return data;
		}

		// 断面の矩形（幅 × せい）をプロファイルグループとして作る（Python 版の
		// BeginGroup / ClosePoly / Poly(0,0, 0,h, w,h, w,0) / EndGroup に対応）。
		//
		// **グループへは VWFC の VWGroupObj::AddObject で入れる**。当初は
		// gSDK->AddObjectToContainer を直に呼んでいたが、その場合ポリゴンは「レイヤに
		// 作ってから移す」形になり、移動に失敗すると**空のグループ**が残る。空のプロファイルは
		// 断面が無いのと同じで、PIO は生成できても実体が描かれない（＝オブジェクトはあるのに
		// 画面に何も出ない）。入ったかどうかは GetFirstMemberObject で確かめ、空なら nil を
		// 返して呼び出し側にフォールバックさせる。
		MCObjectHandle CreateProfileGroup(double width, double height)
		{
			if (width <= 0.0 || height <= 0.0)
				return nil;

			VWPolygon2DObj profile({VWPoint2D(0.0, 0.0), VWPoint2D(0.0, height),
									VWPoint2D(width, height), VWPoint2D(width, 0.0)});
			profile.SetClosed(true);
			const MCObjectHandle profileHandle = profile.GetThisObject();
			if (profileHandle == nil)
				return nil;

			VWGroupObj group;
			group.AddObject(profileHandle);
			const MCObjectHandle groupHandle = group.GetThisObject();
			if (groupHandle == nil)
				return nil;
			// 断面が本当に入ったか（空のグループを渡さない）。
			if (VWGroupObj(groupHandle).GetFirstMemberObject() == nil)
				return nil;
			return groupHandle;
		}

		// 横架材 1 本を構造材ツールで描く。PIO を作れなければ平面投影の直線でフォールバック
		// する。何か 1 つでも配置できたら true。
		bool DrawOne(const core::MemberCommand& member, RefNumber style,
					 std::size_t& outSectionFailures)
		{
			// パスの Z（両端で同じ）。傾斜は高さバインドの offset 差だけで表す
			// （冒頭「パスに傾斜を持たせない」）。絶対 Z かレイヤ相対かの切り替えはここ 1 か所。
			const double kPathZ = member.elevation;

			VWPolygon3DObj path({VWPoint3D(member.start.x, member.start.y, kPathZ),
								 VWPoint3D(member.end.x, member.end.y, kPathZ)});
			path.SetClosed(false); // ポリゴン（閉）でなくポリライン（開）
			const MCObjectHandle pathHandle = path.GetThisObject();
			if (pathHandle == nil)
				return false;

			// 断面（プロファイルグループ）を先に用意する。作れなければ PIO を作らない
			// ——断面の無い構造材は生成できても実体が描かれず、「オブジェクトはあるのに
			// 画面に出ない」状態になるだけなので、直線のフォールバックの方が有用。
			const MCObjectHandle profileGroup =
				CreateProfileGroup(member.width, member.height);
			MCObjectHandle object =
				profileGroup == nil
					? nil
					: gSDK->CreateCustomObjectPath(kStructuralMember, pathHandle, profileGroup, true);
			if (object == nil)
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

			SetClassByName(object, member.drawClass);
			SetAllAttributesByClass(object);
			// スタイルは個別フィールドより**先に**関連付ける（後に設定する実測値で
			// スタイル既定のパラメータを上書きするため）。
			if (style != 0)
				gSDK->SetPluginObjectStyle(object, style);

			// 高さ基準を始端（0）・終端（1）それぞれのストーリレベルへバインドする。これで
			// 構造材ツールの高さ基準が「レイヤの高さ」・offset 0 のまま実ジオメトリと矛盾する
			// ことがなくなり、編集時に高さがリセットされない。傾斜はこの offset 差で表れる。
			gSDK->SetObjectStoryBound(object, kStartBoundID, StoryBoundOf(member.startBound));
			gSDK->SetObjectStoryBound(object, kEndBoundID, StoryBoundOf(member.endBound));

			// パラメータは**名前を解決してから**書き、寸法は**読み戻して確かめる**
			// （名前が 1 つ違うだけで setter は黙って無視される。M6 の垂木で実証済み。
			// draw/DrawUtil の ResolveParamName / SetParamRealChecked）。
			VWParametricObj pio(object);
			const TXString breadth = ResolveParamName(pio, kFieldMajorBreadth, kLocalizedBreadth);
			const TXString depth = ResolveParamName(pio, kFieldMajorDepth, kLocalizedDepth);

			pio.SetParamAsString(ResolveParamName(pio, kFieldProfileShape, kLocalizedProfileShape),
								 kProfileShapeRectangle);
			pio.SetParamAsString(kFieldProfileSeries, kProfileSeriesDefault);
			const bool breadthOk = SetParamRealChecked(pio, breadth, member.width);
			const bool depthOk = SetParamRealChecked(pio, depth, member.height);
			// B / D は矩形断面のときの別名。上と同じ値を入れる（存在しなければ無視される）。
			SetParamRealChecked(pio, ResolveParamName(pio, kFieldB, kLocalizedBreadth),
								member.width);
			SetParamRealChecked(pio, ResolveParamName(pio, kFieldD, kLocalizedDepth),
								member.height);
			pio.SetParamAsString(kFieldMemberID, TXString(member.memberId.c_str()));
			pio.SetParamAsString(kFieldMemberType, kMemberTypeBeam);
			pio.SetParamAsString(kFieldStructuralUse, kStructuralUseBeam);
			pio.SetParamAsString(kFieldAxisAlign, kAxisAlignTopCentre);
			pio.SetParamAsString(kFieldStartCondition, kEndConditionSquare);
			pio.SetParamAsString(kFieldEndCondition, kEndConditionSquare);
			gSDK->ResetObject(object);

			// 断面が入らなかった本数を数える（診断。drawMembers が完了ダイアログへ載せる）。
			if (!breadthOk || !depthOk)
				++outSectionFailures;
			return true;
		}
	} // namespace

	std::size_t drawMembers(const core::Document& document, std::string* outDiagnostics)
	{
		if (document.members.empty())
			return 0;

		const RefNumber style = ResolveMemberStyle();

		std::size_t drawn = 0;
		std::size_t sectionFailures = 0;
		for (const core::MemberCommand& member : document.members)
		{
			// 配置先レイヤ（"n-横架材天端" / "R-軒高" / "n-母屋" / "n-登り梁"）が無い命令は
			// スキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(member.layer) == nil)
				continue;

			if (DrawOne(member, style, sectionFailures))
				++drawn;
		}

		// 全配置後に 1 回だけスタイル更新を掛けて、by-style の描画属性を反映する
		// （冒頭「スタイルは関連付けだけでは効かない」）。
		if (drawn > 0 && style != 0)
			gSDK->UpdateStyledObjects(style);

		// 診断: 実描画はローカルの VectorWorks でしか確認できないので、「作れたが断面が
		// 入らなかった」「スタイルが見つからなかった」を件数で持ち帰る。横架材が 1 本も
		// 見えないときに、原因が命令側（解析）か PIO のパラメータ側かを切り分けられる。
		if (outDiagnostics != nullptr && (sectionFailures > 0 || style == 0))
		{
			std::string note = "横架材の診断: ";
			if (sectionFailures > 0)
				note += "断面を設定できなかった材 " + std::to_string(sectionFailures) + " 本。";
			if (style == 0)
				note += "プラグインスタイル『木質構造材_横架材』が見つかりません。";
			*outDiagnostics = std::move(note);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
