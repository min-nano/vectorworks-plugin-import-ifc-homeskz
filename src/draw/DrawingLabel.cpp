//
//	draw/DrawingLabel.cpp
//
//	軸組図の図面ラベル描画の実装。意図・規約は draw/DrawingLabel.h を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API（すべて SDK リファレンス Findings「Drawing Labels」の実測に拠る）:
//	  * gSDK->CreateCustomObject("Drawing Label2", 挿入点, 角度, bInsert) … 図面ラベルの生成
//	  * gSDK->GetCustomObjectProfileGroup / SetCustomObjectProfileGroup   … ラベルレイアウト
//	    （`…InAux` は nil なので見ない）
//	  * gSDK->CreateGroup / DuplicateObject / AddObjectToContainer        … レイアウトの組み直し
//	  * gSDK->SetTextStyleRef / SetTextSize / GetNamedObject             … タイトルの文字
//	  * gSDK->AddViewportAnnotationObject(viewport, object)              … ビューポート注釈へ
//	    （作りたてのビューポートでは GetViewportGroup の注釈が nil なので、そちらは使わない）
//	  * VWParametricObj::SetParamValue("Title", …)                       … 図面タイトル
//	  * gSDK->GetObjectBounds / MoveObject                               … 測って動かす
//
//	【注釈に入らなかったラベルは消す】bInsert=false で作るので、注釈へ入れられなければ
//	どこにも属さないまま残る——失敗したら必ず削除する（データタグと同じ後始末）。
//

