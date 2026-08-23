//
//	draw/Floor.cpp
//
//	床板描画の実装。命令セット（FloorCommand）を**スラブオブジェクト**として配置する。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【Python 版（床ツール）との差異・意図的】Python 版 vw/floor.py は床ツール
//	（Floor オブジェクト）で描くが、本移植では**スラブツール（Slab オブジェクト）**を使う。
//	床ツールは実体としては押し出しの派生でオブジェクト構造がほぼ押し出しと変わらないのに対し、
//	スラブは BIM オブジェクトとして機能（コンポーネント構成・スタイル・データ連携）が
//	強化されており、今後の発展性が高い。ISDK にも Floor の生成 API は無く Slab には
//	一式（CreateSlab / SetSlabHeight / スタイル・コンポーネント）が揃っているため、
//	SDK の作法にも素直に乗る（CLAUDE.md「移植の基本方針」: 仕様の意図を再現し、実現手段は
//	C++ SDK の作法に合わせる）。2D 表現は床ツールと異なる点に注意（ローカル確認項目）。
//
//	描画手順（Python 版 vw/footing.py の draw_slab と同じ作法。底盤＝M9 と共通）:
//	  1. 外形を閉じた 2D ポリゴンにする
//	  2. CreateSlab(プロファイル) でスラブを生成する
//	  3. クラス分けと描画属性の by-class 設定
//	  4. **スラブ本体へ直接**構成層を組む（上から 床仕上げ → 床下地）。スラブスタイルは
//	     作らない・当てない（draw/DrawUtil.h「複合オブジェクトの構成」）
//	  5. 高さの基準面（データム）を命令に合わせる（一般階＝床仕上げ上端／ロフト＝床下地
//	     下端）。SetSlabHeight にはその基準面の絶対 Z を渡す（SetSlabHeight は厚みではなく
//	     高さを設定する関数。Python 版 #70 の不具合と同じ落とし穴）
//	  6. SetObjectStoryBound で基準面の高さ基準をストーリレベルへバインドする
//	     （一般階＝FL レベル／ロフト＝軒高レベル。offset はそこからの高低差で一般部は 0）
//	  7. ResetObject で反映
//	スラブが作れない場合は外形ポリゴンにフォールバックする（1 枚の失敗で全体を止めない）。
//	**この手順そのもの（構成層・基準面の設定）は draw/DrawUtil に置いてあり、基礎の底盤
//	（draw/Footing。M9）と共有する**——違うのは層の中身だけ。
//
//	実描画（天端の与え方・厚み・バインドのアンカー・2D 表現）はローカルの VectorWorks で
//	目視確認する方針（ROADMAP.md M5「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Floor.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// SetObjectStoryBound に渡すバウンド ID。スラブは高さ基準を 1 つだけ持ち、
		// Python 版 vw/footing.py の draw_slab も 0 を渡している。型は SDK の
		// TObjectBoundID（= Sint32）だが、その別名は SDK の名前空間の中にあるため、
		// ここでは実体の Sint32 で持つ（暗黙変換で同じ）。
		constexpr Sint32 kSlabBoundID = 0;

		// 床 1 枚をスラブとして描く。スラブを作れなければ外形ポリゴンにフォールバックする。
		// 配置できたら true。
		bool DrawOne(const core::FloorCommand& floor)
		{
			const MCObjectHandle profile = CreateClosedPolygon(floor.boundary);
			if (profile == nil)
				return false;

			MCObjectHandle slab = gSDK->CreateSlab(profile);
			if (slab == nil)
			{
				// フォールバック: 外形ポリゴンをクラス付きで残す。
				SetClassByName(profile, floor.drawClass);
				SetAllAttributesByClass(profile);
				return true;
			}

			SetClassByName(slab, floor.drawClass);
			SetAllAttributesByClass(slab);

			// 構成（床仕上げ／床下地）と基準面は**このスラブへ直接**与える。スラブスタイルは
			// 作らない・当てない（draw/DrawUtil.h「複合オブジェクトの構成」）。CreateSlab は
			// 文書の既定スタイルを引き継ぐことがあるので、明示的にスタイル無しへ落としてから
			// 構成を組む。
			gSDK->ConvertToUnstyledSlab(slab);
			SetComponents(slab, floor.components);
			SetSlabDatum(slab, floor.datum, static_cast<short>(floor.components.size()));

			// SetSlabHeight は厚みではなく**基準面の高さ**（絶対 Z）を設定する（Python 版 #70 と
			// 同じ落とし穴）。命令の elevation は基準面の絶対 Z なのでそのまま渡す。
			gSDK->SetSlabHeight(slab, floor.elevation);

			// 基準面（一般階＝床仕上げ上端／ロフト＝床下地下端）を、命令が指すストーリレベル
			// （一般階＝"FL"／ロフト＝"軒高"）へバインドする。offset はそのレベルからの高低差
			// （一般部 0・床レベル指定時は ±差分）。これをしないと編集時に高さがレイヤ基準へ
			// リセットされて実形状と矛盾する。
			VectorWorks::SStoryObjectData bound;
			bound.fBound = VectorWorks::eStoryObjectBound_Story;
			bound.fBoundStory = static_cast<Sint8>(floor.bound.storyOffset);
			bound.fLayerLevelType = TXString(floor.bound.level.c_str());
			bound.fOffset = floor.bound.offset;
			gSDK->SetObjectStoryBound(slab, kSlabBoundID, bound);

			gSDK->ResetObject(slab);
			return true;
		}
	} // namespace

	std::size_t drawFloors(const core::Document& document, core::ProgressReporter& progress)
	{
		std::size_t drawn = 0;
		for (const core::FloorCommand& floor : document.floors)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は枚数で報告し、
			// 描画の前に 1 件進める（＝「いま何枚目を描いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤ（"n-FL"）が無い命令はスキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(floor.layer) == nil)
				continue;

			if (DrawOne(floor))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
