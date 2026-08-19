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
#include <set>
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

		// 較正の基準点候補（下記 CalibrateAnnotationOrigin）。VW がタグを吸着させる点が
		// 部材のどこなのかは分からないので、**実測から選ぶ**。命令は天端線の始端（anchor）と
		// その中央（position）を持つので、終端は 2·position − anchor で出る。
		enum class AnchorGuess
		{
			Mid,
			Start,
			End,
		};

		core::Vec2 GuessedAnchor(const core::TagCommand& tag, AnchorGuess guess)
		{
			switch (guess)
			{
			case AnchorGuess::Start:
				return tag.anchor;
			case AnchorGuess::End:
				return core::Vec2{(2.0 * tag.position.x) - tag.anchor.x,
								  (2.0 * tag.position.y) - tag.anchor.y};
			case AnchorGuess::Mid:
			default:
				return tag.position;
			}
		}

		const char* AnchorGuessLabel(AnchorGuess guess)
		{
			switch (guess)
			{
			case AnchorGuess::Start:
				return "始端";
			case AnchorGuess::End:
				return "終端";
			case AnchorGuess::Mid:
			default:
				return "中央";
			}
		}

		// 注釈へ置いたタグ 1 つの実測。移動は全部置いてから（較正の後で）行う。
		struct PendingTag
		{
			MCObjectHandle object = nil;
			const core::TagCommand* command = nullptr;
			double centreX = 0.0; // 置いた直後の実位置（＝VW が吸着させた場所）
			double centreY = 0.0;
			double clearX = 0.0; // 部材から逃がすベクトル（実寸から求めた）
			double clearY = 0.0;
			bool associated = false; // 関連付けできたか（較正に使えるのはこれだけ）
		};

		// **注釈空間の原点を実測から較正する。**
		//
		// 断面（軸組図）の注釈空間は、モデルの投影と**平行移動ぶんだけ**ずれている
		// （実機で、高さは合うのに横だけ一定量ずれた＝回転も反転もしていない）。その
		// ずれ量は断面ごとに違いうるし、規約も分からない。そこで**当てずっぽうで決めず、
		// VW 自身が置いた位置から測る**——データタグは関連付けると関連付け先へ吸着するので、
		// 「タグの実位置 − その部材の基準点」がそのままずれ量になる。
		//
		// ただし**吸着する基準点が部材のどこか**（始端／中央／終端）が分からない。そこで
		// 3 通りとも試し、**ずれ量のばらつき（残差）が最も小さいもの**を採る——基準点の
		// 読みが正しければずれ量は全タグで一定になり、外れていれば部材の長さぶんばらつく
		// ので、部材の長さが揃っていない限り一意に決まる。選んだ結果と残差は診断行へ出す
		// （次のローカル確認で規約そのものが分かる）。
		Calibration CalibrateAnnotationOrigin(const std::vector<PendingTag>& pending)
		{
			Calibration best;
			bool first = true;
			for (const AnchorGuess guess : {AnchorGuess::Mid, AnchorGuess::Start, AnchorGuess::End})
			{
				double sumX = 0.0;
				double sumY = 0.0;
				for (const PendingTag& tag : pending)
				{
					const core::Vec2 anchor = GuessedAnchor(*tag.command, guess);
					sumX += tag.centreX - anchor.x;
					sumY += tag.centreY - anchor.y;
				}
				const auto count = static_cast<double>(pending.size());
				const double meanX = sumX / count;
				const double meanY = sumY / count;

				double residual = 0.0;
				for (const PendingTag& tag : pending)
				{
					const core::Vec2 anchor = GuessedAnchor(*tag.command, guess);
					residual = std::max(residual, std::hypot((tag.centreX - anchor.x) - meanX,
															 (tag.centreY - anchor.y) - meanY));
				}

				if (first || residual < best.residual)
				{
					best.anchor = AnchorGuessLabel(guess);
					best.offsetX = meanX;
					best.offsetY = meanY;
					best.residual = residual;
					best.samples = pending.size();
					first = false;
				}
			}
			return best;
		}

		// 置いたタグをまとめて目標へ動かす。
		//
		// **絶対で置くもの（伏図）と較正して置くもの（軸組図）を分ける。** 伏図は注釈空間が
		// モデルの平面座標そのものだと実機で確認できているので、測った実位置から目標の
		// 絶対位置へ動かせば正確に決まる。軸組図は原点が分からないので、関連付けできた
		// タグ全体から**ずれ量を較正**し、それを目標へ足す（CalibrateAnnotationOrigin）。
		// 較正に使えるタグが 1 つも無ければ（＝どれも関連付けできなかった）絶対で置く。
		void MovePendingTags(const std::vector<PendingTag>& pending, TagCounts& counts)
		{
			std::vector<PendingTag> calibrated;
			for (const PendingTag& tag : pending)
			{
				if (tag.command->placement == core::TagPlacement::RelativeToAnchor &&
					tag.associated)
					calibrated.push_back(tag);
			}

			Calibration calibration;
			if (!calibrated.empty())
			{
				calibration = CalibrateAnnotationOrigin(calibrated);
				// 診断行には最初のビューポートの較正だけを出す（規約を読み取るには 1 枚で
				// 足りる。全枚数ぶん並べると診断行が読めなくなる）。
				if (counts.calibration.samples == 0)
					counts.calibration = calibration;
			}

			for (const PendingTag& tag : pending)
			{
				const bool useCalibration =
					tag.command->placement == core::TagPlacement::RelativeToAnchor &&
					tag.associated && !calibrated.empty();
				const double originX = useCalibration ? calibration.offsetX : 0.0;
				const double originY = useCalibration ? calibration.offsetY : 0.0;
				const double targetX = tag.command->position.x + tag.clearX + originX;
				const double targetY = tag.command->position.y + tag.clearY + originY;
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
			const bool associated = member != nil && support;
			if (associated)
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

			// **ここで実位置と実寸を測る**。ここまでで VW はタグを関連付け先へ吸着させ、
			// スタイルが本文を流し込んでタグの実寸が確定している。動かすのは全部置いてから
			// （較正に全タグの実測が要る。CalibrateAnnotationOrigin）。
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
			pending.associated = associated;
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
		// （PrepareViewportSetup はデザインレイヤしか走査しない。ローカル確認で判明）、
		// 全部置いてから、そのクラスをまとめて表示へ戻す。
		std::vector<MCObjectHandle> placed;
		placed.reserve(command.tags.size());

		// 実測を積む（動かすのは全部置いてから。較正に全タグぶんの実測が要る）。
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

		MovePendingTags(pending, counts);

		// タグ（とスタイルが決めるその中身）のクラスを表示へ戻し、ビューポートを更新して
		// 反映する。ConfigureViewport は**タグを置く前**に走っているので、ここで足さないと
		// 注釈だけが空白のまま残る。
		if (!placed.empty())
		{
			std::set<InternalIndex> classes;
			for (const MCObjectHandle object : placed)
			{
				const std::vector<InternalIndex> used = CollectObjectClasses(object);
				classes.insert(used.begin(), used.end());
			}
			if (!classes.empty())
			{
				counts.classesShown +=
					ShowViewportClasses(viewport, {classes.begin(), classes.end()});
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
		}
		return drawn;
	}

	namespace
	{
		// 診断行に出す数値（mm）。小数は要らないので整数へ丸めて短くする。
		std::string Round(double value)
		{
			return std::to_string(static_cast<long long>(std::llround(value)));
		}
	} // namespace

	std::string tagDiagnostics(const std::string& label, const TagCounts& counts)
	{
		// **タグを 1 つでも置いたのにクラスを 1 つも表示へ戻せていない**のも異常として扱う
		// （注釈にタグはあるのに図には出ない、という一番分かりにくい壊れ方になる）。
		const bool classesBroken = counts.drawn > 0 && counts.classesShown == 0;
		// 較正の結果は**異常でなくても出す**。断面の注釈空間の規約はまだ分かっておらず、
		// この 1 行がローカル確認で規約を読み取る唯一の手掛かりになる（draw/Tag.h の
		// Calibration）。残差が大きければ基準点の読みが外れているということ。
		const bool hasCalibration = counts.calibration.samples > 0;
		if (counts.failed == 0 && counts.unassociated == 0 && counts.leaderLeft == 0 &&
			counts.updateFailed == 0 && counts.unmeasured == 0 && !classesBroken &&
			!counts.styleMissing && !hasCalibration)
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
		if (hasCalibration)
		{
			const Calibration& c = counts.calibration;
			text += "注釈空間の較正: 基準=" + c.anchor + " / ずれ (" + Round(c.offsetX) + ", " +
					Round(c.offsetY) + ") / 残差 " + Round(c.residual) + " / 標本 " +
					std::to_string(c.samples) + " 件。";
		}
		return text;
	}
} // namespace HomeskzIfcImport::draw
