//
//	draw/Footing.cpp
//
//	基礎描画の実装。命令セットの立上り（WallCommand）を壁オブジェクトへ、底盤
//	（SlabCommand）をスラブオブジェクトへ配置する。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【立上りの描画手順】
//	  1. CreateWall(始点, 終点, 壁厚) で壁芯から壁を作る。**Python 版の
//	     DoubLines(壁厚) → Wall(壁芯) という 2 手順は要らない**——ISDK の CreateWall は
//	     壁厚を引数に取る（ci-debug の sdk-grep で確認）。
//	  2. クラス分けと描画属性の by-class 設定。
//	  3. **壁厚ごとの壁スタイル**を当てる（下記「スタイルは常に新規作成する」）。
//	  4. SetWallOverallHeights で下端・上端をストーリレベルへバインドする。
//	     **壁だけは汎用の SetObjectStoryBound では高さ基準が確定せず**、デザインレイヤの
//	     「壁の高さ（レイヤ設定）」に従ってしまう（構造材・スラブでは SetObjectStoryBound が
//	     効くが、壁は専用関数が要る。Python 版 CLAUDE.md「基礎」節）。命令の
//	     bottomBound / topBound の storyOffset（0=自階・1=上階）がそのまま story 引数になる。
//	     **スタイルより後に置く**のは Python 版 draw_wall と同じ順序で、スタイル適用が壁の
//	     属性を作り直しても高さが残るようにするため。
//	  5. ResetObject で反映。
//	壁を作れない場合は壁芯の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【底盤の描画手順】床板（draw/Floor）と同じ作法で、共通部分は draw/DrawUtil にある
//	（SetComponents / SetSlabDatum）。違うのはスタイル名（"基礎スラブ - コンクリート …"）と
//	構成層（コンクリート／捨てコン／砕石）だけ。SetSlabHeight は厚みではなく**基準面の高さ**
//	（絶対 Z）を設定する関数である点に注意（Python 版 #70 と同じ落とし穴）。基礎ストーリは
//	GL=0 なので絶対 Z がそのまま渡せる。
//
//	【スタイルは常に新規作成する（立上り・底盤とも）】**既存の同名スタイルには一切触れない**
//	（CreateUniqueWallStyle / CreateUniqueSlabStyle）。名前が空いていなければ " (2)" … と
//	連番を付けて作る。当初は「名前で引いて、あれば構成層を組み直す」形にしていたが、
//	ドキュメントのテンプレートに同名のスタイルがあると、そこで設定済みのクラス・マテリアル・
//	用途が既定値へ戻ってしまった（ローカル確認で判明。ROADMAP.md M9）。構成をテンプレートに
//	頼らずコード側から与える方が将来の変更が容易、という方針に基づく。同じ命令スタイル名の
//	立上り／底盤どうしは 1 つのスタイルを共有する（drawWalls / drawSlabs の対応表）。
//
//	立上りのスタイルは**壁厚ごと**（"基礎立上り - コンクリート 150mm"）。Python 版は既製の
//	`基礎 - 木造ベタ基礎150mm` を全ての立上りへ当てるが、実データの壁厚は 120 / 150 / 300mm と
//	混在するので 150mm 固定のスタイルでは厚みが合わない（構成層＝コンクリート 1 層の合計が
//	そのまま壁厚になるので、スタイルを当てても命令の壁厚が保たれる）。
//
//	【端部のキャップ（M10・ローカル確認で判明）】VW の壁は端部を閉じる線（キャップ）を
//	**壁ごとに**持ち、明示しなければドキュメントの壁ツール設定に従う。実機では
//	「T 字でぶつかる側の端線が見える」「自由端に閉じ線が無い」の両方が起きたので、
//	**解析側が端ごとに決めた値**（WallCommand の capStart / capEnd）を SetWallCaps で
//	そのまま設定する。設定は 2 回行う: 壁を作った直後（結合が無くても正しく見えるように）と、
//	**壁結合をすべて実行した後**（JoinWalls が結合した端のキャップを書き換えるため）。
//
//	【壁結合の手順（M10）】立上りを描くときに**命令インデックス → 壁ハンドル**の対応表
//	（WallHandles）を作り、壁結合命令の a / b でその 2 本を引いて JoinWalls へ渡す。SDK
//	ハンドルは Document に載せられないので、この対応表が唯一の受け渡し手段になる
//	（CLAUDE.md「所有権」）。ピック点は解析側が算出済みの「残す側」の点をそのまま渡す
//	（交点そのものを渡すと VW が残す／詰める側を決められない。core/Document.h 参照）。
//	結合失敗のダイアログは抑止する（インポート中に手動操作を求めない）。
//
//	【地中梁の描画（M10）】地中梁は**台形断面**なので単一のスラブでは描けない。底盤に
//	噛み合う台形プリズム（core::ModifierCommand）を **2 回**作って表す:
//	  1. **削り取りモディファイア** … プリズム群を 1 つのグループにまとめ、
//	     SetCustomObjectProfileGroup で通常スラブ（CreateSlab）へ渡すと底盤を**削り取る**。
//	     地中梁の位置で底盤のスラブスタイルの層（コンクリート・捨てコン・砕石）が消え、
//	     断面に写り込まなくなる。
//	  2. **可視の 3D ソリッド** … 同じプリズムを独立したソリッドとして同じレイヤ・同じ
//	     基礎スラブクラスで置き、削り取った位置を地中梁のコンクリートで埋める。
//	     こちらだけ天端を底盤へ 10mm 呑み込ませ（core::raiseModifierTop）、天端と底盤底面が
//	     面ちょうど接する（coplanar）ことによる断面の境界線を防ぐ。
//	Python 版が「足す」形で Slab PIO に噛み合わせられず 2 回作る形に落ち着いた経緯
//	（CreateCustomObjectPath はダイアログ＋クラッシュ、後付けの ProfileGroup は底盤が不可視、
//	ModifySlab は失敗）も同じで、こちらは最初からこの形で作る。
//
//	【Python 版と異なる点・意図的】地中梁の可視ソリッドに**マテリアルを設定しない**。
//	Python 版は文書に登録済みの "基礎コンクリート MT" を名前で引いて割り当てるが、本移植は
//	既存リソースに依存しない（スタイルを名前で作るのと同じ方針。draw/Footing.cpp の
//	「スタイルは常に新規作成する」）。見え方はクラス（基礎スラブ）の by-class 属性で決まる。
//
//	実描画（壁の高さ基準・壁スタイル・底盤の天端とスラブスタイル・壁結合の詰まり方・
//	地中梁の向きと削り取り）はローカルの VectorWorks で目視確認する方針
//	（ROADMAP.md M9 / M10「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Footing.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Geometry.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWExtrudeObj.h"
#include "VWFC/VWObjects/VWGroupObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// SetWallStyle に渡す壁芯からのオフセット（選択側／置換側とも 0＝壁芯に揃える）。
		// **名前は SDK 側の引数名（selectedOffset / replacingOffset）に寄せてある**: 似た並びの
		// InternalIndex + WorldCoord を渡すため、名前が違うと clang-tidy の
		// readability-suspicious-call-argument が「引数が入れ替わっているのでは」と誤検知する
		// （"…StyleOffset" という名前がスタイル引数と紛らわしいと判定された。draw/DrawUtil の
		// SetSlabDatum と同じ理由・同じ対処）。
		constexpr double kSelectedOffset = 0.0;
		constexpr double kReplacingOffset = 0.0;

		// SetObjectStoryBound に渡すバウンド ID。スラブは高さ基準を 1 つだけ持つ
		// （draw/Floor と同じ）。型は SDK の TObjectBoundID（= Sint32）だが、その別名は
		// SDK の名前空間の中にあるため、ここでは実体の Sint32 で持つ（暗黙変換で同じ）。
		constexpr Sint32 kSlabBoundID = 0;

		// 地中梁の可視ソリッドを底盤へ呑み込ませる量（mm。Python 版 _GROUND_BEAM_SLAB_BITE）。
		// 地中梁の天端は底盤の底面とちょうど接する（実データで確認: 天端 = 底盤天端 − 底盤厚）
		// ため、可視ソリッドだけを少し大きくして底盤本体に重ね、断面ビューポートで境界線が
		// 不安定に出るのを防ぐ。**削り取りモディファイアは実形状のまま**（ヘッダ冒頭）。
		constexpr double kGroundBeamSlabBite = 10.0;

		// 地中梁のソリッドに立てるオブジェクト変数（Python 版 _MODIFIER_PLANE_VAR＝1160 /
		// _MARK_STRUCTURAL_VAR＝702。SDK 側の名前付き selector を使う）。
		//   ovPlanarObjIsSrceen（1160）… 「2D スクリーンオブジェクトか」。false を立てて
		//          レイヤ平面のワールド 3D として扱わせる（VW 自身のエクスポートで底盤の
		//          モディファイアに false が立つのに合わせる）。削り取り・可視の両方へ。
		//   ovIsStructural（702）… 「断面ビューポートで構造用図形として扱う」（Mark Object as
		//          Structural）。断面で底盤など他の構造用図形と一体にマージ表示させる。
		//          **可視ソリッドだけ**（底盤を clip するだけのモディファイアには不要）。

		// JoinWalls の showAlerts。結合に失敗してもダイアログを出さない（インポート中に
		// 手動操作を求めない。Python 版 _JOIN_SHOW_ALERTS と同じ）。
		constexpr Boolean kJoinShowAlerts = false;

		// オブジェクト変数へ真偽値を書き込む（呼び出しの定型を 1 か所に。draw/Roof の
		// SetPointVariable / SetRealVariable と同じ流儀）。
		void SetBooleanVariable(MCObjectHandle object, short variable, Boolean value)
		{
			gSDK->SetObjectVariable(object, variable, TVariableBlock(value));
		}

		// 壁の端部キャップ（端を閉じる線）を命令どおりに設定する。
		//
		// **既定値はドキュメントの壁ツール設定に従う**ため、明示的に設定しないと「自由端が
		// 開いたまま」「結合した端が閉じたまま」になる（実機で確認）。命令の capStart /
		// capEnd は解析側が交点から決めた値（core/Document.h「端部を閉じるかは解析側が
		// 決める」）。第 4 引数の round は端部を丸めるかで、基礎の立上りは常に角のまま。
		//
		// ★左右（leftCap / rightCap）が壁芯の始点側／終点側のどちらに当たるかは SDK の
		// ヘッダに明記が無いので、**始点＝left・終点＝right** と解釈している。両端の値が
		// 違う立上り（片方だけ自由端）でだけ差が出るので、ローカル確認の項目にしてある
		// （ROADMAP.md M10。取り違えていれば入れ替えるだけ）。
		void SetWallCaps(MCObjectHandle object, const core::WallCommand& wall)
		{
			gSDK->SetWallCaps(object, static_cast<Boolean>(wall.capStart),
							  static_cast<Boolean>(wall.capEnd), false);
		}

		// 命令の高さ基準（StoryBoundCommand）を SDK の SStoryObjectData へ写す。
		VectorWorks::SStoryObjectData StoryBound(const core::StoryBoundCommand& bound)
		{
			VectorWorks::SStoryObjectData data;
			data.fBound = VectorWorks::eStoryObjectBound_Story;
			data.fBoundStory = static_cast<Sint8>(bound.storyOffset);
			data.fLayerLevelType = TXString(bound.level.c_str());
			data.fOffset = bound.offset;
			return data;
		}

		// 地中梁（台形プリズム）1 本を押し出しソリッドとして作る。作れなければ nil。
		//
		// 断面（profile の u, v）を**ワールド 3D の底面ポリゴン**へ写し、押し出し方向
		// （方位角）へ depth だけ押し出す。u 軸は「走る向きを +90 度回した水平単位ベクトル」で、
		// これは解析側（parse/Footing の groundBeamModifier）が断面を取り直すときの規約と対。
		// v 軸はワールド Z、断面原点（u=v=0）は命令の origin（XY センタリング済み・Z 絶対値）。
		//
		// **押し出しは基面ポリゴンの法線方向へ伸びる**ので、頂点の並びを「法線が軸方向を
		// 向く」向きに揃えてから渡す（法線は Newell 法。逆巻きだと梁が軸の反対側へ伸びる）。
		MCObjectHandle CreateModifierPrism(const core::ModifierCommand& modifier)
		{
			if (modifier.profile.size() < 3 || modifier.depth <= 0.0)
				return nil;

			const double phi = modifier.azimuth * std::numbers::pi / 180.0;
			const core::Vec2 axis{std::cos(phi), std::sin(phi)};
			const core::Vec2 width{-axis.y, axis.x}; // 幅軸 u（解析側の w と同じ取り方）

			std::vector<core::Vec3> vertices;
			vertices.reserve(modifier.profile.size());
			for (const core::Vec2& p : modifier.profile)
			{
				vertices.push_back(core::Vec3{modifier.origin.x + (width.x * p.x),
											  modifier.origin.y + (width.y * p.x),
											  modifier.origin.z + p.y});
			}

			// 面法線（Newell 法）。軸と逆を向いていたら頂点の並びを反転して、押し出しが
			// 梁の走る向きへ伸びるようにする。
			core::Vec3 normal{0.0, 0.0, 0.0};
			const std::size_t count = vertices.size();
			for (std::size_t i = 0; i < count; ++i)
			{
				const core::Vec3& a = vertices[i];
				const core::Vec3& b = vertices[(i + 1) % count];
				normal.x += (a.y - b.y) * (a.z + b.z);
				normal.y += (a.z - b.z) * (a.x + b.x);
				normal.z += (a.x - b.x) * (a.y + b.y);
			}
			if ((normal.x * axis.x) + (normal.y * axis.y) < 0.0)
				std::ranges::reverse(vertices);

			VWPolygon3D base;
			for (const core::Vec3& v : vertices)
				base.AddVertex(v.x, v.y, v.z);
			base.SetClosed(true);

			const VWExtrudeObj prism(base, modifier.depth);
			const MCObjectHandle handle = prism.GetThisObject();
			if (handle == nil)
				return nil;
			SetBooleanVariable(handle, ovPlanarObjIsSrceen, false);
			return handle;
		}

		// 削り取りモディファイア群を 1 つのグループにまとめて返す（SetCustomObjectProfileGroup
		// へ渡すと通常スラブを**削り取る**）。1 本も作れなければ nil（＝削り取りをしない）。
		// **形状は実形状のまま**にする（呑み込みは可視ソリッドだけ。ヘッダ冒頭）。
		MCObjectHandle CreateModifierGroup(const std::vector<core::ModifierCommand>& modifiers)
		{
			VWGroupObj group;
			for (const core::ModifierCommand& modifier : modifiers)
			{
				const MCObjectHandle prism = CreateModifierPrism(modifier);
				if (prism != nil)
					group.AddObject(prism);
			}
			const MCObjectHandle groupHandle = group.GetThisObject();
			if (groupHandle == nil)
				return nil;
			// 空のグループは渡さない（削り取るものが無いのと同じ。draw/DrawUtil の
			// CreateRectangleProfileGroup と同じ確かめ方）。
			if (VWGroupObj(groupHandle).GetFirstMemberObject() == nil)
				return nil;
			return groupHandle;
		}

		// 地中梁を**可視の 3D ソリッド**として置く（削り取りとは別の 2 つ目の実体）。底盤と
		// 同じクラスを付けて一体に見せ、天端を底盤へ呑み込ませる。断面ビューポートで
		// 構造用図形として扱わせる（オブジェクト変数 702）。
		void DrawBeamSolids(const std::vector<core::ModifierCommand>& modifiers,
							const std::string& className)
		{
			for (const core::ModifierCommand& modifier : modifiers)
			{
				const MCObjectHandle solid =
					CreateModifierPrism(core::raiseModifierTop(modifier, kGroundBeamSlabBite));
				if (solid == nil)
					continue;
				SetClassByName(solid, className);
				SetAllAttributesByClass(solid);
				SetBooleanVariable(solid, ovIsStructural, true);
			}
		}

		// 立上り 1 本を壁として描く。壁を作れなければ壁芯の直線にフォールバックする。
		// 配置できた壁のハンドル（フォールバックしたときは nil）を返す。
		MCObjectHandle DrawOneWall(const core::WallCommand& wall, InternalIndex wallStyle,
								   bool& outPlaced)
		{
			const WorldPt start(wall.start.x, wall.start.y);
			const WorldPt end(wall.end.x, wall.end.y);

			outPlaced = false;
			MCObjectHandle object = gSDK->CreateWall(start, end, wall.thickness);
			if (object == nil)
			{
				// フォールバック: 壁芯の直線（クラス付き）を残す（命令の位置は図面に残る）。
				// 2D ポリラインで作るのは他の要素のフォールバックと同じ作法（draw/Grid・
				// draw/Member）。**壁ハンドルは記録しない**ので、この立上りに繋がる壁結合は
				// 引き当てられずスキップされる（drawWallJoins）。
				VWPolygon2DObj line(
					{VWPoint2D(wall.start.x, wall.start.y), VWPoint2D(wall.end.x, wall.end.y)});
				line.SetClosed(false);
				const MCObjectHandle lineHandle = line.GetThisObject();
				if (lineHandle == nil)
					return nil;
				SetClassByName(lineHandle, wall.drawClass);
				SetAllAttributesByClass(lineHandle);
				outPlaced = true;
				return nil;
			}

			SetClassByName(object, wall.drawClass);
			SetAllAttributesByClass(object);

			// スタイルは高さバインドより**先に**当てる（Python 版 draw_wall と同じ順序）。
			// スタイル適用が壁の属性を作り直す場合でも、後から入れる高さが残るようにする。
			// スタイルは壁厚ごとに作ってあるので、当てても壁厚は命令どおりのままになる。
			if (wallStyle != 0)
				gSDK->SetWallStyle(object, wallStyle, kSelectedOffset, kReplacingOffset);

			// 高さは壁専用の SetWallOverallHeights でストーリレベルへバインドする
			// （ヘッダ冒頭「立上りの描画手順」3）。
			gSDK->SetWallOverallHeights(object, StoryBound(wall.bottomBound),
										StoryBound(wall.topBound));

			// 端部を閉じるかは命令どおりに設定する（ヘッダ冒頭「端部のキャップ」）。
			SetWallCaps(object, wall);

			gSDK->ResetObject(object);
			outPlaced = true;
			return object;
		}

		// 底盤 1 枚をスラブとして描く。スラブを作れなければ外形ポリゴンにフォールバックする。
		// 配置できたら true。手順は draw/Floor の DrawOne と同じ（共通部分は draw/DrawUtil）。
		// 地中梁を持つ底盤は、削り取りモディファイア＋可視ソリッドの 2 つを併せて置く。
		bool DrawOneSlab(const core::SlabCommand& slab, InternalIndex style)
		{
			const MCObjectHandle profile = CreateClosedPolygon(slab.boundary);
			if (profile == nil)
				return false;

			MCObjectHandle object = gSDK->CreateSlab(profile);
			if (object == nil)
			{
				// フォールバック: 外形ポリゴンをクラス付きで残す。**スラブを作れなくても
				// 地中梁自体は描く**（削り取る相手が無いだけで、可視ソリッドは意味を持つ）。
				SetClassByName(profile, slab.drawClass);
				SetAllAttributesByClass(profile);
				DrawBeamSolids(slab.modifiers, slab.drawClass);
				return true;
			}

			SetClassByName(object, slab.drawClass);
			SetAllAttributesByClass(object);

			// 地中梁（台形プリズム）を持つ底盤は、プリズム群を**削り取りモディファイア**として
			// 渡して底盤を clip する（スラブスタイルの層が地中梁の位置から消える）。可視の
			// ソリッドは下で別に置く（ヘッダ冒頭「地中梁の描画」）。
			if (!slab.modifiers.empty())
			{
				const MCObjectHandle group = CreateModifierGroup(slab.modifiers);
				if (group != nil)
					gSDK->SetCustomObjectProfileGroup(object, group);
			}

			// 構成はコンクリート厚ごとのスラブスタイルで与える（呼び出し側が用意して渡す）。
			// 用意できない場合だけ、スタイルを外してスラブ本体のコンポーネントを直接組む
			// （構成の欠落で底盤を失わないための保険。draw/Floor と同じ）。
			if (style != 0)
			{
				gSDK->SetSlabStyle(object, style);
			}
			else
			{
				gSDK->ConvertToUnstyledSlab(object);
				SetComponents(object, slab.components);
				SetSlabDatum(object, slab.datum, static_cast<short>(slab.components.size()));
			}

			// SetSlabHeight は厚みではなく**基準面の高さ**（絶対 Z）を設定する。命令の
			// elevation はコンクリート天端の絶対 Z なのでそのまま渡す。
			gSDK->SetSlabHeight(object, slab.elevation);

			// 天端を底盤天端レベルへバインドする（offset はそのレベルからの差。主たる底盤は 0）。
			gSDK->SetObjectStoryBound(object, kSlabBoundID, StoryBound(slab.bound));

			gSDK->ResetObject(object);

			// 削り取った位置を埋める可視の 3D ソリッド（2 つ目の実体）。**スラブの確定後**に
			// 置く（ResetObject がスラブを作り直す前に置くと、同じレイヤの図形として巻き込まれ
			// かねない）。
			DrawBeamSolids(slab.modifiers, slab.drawClass);
			return true;
		}

		// レイヤ上の壁オブジェクトの本数を数える（診断用）。デザインレイヤは group-like なので
		// VWGroupObj として辿れる（VWLayerObj は VWGroupObj の派生）。
		//
		// 【なぜ数えるか】ローカル確認で「解析側が出していない立上り（既存の壁の一部と同じ
		// 区間）が図面に増えている」ことが分かった。壁結合の前後で本数を突き合わせれば、
		// **どの結合種別が壁を増やしているか**が実機の 1 周で切り分けられる（ROADMAP.md M10）。
		std::size_t CountWallsOnLayer(MCObjectHandle layer)
		{
			if (layer == nil)
				return 0;
			std::size_t count = 0;
			for (MCObjectHandle object : VWGroupObj(layer))
			{
				if (object != nil && gSDK->GetObjectType(object) == kWallNode)
					++count;
			}
			return count;
		}

		// 壁結合の joinModifier（SDK の JoinModifierType）へ写す。命令の enum は SDK の値
		// （T=1 / L=2 / X=3）に合わせてあるので、そのまま数値で渡せる（core/Document.h）。
		JoinModifierType JoinModifier(core::WallJoinType type)
		{
			switch (type)
			{
			case core::WallJoinType::T:
				return kTWallJoin;
			case core::WallJoinType::L:
				return kLWallJoin;
			case core::WallJoinType::X:
				return kXWallJoin;
			}
			return kLWallJoin;
		}

		// 結合種別の表示名（診断の内訳に出す）。
		std::string JoinTypeLabel(core::WallJoinType type)
		{
			switch (type)
			{
			case core::WallJoinType::T:
				return "T";
			case core::WallJoinType::L:
				return "L";
			case core::WallJoinType::X:
				return "X";
			}
			return "?";
		}
	} // namespace

	// 壁ハンドル表の実体。命令インデックス → 壁ハンドル（draw/Footing.h の WallHandles）。
	struct WallHandleTable
	{
		std::map<std::size_t, MCObjectHandle> handles;
	};

	WallHandles::WallHandles() : fTable(std::make_unique<WallHandleTable>()) {}

	WallHandles::~WallHandles() = default;

	std::size_t drawWalls(const core::Document& document, core::ProgressReporter& progress,
						  WallHandles* handles)
	{
		// 命令のスタイル名 → このインポートで作ったスタイルの索引（底盤と同じ作法）。
		// 同じ壁厚の立上りは 1 つのスタイルを共有する。既存リソースには触れないので、
		// 実際の名前は連番付きになりうる。
		std::map<std::string, InternalIndex> styles;

		std::size_t drawn = 0;
		for (std::size_t index = 0; index < document.walls.size(); ++index)
		{
			const core::WallCommand& wall = document.walls[index];

			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は本数で報告し、
			// 描画の前に 1 件進める（＝「いま何本目を描いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤ（"F-立上り"）が無い命令はスキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(wall.layer) == nil)
				continue;

			const auto found = styles.find(wall.styleName);
			const InternalIndex wallStyle =
				(found != styles.end())
					? found->second
					: styles
						  .emplace(wall.styleName,
								   CreateUniqueWallStyle(wall.styleName, wall.components))
						  .first->second;

			bool placed = false;
			const MCObjectHandle object = DrawOneWall(wall, wallStyle, placed);
			if (placed)
				++drawn;
			// 壁結合が引けるよう、**壁を作れた命令だけ**を対応表へ記録する（フォールバック
			// 描画＝直線で残した命令は結合できないので入れない）。
			if (handles != nullptr && object != nil)
				handles->table().handles.emplace(index, object);
		}
		return drawn;
	}

	std::size_t drawWallJoins(const core::Document& document, core::ProgressReporter& progress,
							  const WallHandles& handles, std::string* outNote)
	{
		const std::map<std::size_t, MCObjectHandle>& table = handles.table().handles;

		// 診断: 結合の前後で壁の本数がどう変わるかを結合種別ごとに数える（CountWallsOnLayer の
		// doc コメント参照）。立上りは 1 枚のレイヤに載るので、その 1 枚を数えれば足りる。
		const MCObjectHandle layer =
			document.walls.empty()
				? nil
				: gSDK->GetNamedLayer(TXString(document.walls.front().layer.c_str()));
		std::size_t wallCount = CountWallsOnLayer(layer);
		std::map<core::WallJoinType, std::size_t> added;

		std::size_t joined = 0;
		std::size_t refused = 0; // JoinWalls が false を返した件数（診断用）
		for (const core::WallJoinCommand& join : document.wallJoins)
		{
			if (progress.cancelled())
				break;
			progress.step();

			const auto first = table.find(join.a);
			const auto second = table.find(join.b);
			// どちらかが未配置（レイヤ未生成・フォールバック描画）の命令はスキップする。
			if (first == table.end() || second == table.end())
				continue;

			// ピック点は解析側が算出済みの「残す側」の点をそのまま渡す（ヘッダ冒頭
			// 「壁結合の手順」）。結合できたかは戻り値で見る（失敗しても他は続ける）。
			if (gSDK->JoinWalls(first->second, second->second, WorldPt(join.pickA.x, join.pickA.y),
								WorldPt(join.pickB.x, join.pickB.y), JoinModifier(join.joinType),
								static_cast<Boolean>(join.capped), kJoinShowAlerts))
				++joined;
			else
				++refused;

			const std::size_t after = CountWallsOnLayer(layer);
			if (after > wallCount)
				added[join.joinType] += after - wallCount;
			wallCount = after;
		}

		// **結合の後に端部のキャップを入れ直す**。JoinWalls は結合した端のキャップを自分で
		// 書き換えるので、最後に命令どおりへ揃え直さないと「取り合う端が閉じたまま」
		// （T 字でぶつかる側の端線が見える）や「自由端が開いたまま」（閉じ線が無い）が残る
		// ——実機で両方が起きた（ROADMAP.md M10）。結合を拒否された命令があっても、
		// 配置できた立上りは必ず命令どおりの端部になる。
		for (const auto& entry : table)
		{
			if (entry.first < document.walls.size())
				SetWallCaps(entry.second, document.walls[entry.first]);
		}

		// 診断（draw/Member と同じ流儀）。「命令はあるのに繋がらない」「壁が増えた」が起きた
		// とき、解析側の判定（種別・ピック点）と描画側（JoinWalls の挙動）のどちらを疑うべきかを
		// 完了ダイアログから切り分けられるようにする。
		if (outNote != nullptr)
		{
			std::string note;
			if (refused > 0)
				note = "壁結合: " + std::to_string(refused) + " 件を VW が拒否しました。";
			if (!added.empty())
			{
				std::size_t total = 0;
				std::string breakdown;
				for (const auto& entry : added)
				{
					total += entry.second;
					if (!breakdown.empty())
						breakdown += " ";
					breakdown += JoinTypeLabel(entry.first) + ":" + std::to_string(entry.second);
				}
				if (!note.empty())
					note += "\n";
				note += "壁結合: 結合後に立上りが " + std::to_string(total) + " 本増えました（" +
						breakdown + "）。";
			}
			*outNote = note;
		}
		return joined;
	}

	std::size_t drawSlabs(const core::Document& document, core::ProgressReporter& progress)
	{
		// 命令のスタイル名 → このインポートで作ったスタイルの索引。同じコンクリート厚の
		// 底盤は 1 つのスタイルを共有する（毎回作ると厚みの数だけでなく枚数ぶんスタイルが
		// 増えてしまう）。既存リソースには触れないので、実際の名前は連番付きになりうる。
		std::map<std::string, InternalIndex> styles;

		std::size_t drawn = 0;
		for (const core::SlabCommand& slab : document.slabs)
		{
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤ（"F-底盤"）が無い命令はスキップする。
			if (ActivateExistingLayer(slab.layer) == nil)
				continue;

			const auto found = styles.find(slab.styleName);
			const InternalIndex style =
				(found != styles.end())
					? found->second
					: styles
						  .emplace(slab.styleName, CreateUniqueSlabStyle(
													   slab.styleName, slab.components, slab.datum))
						  .first->second;

			if (DrawOneSlab(slab, style))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
