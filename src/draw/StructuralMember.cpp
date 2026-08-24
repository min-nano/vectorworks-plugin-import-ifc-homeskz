//
//	draw/StructuralMember.cpp
//
//	構造材ツール（StructuralMember PIO）共通ヘルパーの実装。呼ぶ SDK API はいずれも
//	従来 draw/Member.cpp・draw/Column.cpp が個別に持っていたものと同一で、集約しただけ
//	（振る舞いは変えない）。設計の意図・パスを共通化しない理由はヘッダ冒頭を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【パラメータは名前解決してから書き、読み戻して確かめる】断面寸法が入らないと材のせいが
//	0 になり、オブジェクトはあるのに画面に出ない。PIO のパラメータは universal 名が 1 つ違う
//	だけで setter が黙って無視され、しかも数値パラメータが実数ではなく文字列で保持されている
//	ことがある（M6 の垂木で両方に遭遇）。そこで draw/DrawUtil の ResolveParamName で名前を
//	解決し、SetParamRealChecked で書いた値を読み戻して確認する。入らなかった本数は
//	StructuralMemberResult::sectionOk 経由で呼び出し側の診断へ流す。
//
//	【スタイルは関連付けだけでは効かない】ISDK の SetPluginObjectStyle はスタイルの関連付け
//	（パラメータ）までで、スタイルが決める描画属性（コンポーネントのクラス／マテリアル）は
//	オブジェクトへプッシュされない（Python 版 #56 と同じ）。全配置後に UpdateStyledObjects を
//	1 回呼ぶのは呼び出し側の責務（横架材・柱でスタイルが別なので、ここでは行わない）。
//	個別フィールドはスタイル関連付けの**後**に設定するので、スタイル既定のパラメータは
//	本命令の実測値で上書きされる。
//

