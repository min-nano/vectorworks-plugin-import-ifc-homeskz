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
//	      SetDataTagStyle    … データタグスタイルの関連付け
//	      AssociateWithObject … 対象の横架材へ関連付け（Python 版 DT_AssociateWithObj）
//	      UpdateDataTag       … 関連付け後の再計算（Python 版 DT_UpdateTaggedTags）
//
//	スタイルを作るとき（createTagStyle）に使う SDK API:
//	  * gSDK->GetNamedObject                        … 名前が空いているかを見る
//	  * gSDK->CreateSymbolDefinition(inoutName)     … スタイルの実体＝シンボル定義
//	  * gSDK->SetSymbolDefSubType(symDef, 内部 ID)  … そのシンボル定義を**スタイルにする**
//	  * gSDK->AddObjectToContainer                  … PIO・テキストを容れ物へ入れる
//	  * gSDK->GetCustomObjectProfileGroup / SetCustomObjectProfileGroup … タグレイアウト
//	  * gSDK->CreateGroup / CreateTextBlock / SetObjectName / SetTextStyleRef … レイアウトの中身
//	  * VectorWorks::Extension::IDataTagTextLinkSupport（VCOM）
//	      SetIsLinked / SetFormula … テキストを**タグフィールド**にする（式を持たせる）
//	  * IDataTagSupport::UpdateUserDefinedTextsUIDs … スタイルにフィールドを認識させる
//
//	【注釈に入らなかったタグは消す】AddViewportAnnotationObject に失敗すると、タグは
//	**生成したときのカレントレイヤ（シートレイヤ）に residue として残る**——図面の上に
//	寸法だけが浮くので、失敗したら必ず削除する。
//

#include "PluginPrefix.h"
#include "draw/Tag.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"
#include "core/Document.h"

