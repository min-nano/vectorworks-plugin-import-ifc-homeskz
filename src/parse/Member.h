//
//	parse/Member.h
//
//	Phase 1（IFC 解析）の横架材モジュール（docs/DEV-NOTES.md M7「横架材」）。土台・梁・桁・
//	母屋・棟木・登り梁——ホームズ君 IFC の IfcBeam / IfcMember をすべてここで解析し、
//	core::MemberCommand へ変換する。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）・構造クラス
//	（parse/StructuralClass）だけで完結する（CLAUDE.md「Phase 1」）。
//
//	解析の要点:
//	  * **基準点補正**: ホームズ君 IFC の梁は**断面中心線**（プロファイル中心の押し出し軸）で
//	    配置されるが、VW 構造材ツールの断面基準点は**左右中央・上端＝天端中央**。そこで
//	    断面中心線を「軸に直交し軸を含む鉛直面内で上向き」の方向へ せい/2 持ち上げた
//	    **天端中央線**を命令に入れる（水平梁なら単純に Z ＋ せい/2）。
//	  * **高さは梁ごとの実配置から採る**。レイヤ基準高さに固定しないので、段差梁も正しい
//	    高さに乗る。ローカル配置 Z を取れない梁だけがレイヤ基準高さ（ストーリ高さ ＋
//	    横架材天端オフセット、最上階はストーリ高さ）へフォールバックする（既に天端なので
//	    せい/2 の補正は掛けない）。
//	  * **高さ基準のバインド**: 始端・終端をそれぞれ配置先レイヤのストーリレベル
//	    （一般階＝横架材天端／最上階＝軒高／母屋＝母屋／登り梁＝登り梁）へバインドし、
//	    offset にレベル絶対 Z から天端 Z までの距離を入れる。傾斜は始端・終端の offset 差
//	    として表れ、**パスには Z 成分を持たせない**（draw/Member.cpp 冒頭）。
//	  * **母屋・棟木の分離**: クラスが母屋／棟木の材は、梁（小屋梁・軒桁）と重なって
//	    見にくいため専用レイヤ "n-母屋" に置き、高さ基準も母屋レベルにする。最上階の主屋根
//	    だけでなく、中間階に架かる下屋根（下屋）の母屋・棟木も同じ扱い。
//	  * **登り梁の分離と任意断面の抽出**: 登り梁は矩形断面ではなく、材の側面（長さ×せいの
//	    平行四辺形。端部は直切り＝鉛直面）を厚み方向へ押し出した任意断面
//	    （IfcArbitraryClosedProfileDef）で出力される。矩形前提の memberProfileDims では
//	    拾えず**取りこぼされて全く描画されない**ため、slopedMemberGeometry が平行四辺形の
//	    4 頂点から中心軸・幅・せい・傾斜を導出する。専用レイヤ "n-登り梁" に置く。
//	    誤取り込み防止に、押し出し軸が鉛直な材（火打）は断面種別より**先に**軸で除外し、
//	    プロファイルが 4 頂点でない材（筋かい＝6 頂点）は導出が失敗してスキップされる。
//	  * **登り梁の直切りの幾何**: 端部が鉛直面なので、天端中央線の端点は断面中心軸の
//	    **直上**（XY は同じ）＝鉛直な端面の上端に取り、高さは 断面中心 ＋ せい/(2·cosθ)。
//	    矩形前提の軸直交持ち上げを使うと天端が (せい/2)(secθ − cosθ) 低くなって垂木下面に
//	    届かず、端部も (せい/2)·sinθ だけ軒側へずれて受ける柱との間に隙間ができる。
//	    **この高さは屋根版が見つからないときのフォールバック**で、通常は parse/Noboribari の
//	    屋根スナップが上書きする。
//	  * **取り合いの端点は相手の芯線**: 命令を組み立てた後、甲乙梁の T 字で負ける側の端点を
//	    勝ち側の**天端中央線（＝芯線）上**へ移し、そこから手前の面までの戻りを端部オフセット
//	    に入れる（resolveMemberInterferences）。実際に描かれる材の範囲は「相手の面まで」で
//	    従来と同じで、命令の端点だけが接合点になる——構造材同士の接合状況を座標から拾える
//	    ようにするため（core/Document.h「端部オフセット」）。出隅の L 字は勝ち負けが付かない
//	    ので触らない。
//
//	【M7 のスコープ】登り梁の端部詰め（parse/Noboribari）は受ける材＝本モジュールの
//	横架材だけを見る。柱（M8）を参照する最終化は柱の導入時に行う（docs/DEV-NOTES.md M7/M8）。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 母屋（棟木を含む小屋組の上端材）・登り梁のレベル／レイヤ名。配置先レイヤは
	// "{接頭辞}-母屋" / "{接頭辞}-登り梁"。文字列の定義は core/Document.h（命令セットの語彙）
	// にあり、ここはその再公開（parse/Story.h の kLevelFL ほか、parse/Rafter.h の
	// kLevelTaruki と同じ流儀）。
	inline constexpr const char* kLevelMoya = core::kLevelMoya;
	inline constexpr const char* kLevelNoboribari = core::kLevelNoboribari;

	// 軸の XY 成分がこれ以下の材は鉛直材（横架材でない＝火打等）とみなしスキップする。
	inline constexpr double kVerticalAxisTol = 1e-9;

	// 両端の天端 Z の差がこの値以下なら水平材とみなす（mm）。クラス推定の「軒高超え」判定と、
	// 食い込み調整の対象外判定（傾斜梁）に使う。
	inline constexpr double kSlopeTol = 1.0;

	// 要素が横架材（IfcBeam / IfcMember）か。**母屋・登り梁の判定（parse/Story の
	// レベル追加）と解析本体で同じ述語を使う**ため、ここに一本化する。
	bool isMemberElement(const Entity& element);

	// 断面寸法と材種名から構造材 ID を組み立てる。例: makeMemberId(120, 180, "杉対称異等級集
	// 成材 E105-F355")→ "120×180 - 杉対称異等級集成材E105-F355"材種が空なら "120×180"。
	// 寸法は整数へ丸める（表示用の ID なので端数不要）。
	std::string makeMemberId(double width, double height, const std::string& material);

	// 横架材のローカル配置。
	//   x / y      … 配置点（断面中心）のワールド XY
	//   z / hasZ   … 配置点のローカル Z。Location の座標が 2 つしか無ければ hasZ=false で、
	//                 呼び出し側がレイヤ基準高さへフォールバックする
	//   axis       … 梁軸（＝押し出し方向＝IfcAxis2Placement3D の Axis）の 3D 単位ベクトル。
	//                 登り梁・隅木等の傾斜梁は Z 成分を持つ。Axis 未設定なら (1,0,0)
	struct MemberPlacement
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		bool hasZ = false;
		core::Vec3 axis{1.0, 0.0, 0.0};
	};

	// 要素のローカル配置から MemberPlacement を得る。ObjectPlacement が無い／
	// IfcLocalPlacement でない／RelativePlacement が IfcAxis2Placement3D でない／Location
	// が無いときは false。親 PlacementRelTo は辿らない（parse/IfcGeometry の
	// resolveObjectPlacement と同じ規約。階高は描画側が反映する）。
	bool memberPlacement3D(const Model& model, const Entity& element, MemberPlacement& out);

	// 矩形断面の寸法。width=XDim・height=YDim ・ length=押し出し長。
	struct MemberProfile
	{
		double width = 0.0;
		double height = 0.0;
		double length = 0.0;
	};

	// 要素の Body 表現から矩形断面の寸法を得る。
	// **RepresentationIdentifier が "Body" の表現の、素の IfcExtrudedAreaSolid ＋
	// IfcRectangleProfileDef だけ**を見る（差演算は剥がさない。剥がすと登り梁の任意断面と
	// 見分けが付かなくなる）。見つからなければ false ＝ 呼び出し側が登り梁経路へ回す。
	bool memberProfileDims(const Model& model, const Entity& element, MemberProfile& out);

	// 任意断面（平行四辺形の側面シルエット）を厚み方向へ押し出した傾斜梁＝登り梁の幾何。
	//   origin … 中心軸の一端のワールド座標（XY 絶対・Z はストーリ相対）
	//   axis   … 中心軸の単位ベクトル（origin → もう一端）
	//   width  … 幅（＝押し出し長＝材の厚み）
	//   height … せい（プロファイルの長さ軸でない方の span）
	//   length … 中心軸長
	struct SlopedMemberGeometry
	{
		core::Vec3 origin;
		core::Vec3 axis;
		double width = 0.0;
		double height = 0.0;
		double length = 0.0;
	};

	// 要素から登り梁の幾何を導出する。
	// **矩形断面（通常の横架材が memberProfileDims で処理する）・4 頂点でないプロファイル
	// （火打の footprint や筋かいの 6 頂点）・解釈不能な材は false**。
	bool slopedMemberGeometry(const Model& model, const Entity& element, SlopedMemberGeometry& out);

	// 要素に関連付けられた材種名を返す。IfcRelAssociatesMaterial を逆参照から辿り、
	// IfcMaterial / IfcMaterialList / IfcMaterialLayerSetUsage の順に名前を拾う。
	// 見つからなければ空文字。逆参照は #id 昇順なので、エンティティ列挙順に依存しない決定的な
	// 結果になる。
	std::string memberMaterialName(const Model& model, const Entity& element);

	// 横架材の端部と相手梁（中心線の始点 otherStart・単位軸 otherAxis・長さ otherLength・
	// 半幅 otherHalfWidth）の取り合い 1 件。端点 point と外向き単位ベクトル outward
	// （＝その端から材の外へ向かう向き）から求める。
	//
	//   found    … 取り合いとみなせるか。端点が相手の矩形（幅・長さの範囲）に載っていて、
	//              ほぼ平行でない（平行なら食い込みではなく継ぎ手）こと。
	//   reach    … 端点から相手の**芯線**までの outward 方向の符号付き距離。端点を
	//              −outward へ reach だけ動かすと芯線に乗る（正＝端点が芯線を越えている）。
	//   setback  … 芯線から相手の**手前の面**までの、こちらの軸方向の距離（>= 0）。
	//              直交なら相手の半幅そのもの、斜交ならその 1/cos。端部オフセットは −setback。
	//   interior … 取り合いが相手の**途中**か（T 字）。相手の端部での取り合い（L 字の出隅）は
	//              false。**勝ち負けはこれで決める**——途中で受けている側が「通し材」で勝ち、
	//              端部どうしがぶつかる出隅は勝ち負けが付かない（従来の相互食い込み量の
	//              比較と同じ結論になり、食い込みが無い＝既に面で止まっている取り合いでも
	//              判定できる点だけが違う）。
	struct MemberJoint
	{
		bool found = false;
		double reach = 0.0;
		double setback = 0.0;
		bool interior = false;
	};

	// 端部 point / outward と相手梁の取り合いを求める（MemberJoint 参照）。
	// **登り梁の端部詰め（parse/Noboribari）と横架材同士の取り合い（本モジュール）が共有する
	// 唯一の幾何**なので、ここに 1 つだけ置く。
	MemberJoint memberEndJoint(const core::Vec2& point, const core::Vec2& outward,
							   const core::Vec2& otherStart, const core::Vec2& otherAxis,
							   double otherLength, double otherHalfWidth);

	// 端点 point・外向き単位ベクトル outward が、相手梁の矩形に食い込む量を返す。
	// 端点が矩形の内部にあるとき、端点を −outward 方向へ引き戻して相手の手前の面まで出すのに
	// 必要な距離（>= 0）。食い込んでいない・ほぼ平行（＝食い込みでなく継ぎ手）なら 0。
	// memberEndJoint の reach + setback（負なら 0）そのもので、**登り梁の端部詰め
	// （parse/Noboribari）が使う**。
	double memberPenetrationDepth(const core::Vec2& point, const core::Vec2& outward,
								  const core::Vec2& otherStart, const core::Vec2& otherAxis,
								  double otherLength, double otherHalfWidth);

	// 横架材同士の取り合い（T 字の甲乙梁）で、**負け側の端点を勝ち側の芯線へ合わせる**。
	//
	// ある横架材の端点が別の横架材の矩形に載り、かつ配置レイヤが一致し Z 範囲
	// （[天端 − せい, 天端]）が重なり、その取り合いが相手の**途中**（＝相手が通し材で勝ち）
	// なら、端点を相手の**天端中央線（芯線）上**へ移し、そこから手前の面までの戻りを端部
	// オフセット（startOffset / endOffset）へ入れる。実際に描かれる材の範囲は「相手の面まで」
	// で従来と変わらず、変わるのは**命令が持つ端点が接合点（芯線の交点）になる**こと
	// （core/Document.h「端部オフセット」）。
	//
	// 端部どうしがぶつかる出隅（L 字）は勝ち負けが付かないので触らない。相手も自分の途中へ
	// 取り付いている（相互に負け）ときも触らない。傾斜梁（両端の天端 Z が異なる材）は水平面内
	// の矩形モデルが成り立たないため、動かす側にも相手側にもしない。相手の形状は変えず、
	// 負け側の端点とオフセットだけを与える。
	//
	// 判定は入力時点のジオメトリ（スナップショット）に対して行うので、命令の並び順に依存
	// しない決定的な結果になる。入力は変更せず、調整後の新しいリストを返す。
	std::vector<core::MemberCommand>
	resolveMemberInterferences(const std::vector<core::MemberCommand>& commands);

	// STEP Model から横架材の描画命令を組み立てる。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、各階に含まれる
	// IfcBeam / IfcMember を解析する。配置先レイヤは一般階が "n-横架材天端"、最上階が
	// "R-軒高"、母屋・棟木は "n-母屋"、登り梁は "n-登り梁"。座標は通り芯と同じグリッド中心
	// オフセットで補正する。配置・断面を解決できない材はスキップする（1 本の欠損で全体を
	// 止めない。CLAUDE.md「エラーハンドリング」）。最後に食い込み調整を掛ける。
	//
	// 並びは階（Elevation 昇順）→ 階内は要素の出現順（#id 昇順の rel 由来）で、エンティティ
	// 列挙順に依存しない決定的な結果になる。
	std::vector<core::MemberCommand> buildMemberCommands(const Model& model);

	// 同上。共有コンテキストのストーリ一覧・センタリング中心・階の要素を使う
	// （parse/Context.h）。**Context 自身がこの結果をキャッシュする**（Context::members）
	// ので、ストーリ（母屋・登り梁レベルの有無）・垂木（桁幅の参照）・登り梁の補正が
	// 同じ 1 回の解析結果を共有する。
	std::vector<core::MemberCommand> buildMemberCommands(Context& context);
} // namespace HomeskzIfcImport::parse