#include "PluginPrefix.h"
#include "draw/StructuralMember.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 構造材ツールの PIO 名（VW 実機の登録名に一致させる）。柱も横架材もこの 1 つの
		// ツールで描く（柱・間柱ツールはスクリプト操作に対して不安定なため、Python 版が
		// 標準の構造材ツールへ置き換えた判断をそのまま引き継ぐ。parse/Column.h）。
		const TXString kStructuralMember(kStructuralMemberPlugin);

		// SetObjectStoryBound に渡すバウンド ID。構造材は始端＝0・終端＝1 の 2 つを持つ
		// （Python 版 vw/member.py と同じ規約。柱では下端＝0・上端＝1 に対応する）。
		// 型は SDK の TObjectBoundID（= Sint32）だが、その別名は SDK の名前空間の中にあるため
		// 実体の Sint32 で持つ（暗黙変換で同じ）。
		constexpr Sint32 kStartBoundID = 0;
		constexpr Sint32 kEndBoundID = 1;

		// 鉛直パス（NURBS 曲線）の次数。直線 1 本なので 1。byCtrlPts=false ＝ 通過点で
		// 定義する（Python 版 CreateNurbsCurve と同じ引数）。
		constexpr short kPathDegree = 1;
		// 鉛直パスに必要な頂点数（下端・上端）。読み戻して診断に使う。
		constexpr Sint32 kPathPointCount = 2;

		// 構造材ツールのフィールド名（Python 版 vw/member.py・vw/column.py の SetRField と
		// 同じ universal 名）。**名前が 1 つ違うだけで setter は黙って無視される**（M6 の
		// 垂木で実証済み。draw/Rafter.cpp 冒頭）ので、寸法は読み戻して確かめる。
		// 記号 PIO も読む 3 つ（kFieldStructuralUse / kFieldMajorBreadth / kFieldMajorDepth）と
		// そのローカライズ名は draw/StructuralMember.h にある。ここは書き手だけが使う残り。
		constexpr const char* kFieldMemberID = "MemberID";			   // 構造材 ID
		constexpr const char* kFieldProfileShape = "ProfileShape";	   // 断面形状
		constexpr const char* kFieldB = "B";						   // 幅（矩形断面）
		constexpr const char* kFieldD = "D";						   // せい（矩形断面）
		constexpr const char* kFieldMemberType = "MemberType";		   // 部材種別
		constexpr const char* kFieldAxisAlign = "AxisAlign";		   // 軸の配置基準
		constexpr const char* kFieldStartCondition = "StartCondition"; // 始端の端部条件
		constexpr const char* kFieldEndCondition = "EndCondition";	   // 終端の端部条件
		constexpr const char* kFieldProfileSeries = "ProfileSeries";   // 断面シリーズ

		// universal 名で引けなかったときに使う OIP のローカライズ名（ResolveParamName）。
		constexpr const char* kLocalizedProfileShape = "断面形状";

		// フィールドに渡す値（Python 版と同じ。ポップアップはキーで保持されるため数値文字列）。
		constexpr const char* kProfileShapeRectangle = "Rectangle";
		// 部材種別は横架材（梁）・柱とも "2"（Python 版 vw/member.py・vw/column.py で同値。
		// 種別の違いは構造用途＝StructuralUse の方に出る）。
		constexpr const char* kMemberTypeStructural = "2";
		constexpr const char* kAxisAlignTopCentre = "1"; // 天端中央（3×3 グリッドの上段中央）
		constexpr const char* kAxisAlignCentre = "4";	 // 中央（同 0 始まり中央）
		constexpr const char* kEndConditionSquare = "3"; // 直切り
		constexpr const char* kProfileSeriesDefault = "AISC (Inch)";

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

		// 断面基準点 → 構造材ツールのポップアップのキー。
		const char* AxisAlignKey(StructuralAxisAlign align)
		{
			return align == StructuralAxisAlign::Centre ? kAxisAlignCentre : kAxisAlignTopCentre;
		}
	} // namespace

	MCObjectHandle CreatePath(const core::Vec3& start, const core::Vec3& end, bool& outAppended)
	{
		outAppended = false;
		MCObjectHandle path =
			gSDK->CreateNurbsCurve(WorldPt3(start.x, start.y, start.z), false, kPathDegree);
		if (path == nil)
			return nil;

		// **Add3DVertex が VS の AddVertex3D にあたる**（ヘッダ参照）。末尾へ 1 点足して
		// 始端 → 終端の 2 点にする。
		gSDK->Add3DVertex(path, WorldPt3(end.x, end.y, end.z));
		// 頂点が本当に 2 つになったかを読み戻す。ピース索引の起点は 0 / 1 のどちらの
		// 規約もあり得るので両方を見る（**判定に失敗しても曲線はそのまま使う**——ここで
		// 諦めると、索引の規約違いというだけで部材が 1 本も描かれなくなる）。
		outAppended = gSDK->NurbsGetNumPts(path, 0) >= kPathPointCount ||
					  gSDK->NurbsGetNumPts(path, 1) >= kPathPointCount;
		return path;
	}

	StructuralMemberResult DrawStructuralMember(const StructuralMemberSpec& spec, RefNumber style)
	{
		StructuralMemberResult result;
		// **空のプロファイルを渡してはならない**（断面が無いのと同じで、PIO は生成できても
		// 実体が描かれず「オブジェクトはあるのに画面に何も出ない」状態になる。
		// draw/DrawUtil の CreateRectangleProfileGroup 参照）。
		if (spec.path == nil || spec.profile == nil)
			return result;

		MCObjectHandle object =
			gSDK->CreateCustomObjectPath(kStructuralMember, spec.path, spec.profile, true);
		if (object == nil)
			return result;

		SetClassByName(object, spec.drawClass);
		SetAllAttributesByClass(object);
		// スタイルは個別フィールドより**先に**関連付ける（後に設定する実測値で
		// スタイル既定のパラメータを上書きするため）。
		if (style != 0)
			gSDK->SetPluginObjectStyle(object, style);

		// 高さ基準を始端（0）・終端（1）それぞれのストーリレベルへバインドする。これで
		// 構造材ツールの高さ基準が「レイヤの高さ」・offset 0 のまま実ジオメトリと矛盾する
		// ことがなくなり、編集時に高さがリセットされない。水平材の傾斜はこの offset 差で
		// 表れ、鉛直材ではこの差が柱高さを支配する。
		gSDK->SetObjectStoryBound(object, kStartBoundID, StoryBoundOf(spec.startBound));
		gSDK->SetObjectStoryBound(object, kEndBoundID, StoryBoundOf(spec.endBound));

		VWParametricObj pio(object);
		const TXString breadth = ResolveParamName(pio, kFieldMajorBreadth, kLocalizedBreadth);
		const TXString depth = ResolveParamName(pio, kFieldMajorDepth, kLocalizedDepth);

		pio.SetParamAsString(ResolveParamName(pio, kFieldProfileShape, kLocalizedProfileShape),
							 kProfileShapeRectangle);
		pio.SetParamAsString(kFieldProfileSeries, kProfileSeriesDefault);
		const bool breadthOk = SetParamRealChecked(pio, breadth, spec.width);
		const bool depthOk = SetParamRealChecked(pio, depth, spec.depth);
		// B / D は矩形断面のときの別名。上と同じ値を入れる（存在しなければ無視される）。
		SetParamRealChecked(pio, ResolveParamName(pio, kFieldB, kLocalizedBreadth), spec.width);
		SetParamRealChecked(pio, ResolveParamName(pio, kFieldD, kLocalizedDepth), spec.depth);
		pio.SetParamAsString(kFieldMemberID, TXString(spec.memberId.c_str()));
		pio.SetParamAsString(kFieldMemberType, kMemberTypeStructural);
		pio.SetParamAsString(kFieldStructuralUse, TXString(spec.structuralUse.c_str()));
		pio.SetParamAsString(kFieldAxisAlign, AxisAlignKey(spec.axisAlign));
		pio.SetParamAsString(kFieldStartCondition, kEndConditionSquare);
		pio.SetParamAsString(kFieldEndCondition, kEndConditionSquare);
		gSDK->ResetObject(object);

		result.object = object;
		result.sectionOk = breadthOk && depthOk;
		return result;
	}
} // namespace HomeskzIfcImport::draw
