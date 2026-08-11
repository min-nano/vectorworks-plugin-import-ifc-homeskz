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
//	【地中梁は底盤へ「噛み合わせる」（Python 版と実現手段が異なる最大の点）】
//	地中梁（下り梁・台形断面）は命令セットでは底盤の modifiers に載っている。描画は
//	  1. 断面（u=幅・v=鉛直）を閉じた 2D ポリゴンにし、VWExtrudeObj で 0→depth へ押し出す
//	  2. 配置行列（VWTransformMatrix）の u/v/w 軸へ「幅軸 w・ワールド +Z・走る向き」を入れて
//	     SetObjectMatrix でワールドへ置く（断面原点＝origin。z は絶対値＝梁下端）
//	  3. **ISDK::ModifySlab(slab, solid, isClipObject=false, componentFlags)** で底盤へ足す
//	の 3 手順だけで済む。
//
//	Python 版（VectorScript）は同じ台形プリズムを **2 回**作っていた——(1) 底盤を削り取る
//	clip モディファイア（SetCustomObjectProfileGroup）と (2) 削った位置を埋める可視の 3D
//	ソリッド——が、これは **VS に「足す」形で噛み合わせる手段が無かった**ための回避策である
//	（VW 2026 で確認: vs.ModifySlab は「選択が間違っています」で失敗、CreateCustomObjectPath は
//	作成時ダイアログ＋再実行クラッシュ、後付けの SetCustomObjectProfileGroup は未確定で底盤が
//	不可視）。C++ SDK の ISDK には **ModifySlab（"Adds to or clips from a slab"）** があり、
//	isClipObject=false で**足す**モディファイアとして噛み合わせられる（ci-debug の sdk-grep で
//	実在を確認）。したがって本移植は**地中梁 1 本＝ソリッド 1 つ**で、複製も削り取りも持たない。
//
//	噛み合わせに失敗した場合（ModifySlab が false）だけ、作ったソリッドを**独立した可視
//	ソリッド**として底盤と同じクラスで残す（Python 版の (2) に相当。1 本の失敗で地中梁を
//	失わないためのフォールバックで、正常系では通らない）。
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
//	実描画（壁の高さ基準・壁スタイル・底盤の天端とスラブスタイル）はローカルの
//	VectorWorks で目視確認する方針（ROADMAP.md M9「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Footing.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/Math/VWTransformMatrix.h"
#include "VWFC/VWObjects/VWExtrudeObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cmath>
#include <cstddef>
#include <map>
#include <numbers>
#include <string>

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

		// ModifySlab の isClipObject。**false＝足す（add）モディファイア**で、地中梁は
		// 底盤へ足して噛み合わせる（削り取り＝true は使わない。ヘッダ冒頭「地中梁は底盤へ
		// 噛み合わせる」）。名前を付けて渡すのは、bool の裸の false が「clip しない」なのか
		// 「add しない」なのか読めないため。
		constexpr bool kAddModifier = false;

		// ModifySlab の componentFlags（どの構成層をモディファイアが変えるか、のビット）。
		// 地中梁は**コンクリートの下り梁**なので、最上層＝コンクリート（索引 0）のビットだけを
		// 立てる。スラブのコンポーネント索引が 0 始まりであることは実機で確認済み
		// （draw/DrawUtil の SetComponents の注記）。
		//
		// ★ローカル確認の観察点: 断面ビューポートで**コンクリート層**が地中梁の形に下がって
		// いるか。もし下がるのが捨てコン層（＝ビットが 1 始まり）だったら 1u << 1 へ、
		// 層の区別がそもそも無ければ全層（0xFFFFFFFFu）へ変える。**この 1 か所だけ**を直せば
		// よいようにここに置く。
		constexpr Uint32 kGroundBeamComponentFlags = 1u << 0;

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

		// 立上り 1 本を壁として描く。壁を作れなければ壁芯の直線にフォールバックする。
		// 配置できたら true。
		bool DrawOneWall(const core::WallCommand& wall, InternalIndex wallStyle)
		{
			const WorldPt start(wall.start.x, wall.start.y);
			const WorldPt end(wall.end.x, wall.end.y);

			MCObjectHandle object = gSDK->CreateWall(start, end, wall.thickness);
			if (object == nil)
			{
				// フォールバック: 壁芯の直線（クラス付き）を残す（命令の位置は図面に残る）。
				// 2D ポリラインで作るのは他の要素のフォールバックと同じ作法（draw/Grid・
				// draw/Member）。
				VWPolygon2DObj line(
					{VWPoint2D(wall.start.x, wall.start.y), VWPoint2D(wall.end.x, wall.end.y)});
				line.SetClosed(false);
				const MCObjectHandle lineHandle = line.GetThisObject();
				if (lineHandle == nil)
					return false;
				SetClassByName(lineHandle, wall.drawClass);
				SetAllAttributesByClass(lineHandle);
				return true;
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

			gSDK->ResetObject(object);
			return true;
		}

		// 地中梁 1 本を台形プリズム（押し出しソリッド）として作り、そのハンドルを返す。
		// 作れなければ nil。
		//
		// 断面（profile。u=幅・v=鉛直で v=0 が梁下端）を閉じた 2D ポリゴンにして局所 Z へ
		// 0→depth だけ押し出し、配置行列でワールドへ置く。行列の 3 軸は命令の座標規約
		// （core/Document.h の ModifierCommand）と 1 対 1 で対応する:
		//   u 軸（局所 X）… 幅軸 w ＝ 走る向きを +90 度回した水平単位ベクトル
		//   v 軸（局所 Y）… ワールド +Z（鉛直）
		//   w 軸（局所 Z）… 走る向き（押し出し方向。方位角 azimuth の水平単位ベクトル）
		//   offset       … 断面原点のワールド座標（z は**絶対値**＝梁下端の Z）
		// この 3 軸は右手系（u × v = w）で、Python 版が Rotate3D を 2 回かけて作っていた姿勢と
		// 同じものを 1 つの行列で与える（C++ では回転状態を持たず行列を直接組める）。
		MCObjectHandle CreateGroundBeamSolid(const core::ModifierCommand& modifier)
		{
			const MCObjectHandle profile = CreateClosedPolygon(modifier.profile);
			if (profile == nil)
				return nil;

			// 押し出し（局所 Z へ 0→depth）。プロファイルは押し出しの中へ入る。
			VWExtrudeObj extrude(profile, 0.0, modifier.depth);
			const MCObjectHandle object = extrude.GetThisObject();
			if (object == nil)
				return nil;

			const double phi = modifier.azimuth * (std::numbers::pi / 180.0);
			const double dirX = std::cos(phi);
			const double dirY = std::sin(phi);

			VWTransformMatrix matrix;
			matrix.SetMatrix(VWPoint3D(-dirY, dirX, 0.0), VWPoint3D(0.0, 0.0, 1.0),
							 VWPoint3D(dirX, dirY, 0.0),
							 VWPoint3D(modifier.origin.x, modifier.origin.y, modifier.origin.z));
			extrude.SetObjectMatrix(matrix);
			return object;
		}

		// 底盤へ地中梁を噛み合わせる（slabObject が nil＝スラブを作れなかったときは、
		// 地中梁を可視ソリッドとして置くだけにする）。
		//
		// ModifySlab に **isClipObject=false（足す）** で渡すだけで、地中梁は底盤の一部になる
		// （ヘッダ冒頭「地中梁は底盤へ噛み合わせる」）。失敗した 1 本は独立した可視ソリッドと
		// して底盤と同じクラスで残し、**断面ビューポートで構造用図形として扱う**
		// （ovIsStructural）を立てて底盤と一体に見えるようにする（Python 版の可視ソリッドと
		// 同じ扱い。1 本の失敗で地中梁を失わないための保険）。
		void AttachGroundBeams(MCObjectHandle slabObject, const core::SlabCommand& slab)
		{
			for (const core::ModifierCommand& modifier : slab.modifiers)
			{
				const MCObjectHandle solid = CreateGroundBeamSolid(modifier);
				if (solid == nil)
					continue;

				if (slabObject != nil &&
					gSDK->ModifySlab(slabObject, solid, kAddModifier, kGroundBeamComponentFlags))
					continue;

				// フォールバック: 噛み合わせられなかった地中梁は、そのまま可視ソリッドとして残す。
				SetClassByName(solid, slab.drawClass);
				SetAllAttributesByClass(solid);
				gSDK->SetObjectVariable(solid, ovIsStructural, TVariableBlock(true));
			}
		}

		// 底盤 1 枚をスラブとして描く。スラブを作れなければ外形ポリゴンにフォールバックする。
		// 配置できたら true。手順は draw/Floor の DrawOne と同じ（共通部分は draw/DrawUtil）。
		bool DrawOneSlab(const core::SlabCommand& slab, InternalIndex style)
		{
			const MCObjectHandle profile = CreateClosedPolygon(slab.boundary);
			if (profile == nil)
				return false;

			MCObjectHandle object = gSDK->CreateSlab(profile);
			if (object == nil)
			{
				// フォールバック: 外形ポリゴンをクラス付きで残す。スラブが作れなくても
				// **地中梁自体は描く**（AttachGroundBeams が可視ソリッドとして残す）。
				SetClassByName(profile, slab.drawClass);
				SetAllAttributesByClass(profile);
				AttachGroundBeams(nil, slab);
				return true;
			}

			SetClassByName(object, slab.drawClass);
			SetAllAttributesByClass(object);

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

			// 地中梁を噛み合わせる（**高さ・スタイルを与えた後**に行う。スラブの構成層と
			// 基準面が確定してからでないと、モディファイアがどの層をどこまで変えるかが
			// 決まらない）。ResetObject はこの後に 1 回だけ回して形状を作り直させる。
			AttachGroundBeams(object, slab);

			gSDK->ResetObject(object);
			return true;
		}
	} // namespace

	std::size_t drawWalls(const core::Document& document, core::ProgressReporter& progress)
	{
		// 命令のスタイル名 → このインポートで作ったスタイルの索引（底盤と同じ作法）。
		// 同じ壁厚の立上りは 1 つのスタイルを共有する。既存リソースには触れないので、
		// 実際の名前は連番付きになりうる。
		std::map<std::string, InternalIndex> styles;

		std::size_t drawn = 0;
		for (const core::WallCommand& wall : document.walls)
		{
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

			if (DrawOneWall(wall, wallStyle))
				++drawn;
		}
		return drawn;
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