#include "Interfaces/VectorWorks/Extension/IDataTagSupport.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 生成したスタイルの実体（宣言は draw/TagStyle.h）。**実描画はローカルの VW でしか
	// 確認できない**ので、「どこまでできたか」を段階ごとに残す（tagStyleDiagnostics が
	// 1 行にする）。
	struct TagStyleRecord
	{
		RefNumber style = 0; // 生成できたスタイル（0＝作れなかった＝スタイル無しで置く）
		std::string requested; // 求めた基準名（命令のスタイル名）
		std::string name; // 実際に付いた名前（基準名が埋まっていれば "-2" 等が付く）
		std::string failure; // 躓いた段階（空＝最後まで作れた）
		bool attempted = false; // 生成を試みたか（1 回の取り込みで 1 回だけ試みる）
		bool renamed = false; // 基準名が埋まっていて別名になった
		bool layoutCreated = false; // タグレイアウト（プロファイルグループ）を自分で作った
		bool textStyleMissing = false; // 文字スタイル（"寸法(6pt)"）が文書に無かった
		bool labelMissing = false; // フィールドラベル（テキストの名前）を付けられなかった
		bool linkMissing = false; // テキストをタグフィールドにできなかった（式が入らない）
		bool leaderLeft = false; // スタイル側の引出線を OFF にできなかった
	};

	namespace
	{
		// データタグの内部プラグイン名（Python 版 vw/sheet.py _DATA_TAG_PLUGIN）。VW 標準の
		// データタグツールの universal 名で、表示名（"データタグ"）とは別物。
		constexpr const char* kDataTagPlugin = "Data Tag";

		// --- 生成するスタイルの中身（draw/Tag.h「スタイルは作って使う」）-----------------

		// タグフィールドのラベル。VW はレイアウトの中の**テキストの名前**をフィールドの
		// ラベルとして扱う（タグデータの取り出し口 IDataTagSupport::GetDataTagExtractedData も
		// ラベルで引く）ので、テキストへこの名前を付ける。
		constexpr const char* kFieldLabel = "断面寸法";

		// フィールドの文字スタイル。**文書にあれば当て、無ければ大きさだけを直接与える**
		// （テンプレート由来の資源なので、無い文書でも寸法が読める大きさにはしておく）。
		constexpr const char* kTextStyleName = "寸法(6pt)";
		constexpr double kTextSizePoints = 6.0;

		// スタイル名が埋まっていたときに足す通し番号の上限。ここまで埋まっている文書は
		// 事実上あり得ないが、無限ループにしないための歯止め。
		constexpr int kNameSuffixLimit = 999;

		// タグフィールドの式（VW のタグフィールド定義式）。構造材の断面幅×せいを mm 整数で
		// 並べ、勾配（IPZL）が 0 でないときだけ括弧付きで添える。**レコード名・フィールド名は
		// draw/StructuralMember の定義から組む**——構造材を書いているのはこちらなので、
		// 名前を 2 か所に書かない（CLAUDE.md「重複を作らない置き場所」）。
		TXString TagFieldFormula()
		{
			TXString formula;
			formula += "#";
			formula += kStructuralMemberPlugin;
			formula += "#.#";
			formula += kFieldMajorBreadth;
			formula += "##mm_0_0#×#";
			formula += kStructuralMemberPlugin;
			formula += "#.#";
			formula += kFieldMajorDepth;
			formula += "##mm_0_0#";
			// 勾配の添え書き。式の記法（条件・区切り）は VW のタグフィールド定義そのままで、
			// 意味を持たせずに写す。
			formula += R"FML(" ("@#IPZL#<>0:""#IPZL##thsep#sign#@#IPZL#<>0:""")"@#IPZL#<>0:"")FML";
			return formula;
		}

		// 「引出線を表示」パラメータ（既定 ON）。部材の面ちょうどに置いても ON のままだと
		// 引出線が描かれるので OFF にする（Python 版 _LEADER_FIELD / _LEADER_OFF）。
		// universal 名で見つからなければ OIP の日本語名で引き直す（draw/DrawUtil の
		// ResolveParamName。名前が 1 つ違うだけで setter は黙って無視される）。
		constexpr const char* kFieldUseLeader = "Use Leader";
		constexpr const char* kLocalizedUseLeader = "引出線を表示";

		// 引出線を OFF にする（消せたら true）。**タグ本体とスタイルの中の PIO で同じ手順を
		// 使う**ので 1 か所に置く。universal 名で引けない環境（日本語 UI）に備えて OIP の
		// 表示名でも引き直す（ResolveParamName）。
		bool TurnOffLeader(MCObjectHandle object)
		{
			try
			{
				VWParametricObj pio(object);
				const TXString param = ResolveParamName(pio, kFieldUseLeader, kLocalizedUseLeader);
				pio.SetParamBool(param, false);
				return true;
			}
			catch (...)
			{
				// 引出線が残るだけでタグ自体は使えるので、失敗しても続ける（呼び出し側が
				// 件数を数えて診断へ回す）。
				return false;
			}
		}

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

			// スタイル（createTagStyle がこの取り込みのために作ったもの）。作れていなければ
			// **スタイル無しで置く**——タグを失うより、位置だけでも正しいタグを残した方が
			// 原因を追いやすい（構造材のプラグインスタイルと同じ方針）。
			//
			// **skipValidation=true** を渡して検証を止める。関連付けを先に済ませてあれば
			// 本来は通るはずだが、この検証は**ダイアログでユーザーに聞く**造りなので、
			// 1 件でも引っかかるとインポートが止まってしまう（無人で走らせられない）。
			// 互換性が無ければタグの本文が空になるだけで図面は壊れないので、ここは黙って
			// 進めて結果を目で見てもらう方がよい。
			if (style != 0 && support)
				support->SetDataTagStyle(object, style, /*skipValidation=*/true);

			// 引出線を OFF にする（スタイル側でも切ってあるが、命令ごとに念を入れる）。
			if (!TurnOffLeader(object))
				++counts.leaderLeft;

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

		// 文書で空いている名前。基準名がそのまま空いていればそれを、埋まっていれば
		// "-2"、"-3" … と後ろを足す（**既存のスタイルを乗っ取らない**。draw/Tag.h）。
		// どれも埋まっていれば空文字（呼び出し側はスタイル無しへ落ちる）。
		TXString UnusedResourceName(const std::string& base, bool& outRenamed)
		{
			outRenamed = false;
			const TXString wanted(base.c_str());
			if (gSDK->GetNamedObject(wanted) == nil)
				return wanted;

			outRenamed = true;
			for (int suffix = 2; suffix <= kNameSuffixLimit; ++suffix)
			{
				const TXString candidate((base + "-" + std::to_string(suffix)).c_str());
				if (gSDK->GetNamedObject(candidate) == nil)
					return candidate;
			}
			return TXString();
		}

		// フィールドの文字を整える。文書に文字スタイル（"寸法(6pt)"）があればそれを当て、
		// 無ければ大きさだけを直接与える（**その文書でも寸法が読める**ようにする）。
		void ApplyFieldTextStyle(MCObjectHandle text, Sint32 length, TagStyleRecord& record)
		{
			const MCObjectHandle resource = gSDK->GetNamedObject(TXString(kTextStyleName));
			if (resource != nil)
			{
				gSDK->SetTextStyleRef(text, gSDK->GetObjectInternalIndex(resource));
				return;
			}

			// 文字スタイルが無い文書。大きさだけを与えて先へ進む（診断に残す）。
			record.textStyleMissing = true;
			gSDK->SetTextSize(text, 0, length, kTextSizePoints);
		}

		// タグレイアウト（＝タグの中身を描くグループ）を用意する。PIO が既に持っていれば
		// それを使い、無ければ作って与える。**プロファイルグループ**は PIO が持つ「編集できる
		// 中身」の入れ物で、データタグではこれがタグレイアウトにあたる。
		MCObjectHandle ResolveTagLayout(MCObjectHandle pio, TagStyleRecord& record)
		{
			MCObjectHandle layout = gSDK->GetCustomObjectProfileGroup(pio);
			if (layout != nil)
				return layout;

			layout = gSDK->CreateGroup();
			if (layout == nil)
				return nil;
			record.layoutCreated = true;
			if (!gSDK->SetCustomObjectProfileGroup(pio, layout))
			{
				gSDK->DeleteObject(layout, true);
				return nil;
			}
			return layout;
		}

		// レイアウトへ断面寸法フィールドを 1 つ置く。フィールドの実体は**式を持たせた
		// テキスト**（リンクされたテキスト）で、ラベルはテキストの名前。
		bool AddTagField(MCObjectHandle layout, TagStyleRecord& record)
		{
			const TXString formula = TagFieldFormula();

			// 式そのものを本文にしておく（スタイルが評価するまでの見た目であり、評価後は
			// 断面寸法に置き換わる）。fixedSize=false で幅は中身なり。
			const MCObjectHandle text = gSDK->CreateTextBlock(formula, WorldPt(0.0, 0.0), false, 0);
			if (text == nil)
				return false;

			if (!gSDK->AddObjectToContainer(text, layout))
			{
				gSDK->DeleteObject(text, true);
				return false;
			}

			ApplyFieldTextStyle(text, static_cast<Sint32>(formula.GetLength()), record);

			// フィールドラベル＝テキストの名前。既に同じ名前の資源がある文書では付かない
			// （ラベルが無いだけでタグは出るので、診断に残して先へ進む）。
			if (gSDK->SetObjectName(text, TXString(kFieldLabel)) != 0)
				record.labelMissing = true;

			// **ここでテキストがタグフィールドになる。** リンクを立てて式を持たせる。
			const VectorWorks::Extension::IDataTagTextLinkSupportPtr link(
				VectorWorks::Extension::IID_DataTagTextLinkSupport);
			if (!link)
			{
				record.linkMissing = true;
				return true;
			}
			link->SetIsLinked(text, true);
			link->SetFormula(text, formula);
			return true;
		}

		// 命令セットが求めているスタイル名（＝ parse/Tag の kTagStyle）。タグが 1 つも無ければ
		// 空文字。**名前の持ち主は解析側**なので、描画側は命令から受け取る（draw/ は parse/ を
		// include しない。CLAUDE.md「依存の向きは厳守する」）。
		std::string RequestedStyleName(const core::Document& document)
		{
			for (const core::SheetCommand& sheet : document.sheets)
				for (const core::TagCommand& tag : sheet.viewport.tags)
					if (!tag.style.empty())
						return tag.style;
			for (const core::SectionCommand& section : document.sections)
				for (const core::TagCommand& tag : section.viewport.tags)
					if (!tag.style.empty())
						return tag.style;
			return {};
		}
	} // namespace

	TagStyle::TagStyle() : fRecord(std::make_unique<TagStyleRecord>()) {}

	TagStyle::~TagStyle() = default;

	void createTagStyle(const core::Document& document, TagStyle& style)
	{
		TagStyleRecord& record = style.record();
		if (record.attempted)
			return; // 1 回の取り込みで 1 つ（伏図・軸組図が同じスタイルを共有する）

		record.requested = RequestedStyleName(document);
		if (record.requested.empty())
			return; // タグが 1 つも無い文書には資源を足さない

		record.attempted = true;

		// スタイルの中に置く PIO を作るので、タグ本体と同じく**設定ダイアログを出さない**
		// 定義を先に用意する（draw/Tag.h の prepareDataTagPlugin）。
		prepareDataTagPlugin();

		bool renamed = false;
		TXString name = UnusedResourceName(record.requested, renamed);
		if (name.IsEmpty())
		{
			record.failure = "名前が空いていません";
			return;
		}
		record.renamed = renamed;

		// **プラグインオブジェクトスタイルの実体はシンボル定義**で、そのサブタイプに PIO の
		// 内部 ID が入っているものがスタイルとして扱われる（draw/Tag.h）。名前は in/out で、
		// VW が調整することがあるので**戻ってきた方**を控える。
		const MCObjectHandle symDef = gSDK->CreateSymbolDefinition(name);
		if (symDef == nil)
		{
			record.failure = "シンボル定義を作れませんでした";
			return;
		}
		record.name = name.GetStdString();
		gSDK->SetSymbolDefSubType(symDef, kInternalID_DataTag);

		// スタイルが持つパラメータの本体＝データタグ PIO 1 つ。図面には出さない
		// （bInsert=false で作ってシンボル定義へ入れる）。
		const MCObjectHandle pio =
			gSDK->CreateCustomObject(TXString(kDataTagPlugin), WorldPt(0.0, 0.0), 0.0, false);
		if (pio == nil || !gSDK->AddObjectToContainer(pio, symDef))
		{
			if (pio != nil)
				gSDK->DeleteObject(pio, true);
			gSDK->DeleteSymbolDefinition(symDef, true);
			record.name.clear();
			record.failure = "スタイルの中のデータタグを作れませんでした";
			return;
		}

		// 引出線はスタイルの側でも切っておく（タグは部材の面ちょうどに置く。parse/Tag.h）。
		record.leaderLeft = !TurnOffLeader(pio);

		// タグレイアウト（断面寸法フィールド 1 つ）。**ここが空だとタグは何も表示しない**ので、
		// 作れなければスタイルごと捨ててスタイル無しへ落とす（中身の無いスタイルを文書へ
		// 残さない）。
		const MCObjectHandle layout = ResolveTagLayout(pio, record);
		if (layout == nil || !AddTagField(layout, record))
		{
			gSDK->DeleteSymbolDefinition(symDef, true);
			record.name.clear();
			record.failure = "タグレイアウトを作れませんでした";
			return;
		}

		gSDK->ResetObject(pio);

		// スタイルにフィールドを認識させる（これをしないとタグ側が式を拾わない）。
		const VectorWorks::Extension::IDataTagSupportPtr support(
			VectorWorks::Extension::IID_DataTagSupport);
		if (support)
			support->UpdateUserDefinedTextsUIDs(symDef);

		record.style = static_cast<RefNumber>(gSDK->GetObjectInternalIndex(symDef));
		if (record.style == 0)
			record.failure = "作ったスタイルを参照できませんでした";
	}

	std::string tagStyleDiagnostics(const TagStyle& style)
	{
		const TagStyleRecord& record = style.record();
		if (!record.attempted)
			return {}; // タグの無い文書（何も作っていない）

		std::string text = "断面寸法データタグスタイルの診断: ";
		if (record.style == 0)
		{
			return text + "スタイルを作れませんでした（" +
				   (record.failure.empty() ? std::string("原因不明") : record.failure) +
				   "）。タグはスタイル無しで置きます。";
		}

		// 作れたときは**付いた名前**を必ず出す（どのスタイルが増えたかが図面と突き合わせ
		// られる）。以降は引っかかった点だけを足す。
		text += "「" + record.name + "」を作りました。";
		if (record.renamed)
			text += "（基準名「" + record.requested + "」は文書に在るので別名にしました。）";
		if (record.layoutCreated)
			text += "タグレイアウトは新しく作りました。";
		if (record.textStyleMissing)
			text += std::string("文字スタイル「") + kTextStyleName +
					"」が文書に無いので大きさだけを与えました。";
		if (record.labelMissing)
			text += std::string("フィールドラベル「") + kFieldLabel +
					"」を付けられませんでした（同じ名前の資源があります）。";
		if (record.linkMissing)
			text += "タグフィールドの式を入れられませんでした（寸法が空になります）。";
		if (record.leaderLeft)
			text += "スタイルの引出線を OFF にできませんでした。";
		return text;
	}

	void prepareDataTagPlugin()
	{
		gSDK->DefineCustomObject(TXString(kDataTagPlugin), kCustomObjectPrefNever);
	}

	std::size_t drawViewportTags(MCObjectHandle viewport, const core::ViewportCommand& command,
								 const ObjectHandleTable& memberHandles, const TagStyle& style,
								 TagCounts& counts)
	{
		if (viewport == nil || command.tags.empty())
			return 0;

		// スタイルは**この取り込みのために作った 1 つ**（createTagStyle）。文書のリソースを
		// 名前で引き直さないので、タグの本数ぶんの検索も要らない。作れていなければ 0＝
		// スタイル無しで置く（原因は tagStyleDiagnostics が別行で説明する）。
		const RefNumber styleRef = style.record().style;
		if (styleRef == 0)
			counts.styleMissing = true;

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
			const auto found = memberHandles.handles.find(tag.memberIndex);
			const MCObjectHandle member =
				found == memberHandles.handles.end() ? nil : found->second;

			if (PlaceOne(viewport, tag, styleRef, member, support, counts, placed, pending))
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
			text += "データタグスタイルを作れていないので、スタイル無しで置きました。";
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