#include "PluginPrefix.h"
#include "draw/DrawingLabel.h"
#include "draw/DrawUtil.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 図面ラベルの内部プラグイン名（universal 名）。表示名（"図面ラベル"）とは別物。
		// VW 2026 の既定は 2 のほう（旧版 "Drawing Label" は内部 ID 96。Findings）。
		constexpr const char* kDrawingLabelPlugin = "Drawing Label2";

		// 図面タイトルのパラメータ（universal 名。Findings「パラメータ（16 件）」）。
		constexpr const char* kFieldTitle = "Title";

		// タイトルの文字スタイル（要件）。**図面にあれば当て、無ければ大きさだけを直接
		// 与える**——テンプレート由来の資源なので作らない（データタグの "寸法(6pt)" と同じ）。
		constexpr const char* kTextStyleName = "図面ラベル(10pt)";
		constexpr double kTextSizePoints = 10.0;

		// タイトルの文字を整える。図面に文字スタイル（"図面ラベル(10pt)"）があればそれを当て、
		// 無ければ大きさだけを直接与える（データタグの ApplyFieldTextStyle と同じ流儀）。
		// **複製したテキストへ当てても式は外れない**（Findings: 文字を SetText で潰しても
		// Title が描かれた＝式は文字でも書式でもない隠れた状態が持っている）。
		void ApplyTitleTextStyle(MCObjectHandle text, DrawingLabelCounts& counts)
		{
			const MCObjectHandle resource = gSDK->GetNamedObject(TXString(kTextStyleName));
			if (resource != nil)
			{
				gSDK->SetTextStyleRef(text, gSDK->GetObjectInternalIndex(resource));
				return;
			}
			counts.textStyleMissing = true;
			gSDK->SetTextSize(text, 0, gSDK->GetTextLength(text), kTextSizePoints);
		}

		// ラベルレイアウトを「タイトルのテキストと下線だけ」に組み直す。組めたら true。
		//
		// **複製して、要らないものを落とす**（Findings「SDK から組む手順」）。式はテキストが
		// 抱える隠れた状態なので、新しく作ったテキストでは図面タイトルに置き換わらない。
		// 既定のレイアウトは「タイトル・下線・縮尺・丸・図番」の順なので、**最初の
		// テキスト＝タイトル**・**最初の線＝その下線**を取る（テキストに名前は付いておらず、
		// 見えている文字は UI の言語で変わるので、文字では選ばない）。
		//
		// **中身を入れてから渡す**（空の群を先に渡すと迷子になる。データタグと同じ筋）。
		bool KeepTitleOnly(MCObjectHandle label, DrawingLabelCounts& counts)
		{
			const MCObjectHandle layout = gSDK->GetCustomObjectProfileGroup(label);
			if (layout == nil)
				return false;

			const MCObjectHandle group = gSDK->CreateGroup();
			if (group == nil)
				return false;

			bool tookText = false;
			bool tookLine = false;
			for (MCObjectHandle member = gSDK->FirstMemberObj(layout); member != nil;
				 member = gSDK->NextObject(member))
			{
				const short type = gSDK->GetObjectTypeN(member);
				const bool text = type == kTextNode && !tookText;
				const bool line = type == kLineNode && !tookLine;
				if (!text && !line)
					continue;

				const MCObjectHandle copy = gSDK->DuplicateObject(member);
				if (copy == nil || !gSDK->AddObjectToContainer(copy, group))
				{
					if (copy != nil)
						gSDK->DeleteObject(copy, true);
					continue;
				}
				if (text)
				{
					ApplyTitleTextStyle(copy, counts);
					tookText = true;
				}
				else
					tookLine = true;
			}

			// タイトルが取れなければ組み直さない（既定のレイアウトのまま＝図番と縮尺も出るが、
			// タイトルが消えるよりはよい）。
			if (!tookText || !gSDK->SetCustomObjectProfileGroup(label, group))
			{
				gSDK->DeleteObject(group, true);
				return false;
			}

			// VW が渡した群を複製して持った場合は、こちらの群は図面上の residue なので消す
			// （データタグと同じ後始末。draw/Tag の ResolveTagLayout）。
			const MCObjectHandle held = gSDK->GetCustomObjectProfileGroup(label);
			if (held == nil)
			{
				gSDK->DeleteObject(group, true);
				return false;
			}
			if (held != group)
				gSDK->DeleteObject(group, true);
			return true;
		}

		// 図面タイトルを書く。書けたら true。
		bool WriteTitle(MCObjectHandle label, const std::string& title)
		{
			try
			{
				VWParametricObj(label).SetParamValue(TXString(kFieldTitle),
													 TXString(title.c_str()));
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		// ビューポートの縮尺（分母）。読めなければ 1（＝用紙 mm をそのままモデル mm と
		// みなす。間隔が縮むだけで、ラベルは置ける）。
		double ViewportScale(MCObjectHandle viewport)
		{
			try
			{
				const double scale = VWViewportObj(viewport).GetScale();
				return scale > 0.0 ? scale : 1.0;
			}
			catch (...)
			{
				return 1.0;
			}
		}
	} // namespace

	void prepareDrawingLabelPlugin()
	{
		PrepareCustomObjectDefinition(kDrawingLabelPlugin);
	}

	bool drawSectionLabel(MCObjectHandle viewport, const std::string& title,
						  const core::Vec2& anchor, double drop, DrawingLabelCounts& counts)
	{
		if (viewport == nil)
			return false;

		// bInsert=false: どのレイヤにも入れず、この後 AddViewportAnnotationObject で注釈へ
		// 入れる（Findings の手順）。挿入点は目標の近くにしておく——最終位置は下で測って
		// 決めるので、ここは目安でよい。
		MCObjectHandle label = nil;
		{
			VW_DRAW_TIME("図面ラベル:生成");
			label = gSDK->CreateCustomObject(TXString(kDrawingLabelPlugin),
											 WorldPt(anchor.x, anchor.y), 0.0, false);
		}
		if (label == nil)
		{
			++counts.failed;
			return false;
		}

		bool annotated = false;
		{
			VW_DRAW_TIME("図面ラベル:注釈へ移す");
			annotated = gSDK->AddViewportAnnotationObject(viewport, label);
		}
		if (!annotated)
		{
			gSDK->DeleteObject(label, true);
			++counts.failed;
			return false;
		}

		// **レイアウトを組み直す**（タイトルと下線だけ。図番と縮尺を落とす）。Findings が
		// 実測したのと同じ「注釈へ入れた後」に行う。組めなくてもラベルは置く——タイトルは
		// 出るので、図番と縮尺が残るだけで済む（件数を診断へ回す）。
		{
			VW_DRAW_TIME("図面ラベル:レイアウト");
			if (!KeepTitleOnly(label, counts))
				++counts.layoutFailed;
		}

		// **注釈へ入れた後に Title を書く**。置いた直後の Title は隣のビューポートの
		// タイトルが入っていることがある（Findings「ビューポートとの紐づき」）。リンクは
		// 生きたままなので、後から利用者がビューポートの図面タイトルを変えればラベルも
		// 追随する。書けなくてもラベルは残す（VW がリンクで入れた値が出る）。
		WriteTitle(label, title);
		{
			VW_DRAW_TIME("図面ラベル:リセット");
			gSDK->ResetObject(label);
		}

		// **測って動かす**——上端中央を「建物の最下点の中央から用紙で drop 下」へ合わせる
		// （drop は下に出る寸法の帯を含む。core::sectionLabelDrop）。注釈空間の長さは
		// モデル mm なので、用紙 mm に縮尺を掛ける。
		WorldRect bounds;
		if (!gSDK->GetObjectBounds(label, bounds))
		{
			++counts.unmeasured;
			++counts.drawn;
			return true;
		}
		const double targetX = anchor.x;
		const double targetTop = anchor.y - (drop * ViewportScale(viewport));
		// WorldRect は top > bottom（Y 上向き）。
		const double centreX = (bounds.left + bounds.right) / 2.0;
		gSDK->MoveObject(label, targetX - centreX, targetTop - bounds.top);
		++counts.drawn;
		return true;
	}

	std::string drawingLabelDiagnostics(const std::string& label, const DrawingLabelCounts& counts)
	{
		// 異常が無ければ 1 行も出さない（うまくいった取り込みでは雑音でしかない）。
		if (counts.failed == 0 && counts.layoutFailed == 0 && counts.unmeasured == 0 &&
			!counts.textStyleMissing)
			return {};

		std::string text = label + "の図面ラベルの診断: ";
		AppendCount(text, "図面ラベルを置けなかった軸組図", counts.failed, "枚");
		AppendCount(text, "ラベルレイアウトを組み直せなかった図面ラベル", counts.layoutFailed, "件",
					"図番と縮尺も表示されます");
		AppendCount(text, "実位置を測れず動かせなかった図面ラベル", counts.unmeasured, "件");
		if (counts.textStyleMissing)
			text += std::string("文字スタイル「") + kTextStyleName +
					"」が図面に無いので大きさだけを与えました。";
		return text;
	}
} // namespace HomeskzIfcImport::draw
