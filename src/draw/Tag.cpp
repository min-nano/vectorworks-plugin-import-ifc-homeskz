//
//	draw/Tag.cpp
//
//	断面寸法データタグ描画の実装。意図・規約は draw/Tag.h と parse/Tag.h を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API:
//	  * gSDK->CreateCustomObject("Data Tag", 挿入点, 角度, bInsert) … データタグ PIO の生成
//	  * gSDK->AddViewportAnnotationObject(viewport, object)         … ビューポート注釈へ移す
//	  * gSDK->ResetObject / DeleteObject                            … 反映・後始末
//	  * VectorWorks::Extension::IDataTagSupport（VCOM）
//	      SetDataTagStyle    … データタグスタイル（"断面寸法"）の関連付け
//	      AssociateWithObject … 対象の横架材へ関連付け（Python 版 DT_AssociateWithObj）
//	      UpdateDataTag       … 関連付け後の再計算（Python 版 DT_UpdateTaggedTags）
//
//	【注釈に入らなかったタグは消す】AddViewportAnnotationObject に失敗すると、タグは
//	**生成したときのカレントレイヤ（シートレイヤ）に residue として残る**——図面の上に
//	寸法だけが浮くので、失敗したら必ず削除する。
//

#include "PluginPrefix.h"
#include "draw/Tag.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "Interfaces/VectorWorks/Extension/IDataTagSupport.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// データタグの内部プラグイン名（Python 版 vw/sheet.py _DATA_TAG_PLUGIN）。VW 標準の
		// データタグツールの universal 名で、表示名（"データタグ"）とは別物。
		constexpr const char* kDataTagPlugin = "Data Tag";

		// 「引出線を表示」パラメータ（既定 ON）。部材の面ちょうどに置いても ON のままだと
		// 引出線が描かれるので OFF にする（Python 版 _LEADER_FIELD / _LEADER_OFF）。
		// universal 名で見つからなければ OIP の日本語名で引き直す（draw/DrawUtil の
		// ResolveParamName。名前が 1 つ違うだけで setter は黙って無視される）。
		constexpr const char* kFieldUseLeader = "Use Leader";
		constexpr const char* kLocalizedUseLeader = "引出線を表示";

		// タグの逃がし量（offset の向きに沿ったタグの差し渡しの半分）。命令の position は
		// **部材の辺の中央**で、そこへタグの下端中央が接するようにしたい。タグの実寸は
		// スタイルが決めるので、置いてから GetObjectBounds で測る。offset は軸に平行
		// （伏図＝上または左、軸組図＝上）なので、|x| 成分には幅・|y| 成分には高さを当てれば
		// よい（斜材で斜めになる場合も、外接矩形の差し渡しとして妥当な近似になる）。
		double Clearance(const core::TagCommand& tag, double width, double height)
		{
			return (std::abs(tag.offset.x) * width + std::abs(tag.offset.y) * height) / 2.0;
		}

		// 注釈へ置いたタグ 1 つの実測。移動は全部置いてから行う（診断へ出す実測を先頭から
		// 数件そろえるため）。
		struct PendingTag
		{
			MCObjectHandle object = nil;
			const core::TagCommand* command = nullptr;
			double centreX = 0.0; // 置いた直後の実位置
			double centreY = 0.0;
			double clearX = 0.0; // 部材から逃がすベクトル（実寸から求めた）
			double clearY = 0.0;
		};

		// 置いたタグをまとめて目標へ動かす。
		//
		// **目標の絶対位置へバウンディングボックスの中心を合わせる**だけ。命令の position は
		// すでにそのビューポートの注釈空間で表されている（伏図＝モデルの平面座標そのもの、
		// 軸組図＝切断線の終点からの距離と天端 Z。parse/Tag.h）。
		//
		// **この後処理が最終位置を決める。** VW は指定した挿入点にタグを留めない（伏図は
		// タグ幅の半分だけ −X へ寄り、軸組図はビューポートごとにばらばらの場所へ落ちる。
		// ローカル確認で実測。draw/Tag.h の落とし穴 2）ので、どこへ置かれたかに依らず
		// 実位置との差だけ動かす。
		void MovePendingTags(const std::vector<PendingTag>& pending)
		{
			for (const PendingTag& tag : pending)
			{
				const double targetX = tag.command->position.x + tag.clearX;
				const double targetY = tag.command->position.y + tag.clearY;
				gSDK->MoveObject(tag.object, targetX - tag.centreX, targetY - tag.centreY);
			}
		}

		// タグ 1 つを注釈として置く。置けたら true。support は呼び出し側が 1 回だけ作った
		// VCOM のデータタグ支援インターフェース（タグごとに QueryInterface しない）。
		// 置けたタグのハンドルは outPlaced へ積む（クラスを表示へ戻すのに使う）。
		bool PlaceOne(MCObjectHandle viewport, const core::TagCommand& tag, RefNumber style,
					  MCObjectHandle member,
					  const VectorWorks::Extension::IDataTagSupportPtr& support, TagCounts& counts,
					  std::vector<MCObjectHandle>& outPlaced, std::vector<PendingTag>& outPending)
		{
			// 第 4 引数 bInsert=true でカレントレイヤへ入る。この後 AddViewportAnnotationObject で
			// 注釈へ移すので、レイヤ上に残るのは失敗したときだけ（下記で消す）。
			const MCObjectHandle object = gSDK->CreateCustomObject(
				TXString(kDataTagPlugin), WorldPt(tag.position.x, tag.position.y), tag.angle, true);
			if (object == nil)
			{
				++counts.failed;
				return false;
			}

			// **関連付けを先に行う**（スタイルより前）。関連付け先の無いタグにスタイルを
			// 当てると、VW が「互換性のないデータタグスタイルを選択しています」の警告
			// ダイアログを出してインポートが止まる（ローカル確認で判明。タグの数だけ出る）。
			// フォールバックの直線になった横架材はハンドルが無いので関連付けを省く
			// （Python 版と同じ。タグは置く）。
			if (member != nil && support)
				support->AssociateWithObject(object, member);
			else
				++counts.unassociated;

			// スタイル（"断面寸法"）。文書に無ければ**スタイル無しで置く**——タグを失うより、
			// 位置だけでも正しいタグを残した方が原因を追いやすい（構造材のプラグイン
			// スタイルと同じ方針。draw/DrawUtil の ResolvePluginStyle）。
			//
			// **skipValidation=true** を渡して検証を止める。関連付けを先に済ませてあれば
			// 本来は通るはずだが、この検証は**ダイアログでユーザーに聞く**造りなので、
			// 1 件でも引っかかるとインポートが止まってしまう（無人で走らせられない）。
			// 互換性が無ければタグの本文が空になるだけで図面は壊れないので、ここは黙って
			// 進めて結果を目で見てもらう方がよい。
			if (style != 0 && support)
				support->SetDataTagStyle(object, style, /*skipValidation=*/true);

			// 引出線を OFF にする。
			try
			{
				VWParametricObj pio(object);
				const TXString param = ResolveParamName(pio, kFieldUseLeader, kLocalizedUseLeader);
				pio.SetParamBool(param, false);
			}
			catch (...)
			{
				// 引出線が残るだけでタグ自体は使えるので、失敗しても続ける（件数だけ
				// 数えて診断へ回す）。
				++counts.leaderLeft;
			}

			gSDK->ResetObject(object);

			// ビューポートの注釈へ移す。入らなければタグを消す（冒頭「注釈に入らなかった
			// タグは消す」）。
			if (!gSDK->AddViewportAnnotationObject(viewport, object))
			{
				gSDK->DeleteObject(object, true);
				++counts.failed;
				return false;
			}

			// 関連付け後の再計算（Python 版 DT_UpdateTaggedTags）。これをしないと、関連付けた
			// 横架材の断面寸法が本文へ流し込まれない。
			if (support)
				support->UpdateDataTag(object);

			// **ここで実位置と実寸を測る**。ここまででスタイルが本文を流し込み、タグの実寸が
			// 確定している。動かすのは全部置いてから（診断へ出す実測を先頭から数件そろえる
			// ため。MovePendingTags）。
			WorldRect bounds;
			if (!gSDK->GetObjectBounds(object, bounds))
			{
				// 測れないものは動かしようがないので、そのまま残す（生成した位置のまま）。
				++counts.unmeasured;
				outPlaced.push_back(object);
				++counts.drawn;
				return true;
			}

			PendingTag pending;
			pending.object = object;
			pending.command = &tag;
			pending.centreX = (bounds.left + bounds.right) / 2.0;
			// WorldRect は top > bottom（Y 上向き）。
			pending.centreY = (bounds.top + bounds.bottom) / 2.0;
			const double clearance = Clearance(tag, std::abs(bounds.right - bounds.left),
											   std::abs(bounds.top - bounds.bottom));
			pending.clearX = tag.offset.x * clearance;
			pending.clearY = tag.offset.y * clearance;
			outPending.push_back(pending);

			outPlaced.push_back(object);
			++counts.drawn;
			return true;
		}
	} // namespace

	void prepareDataTagPlugin()
	{
		gSDK->DefineCustomObject(TXString(kDataTagPlugin), kCustomObjectPrefNever);
	}

	std::size_t drawViewportTags(MCObjectHandle viewport, const core::ViewportCommand& command,
								 const ObjectHandleTable& memberHandles, TagCounts& counts)
	{
		if (viewport == nil || command.tags.empty())
			return 0;

		// スタイル名 → RefNumber。**タグ 1 つごとに文書のリソースを引き直すと図面の規模なりに
		// 効いてくる**（1 枚の伏図に横架材の本数だけタグが載る）ので、このビューポートの中では
		// 名前ごとに 1 回だけ引く。引けなければ 0＝スタイル無しで置く。
		std::map<std::string, RefNumber> styles;

		// VCOM のデータタグ支援インターフェース（関連付け・スタイル・更新）。ビューポート
		// 1 枚につき 1 回だけ取る。取れなければ**タグは置くが関連付けとスタイルは省く**
		// （位置だけでも正しいタグが残る方が原因を追いやすい）。
		const VectorWorks::Extension::IDataTagSupportPtr support(
			VectorWorks::Extension::IID_DataTagSupport);

		// 置けたタグ。**注釈へ足した図形のクラスはビューポートで非表示のまま**なので
		// （ConfigureViewport はタグを置く前に走る。ローカル確認で判明）、全部置いてから
		// 改めて全クラスを表示へ戻す。
		std::vector<MCObjectHandle> placed;
		placed.reserve(command.tags.size());

		// 実測を積む（動かすのは全部置いてから。診断へ出す実測を先頭から数件そろえるため）。
		std::vector<PendingTag> pending;
		pending.reserve(command.tags.size());

		std::size_t drawn = 0;
		for (const core::TagCommand& tag : command.tags)
		{
			auto style = styles.find(tag.style);
			if (style == styles.end())
			{
				const RefNumber resolved = ResolvePluginStyle(TXString(tag.style.c_str()));
				style = styles.emplace(tag.style, resolved).first;
				// 「1 度でも引けなかったか」だけを持ち帰る（診断行が長くならないように）。
				if (resolved == 0)
					counts.styleMissing = true;
			}

			const auto found = memberHandles.handles.find(tag.memberIndex);
			const MCObjectHandle member =
				found == memberHandles.handles.end() ? nil : found->second;

			if (PlaceOne(viewport, tag, style->second, member, support, counts, placed, pending))
				++drawn;
		}

		MovePendingTags(pending);

		// クラスを表示へ戻し、ビューポートを更新して反映する。ConfigureViewport は**タグを
		// 置く前**に走っているので、**スタイルがその時点で文書に無かったクラスを持ち込んだ
		// 場合**（タグの中身はスタイルが決める）、ここで戻さないと注釈だけが空白のまま残る。
		// 戻すのはビューポートと同じく**全クラス**（draw/DrawUtil の ShowAllViewportClasses）
		// ——タグが身に付けているクラスを数え上げる必要はない。
		if (!placed.empty())
		{
			counts.classesShown += ShowAllViewportClasses(viewport);
			try
			{
				VWViewportObj(viewport).Update();
			}
			catch (...)
			{
				// 更新できなくてもタグ自体は図面に残る（表示は次の更新で追いつく）。
				++counts.updateFailed;
			}
		}
		return drawn;
	}

	std::string tagDiagnostics(const std::string& label, const TagCounts& counts)
	{
		// **タグを 1 つでも置いたのにクラスを 1 つも表示へ戻せていない**のも異常として扱う
		// （注釈にタグはあるのに図には出ない、という一番分かりにくい壊れ方になる）。
		const bool classesBroken = counts.drawn > 0 && counts.classesShown == 0;
		if (counts.failed == 0 && counts.unassociated == 0 && counts.leaderLeft == 0 &&
			counts.updateFailed == 0 && counts.unmeasured == 0 && !classesBroken &&
			!counts.styleMissing)
			return {};

		std::string text = label + "の断面寸法タグの診断: ";
		if (counts.styleMissing)
			text +=
				"データタグスタイル「断面寸法」が文書にありません（スタイル無しで置きました）。";
		if (counts.failed > 0)
			text += "タグを置けなかった命令 " + std::to_string(counts.failed) + " 件。";
		if (counts.leaderLeft > 0)
			text += "引出線を消せなかったタグ " + std::to_string(counts.leaderLeft) + " 件。";
		if (counts.unassociated > 0)
			text += "関連付け先の横架材が無いタグ " + std::to_string(counts.unassociated) +
					" 件（断面寸法が空になります）。";
		if (classesBroken)
			text += "タグのクラスを表示に戻せませんでした（タグが図に出ません）。";
		if (counts.updateFailed > 0)
			text += "クラスを戻した後に更新できなかったビューポート " +
					std::to_string(counts.updateFailed) + " 枚。";
		if (counts.unmeasured > 0)
			text +=
				"実位置を測れず動かせなかったタグ " + std::to_string(counts.unmeasured) + " 件。";
		return text;
	}
} // namespace HomeskzIfcImport::draw
