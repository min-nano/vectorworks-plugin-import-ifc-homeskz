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
//	      AssociateWithObject        … 対象の横架材へ関連付け
//	      UpdateUserDefinedTextsUIDs … タグにタグフィールドを認識させる
//	      UpdateDataTag              … 関連付け後の再計算
//
//	タグレイアウト（＝タグ 1 本の中身。**スタイルは作らない・当てない**。draw/Tag.h の ★）を
//	組むときに使う SDK API:
//	  * gSDK->GetCustomObjectProfileGroup / SetCustomObjectProfileGroup … タグレイアウト
//	  * gSDK->GetCustomObjectProfileGroupInAux                          … レイアウトのもう 1 つの入り口
//	  * gSDK->CreateGroup / CreateTextBlock / SetTextStyleRef / GetNamedObject … レイアウトの中身
//	  * gSDK->AddObjectToContainer                  … テキストをレイアウトへ入れる
//	  * gSDK->FirstMemberObj / NextObject / GetObjectTypeN … 中身の数え上げ（ロクス除去・診断）
//	  * VectorWorks::Extension::IDataTagTextLinkSupport（VCOM）
//	      SetIsLinked / SetFormula … テキストを**タグフィールド**にする（式を持たせる）
//
//	クラス分け（draw/DrawUtil の SetClassByName / SetAllAttributesByClass）は**タグ本体と
//	レイアウトの中のテキストの両方**へ行う（どちらも "寸法" クラス・描画属性は全て by-class）。
//	他の要素と同じ定型で、見え方を図面側のクラスに預けるため（下記 kTagClass）。
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
#include "core/Layout.h"

#include "Interfaces/VectorWorks/Extension/IDataTagSupport.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// データタグの内部プラグイン名。VW 標準のデータタグツールの universal 名で、
		// 表示名（"データタグ"）とは別物。
		constexpr const char* kDataTagPlugin = "Data Tag";

		// --- タグレイアウト（タグ 1 本の中身。draw/Tag.h「タグレイアウト＝タグ 1 本の中身」）---

		// フィールドの文字スタイル。**文書にあれば当て、無ければ大きさだけを直接与える**
		// （テンプレート由来の資源なので、無い文書でも寸法が読める大きさにはしておく）。
		constexpr const char* kTextStyleName = "寸法(6pt)";
		constexpr double kTextSizePoints = 6.0;

		// 断面寸法タグのクラスは**寸法と同じ "寸法"**（draw/DrawUtil の kDimensionClass）。
		// **タグ本体（PIO）とタグレイアウトの中のテキストの両方**をこのクラスに置き、描画
		// 属性はすべてクラス属性に従わせる（SetAllAttributesByClass）——寸法の見え方
		// （色・線の太さ）を図面側のクラスで一括して決められるようにするため。**タグの中の
		// テキストにも要る**——タグレイアウトの中身はタグ本体のクラスを継がない。
		constexpr const char* kTagClass = kDimensionClass;

		// 高さの注記を部材の高さに連動させる綴り。どちらも**階の高さを基準にした**値
		// （部材の絶対Z − 部材の居るレイヤが属する階の高さ）で、SDK リファレンス
		// Findings「Data Tags」の「傾斜材の両端の天端」で実機確認済み。
		//   * kStartTopToken … 挿入点の高さ (Z)_ストーリの高さ＝**始端の天端**
		//   * kHighTopToken  … バウンディングボックス上面の高さ (Z)_ストーリの高さ＝**高い端の
		//                      天端**（断面の角が天端より上に出ることはない。同 Findings）
		// 始端を低い端にそろえるのは横架材の描画（draw/Member の「始端は低い端」）。
		constexpr const char* kStartTopToken = "#IPZS#";
		constexpr const char* kHighTopToken = "#ZTBBS#";
		// 数値の書式。mm の整数・3 桁ごとのコンマ・正に "+"（0 は "±0"）で、取り込んだ時点の
		// 文字（core::signedMillimetreText）と同じ体裁になる（ご要望）。**#thsep# は単位・
		// 精度の修飾子の後ろでだけ効く**ので mm_0_0（単位記号なし）を前に置く。単位の
		// 修飾子は値を換算するので必ず mm（Findings「単位・精度の修飾子」）。修飾子は
		// **各綴りの直後**に付ける（式の後ろでは効かない。同 Findings）。
		constexpr const char* kHeightFormat = "#mm_0_0#thsep#sign#";

		// 連動する高さの注記の式（" (2FL -872~-40)" / 水平な材は " (2FL -872)"）。
		// Findings の推奨の 1 本に書式を足したもの:
		//   " (2FL "<始端>"~"@<高い端><><始端>:""<高い端>@<高い端><><始端>:""")"
		// 条件（"~"@条件:""）で、両端が同じ高さなら "~高い端" を畳む——水平な材と傾斜材で
		// 式を分けずに済み、後から材を傾けても注記が追随する。**演算を混ぜない**（連結と
		// 演算を混ぜると式が空になるか括弧が印字される。Findings「連結と演算は素直には
		// 混ざらない」）。基準名は解析側が引用符を含まないものにしてある（validateDocument）。
		std::string LinkedHeightFormula(const std::string& datum)
		{
			const std::string start = std::string(kStartTopToken) + kHeightFormat;
			const std::string high = std::string(kHighTopToken) + kHeightFormat;
			// 両端が違う高さのときだけ値を出す条件（"<値>@<条件>:"" "）。
			const std::string sloped =
				std::string("@") + kHighTopToken + "<>" + kStartTopToken + ":\"\"";
			return "\" (" + datum + " \"" + start + "\"~\"" + sloped + high + sloped + "\")\"";
		}

		// タグフィールドの式（VW のタグフィールド定義式）。構造材の断面幅×せいを mm 整数で
		// 並べ、**高さの注記**があれば後ろへ添える。
		// **レコード名・フィールド名は draw/StructuralMember の定義から組む**——構造材を書いて
		// いるのはこちらなので、名前を 2 か所に書かない（CLAUDE.md「重複を作らない置き場所」）。
		//
		// 【高さは部材から読む】linkHeight なら注記の数値を式で部材から読む
		// （LinkedHeightFormula）——**材を動かしても注記が追随する**（ご要望）。階の高さを
		// 基準にした綴りなので、横架材の高さごとに伏図のレイヤが分かれても、同じ階に属して
		// いれば基準は FL のまま変わらない（以前の IPZL＝レイヤ基準はここで壊れた。
		// docs/DEV-NOTES.md M36）。
		// 連動できない材は、解析側が書いた文字（note）をそのまま置く（取り込んだ時点の値で、
		// 材を動かしても追随しない）。
		TXString TagFieldFormula(const core::TagCommand& tag, bool linkHeight)
		{
			const std::string& note = tag.note;
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
			// 注記は**二重引用符で囲んだ文字列**として置く。裸のまま続けると "(" や "-" が式の
			// 演算子として読まれ、式全体が評価されずに本文のまま表示された（実機・round 後の
			// ご指摘）。"×" のように演算子でない文字は裸でも通る。注記は解析側が数字・符号・
			// 括弧・"~"・","・階名だけで書き、引用符を含む階名は番号で呼ぶ（parse/Tag の
			// memberLevelNote）ので、引用符が中に入ることはない。
			if (linkHeight)
				formula += TXString(LinkedHeightFormula(tag.noteDatum).c_str());
			else if (!note.empty())
			{
				formula += "\" ";
				formula += TXString(note.c_str());
				formula += "\"";
			}
			return formula;
		}

		// 「引出線を表示」パラメータ（既定 ON）。部材の面ちょうどに置いても ON のままだと引出
		// 線が描かれるので OFF にする。universal 名で見つからなければ OIP の日本語名で引き直
		// す（draw/DrawUtil の ResolveParamName。名前が 1 つ違うだけで setter は黙って無視され
		// る）。
		constexpr const char* kFieldUseLeader = "Use Leader";
		constexpr const char* kLocalizedUseLeader = "引出線を表示";

		// 引出線を OFF にする（消せたら true）。universal 名で引けない環境（日本語 UI）に
		// 備えて OIP の表示名でも引き直す（ResolveParamName）。
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

		// タグの逃がし量は**タグ自身の高さの半分**（高さ＝文字の向きに直交する差し渡し）。命令の
		// position は**部材の辺の中央**で、そこへタグの下端中央が接するようにしたい。offset は
		// どちらの図でも**部材（＝文字の向き）に直交**するので、逃がす量はタグ自身の高さの
		// 半分になる。タグの実寸はレイアウトの中身が決めるので、置いてから GetObjectBounds で
		// 測る。
		//
		// 外接矩形は軸に平行なので、**傾いたタグ（傾斜材）では外接矩形の高さ≠タグの高さ**。
		// 以前は外接矩形の幅・高さを offset の成分で按分していたが、これだと文字の長さの
		// sin 成分まで逃がし量に入り、軸組図の登り梁のタグが材から大きく離れた。回転角から
		// タグ自身の高さを戻す（core::rotatedRectHeight）。45 度近くで解けないタグは、同じ図の
		// ほかのタグから高さを借りる（MovePendingTags）。
		//
		// 高さを解けず、借りる相手も無いときの逃がし量（外接矩形を offset の成分で按分した
		// 差し渡しの半分）。45 度近くの傾きでは実際より大きく出る（離れる側へ倒れるので
		// 材には重ならない）。
		double FallbackClearance(const core::TagCommand& tag, double width, double height)
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
			double width = 0.0; // 外接矩形の実寸
			double height = 0.0;
		};

		// 置いたタグをまとめて目標へ動かす。
		//
		// **目標の絶対位置へバウンディングボックスの中心を合わせる**だけ。命令の position は
		// すでにそのビューポートの注釈空間で表されている（伏図＝モデルの平面座標そのもの、
		// 軸組図＝切断線の終点からの距離と天端 Z。parse/Tag.h）。
		//
		// **この後処理が最終位置を決める。** VW は指定した挿入点にタグを留めない（伏図は
		// タグ幅の半分だけ −X へ寄り、軸組図はビューポートごとにばらばらの場所へ落ちる。
		// ローカル確認で実測。draw/Tag.h の落とし穴 1）ので、どこへ置かれたかに依らず
		// 実位置との差だけ動かす。
		void MovePendingTags(const std::vector<PendingTag>& pending)
		{
			// 高さを解けたタグのうち最大のもの＝解けないタグ（45 度近く）へ貸す高さ。タグは
			// どれも同じレイアウトの 1 行なので高さは揃う。揃わないとしても、大きい方へ
			// 倒せば材に重ならない。
			std::vector<std::optional<double>> heights;
			heights.reserve(pending.size());
			std::optional<double> lent;
			for (const PendingTag& tag : pending)
			{
				heights.push_back(
					core::rotatedRectHeight(tag.command->angle, tag.width, tag.height));
				if (heights.back().has_value())
					lent = std::max(lent.value_or(0.0), *heights.back());
			}

			for (std::size_t i = 0; i < pending.size(); ++i)
			{
				const PendingTag& tag = pending[i];
				const std::optional<double> height = heights[i].has_value() ? heights[i] : lent;
				const double clearance =
					height.has_value() ? *height / 2.0
									   : FallbackClearance(*tag.command, tag.width, tag.height);
				const double targetX =
					tag.command->position.x + (tag.command->offset.x * clearance);
				const double targetY =
					tag.command->position.y + (tag.command->offset.y * clearance);
				gSDK->MoveObject(tag.object, targetX - tag.centreX, targetY - tag.centreY);
			}
		}

		// フィールドの文字を整える。文書に文字スタイル（"寸法(6pt)"）があればそれを当て、
		// 無ければ大きさだけを直接与える（**その文書でも寸法が読める**ようにする）。
		void ApplyFieldTextStyle(MCObjectHandle text, Sint32 length, TagCounts& counts)
		{
			const MCObjectHandle resource = gSDK->GetNamedObject(TXString(kTextStyleName));
			if (resource != nil)
			{
				gSDK->SetTextStyleRef(text, gSDK->GetObjectInternalIndex(resource));
				return;
			}

			// 文字スタイルが無い文書。大きさだけを与えて先へ進む（診断に残す）。
			counts.textStyleMissing = true;
			gSDK->SetTextSize(text, 0, length, kTextSizePoints);
		}

		// レイアウトへ置く断面寸法フィールドを 1 つ作って container へ入れる。フィールドの
		// 実体は**式を持たせたテキスト**（リンクされたテキスト）。
		bool CreateTagField(MCObjectHandle container, const TXString& formula, TagCounts& counts)
		{
			// 式そのものを本文にしておく（タグが評価するまでの見た目であり、評価後は
			// 断面寸法に置き換わる）。fixedSize=false で幅は中身なり。
			const MCObjectHandle text = gSDK->CreateTextBlock(formula, WorldPt(0.0, 0.0), false, 0);
			if (text == nil)
				return false;

			if (!gSDK->AddObjectToContainer(text, container))
			{
				gSDK->DeleteObject(text, true);
				return false;
			}

			ApplyFieldTextStyle(text, static_cast<Sint32>(formula.GetLength()), counts);

			// **文字スタイルを当てた後に**クラスと by-class を与える（描画属性はクラスの
			// ものが最終的に効く）。文字スタイルは書体・大きさを、クラスは色・線の太さを
			// 受け持つ。
			SetClassWithAttributes(text, kTagClass);

			// **フィールドラベルはテキストの名前ではない。** 実機の構造ダンプで、手で作った
			// （寸法が出ている）タグのレイアウトのテキストには**名前が付いていない**ことを
			// 確かめた（draw/Tag.h「実機から持ち帰った見本」）。名前を付けようとすると文書の
			// 資源名とぶつかるだけなので、何もしない。

			// **ここでテキストがタグフィールドになる。** リンクを立てて式を持たせる。
			const VectorWorks::Extension::IDataTagTextLinkSupportPtr link(
				VectorWorks::Extension::IID_DataTagTextLinkSupport);
			if (!link)
			{
				counts.linkMissing = true;
				return true;
			}
			link->SetIsLinked(text, true);
			link->SetFormula(text, formula);
			return true;
		}

		// container の中身の数。レイアウトが本当に載ったかを数で確かめる（ローカル確認で
		// 「レイアウトが空」と分かったときに、どこで落ちたかを診断へ出すため）。
		std::size_t ContainerCount(MCObjectHandle container)
		{
			std::size_t count = 0;
			for (MCObjectHandle h = gSDK->FirstMemberObj(container); h != nil;
				 h = gSDK->NextObject(h))
				++count;
			return count;
		}

		// 既定のタグレイアウトに入っているロクス（kLocusNode）を取り除く。
		//
		// **なぜ消すか**: 生成したばかりのデータタグはレイアウトにロクスを 1 つ持っている。
		// ユーザーが手で作った（実際に寸法が出ている）タグのレイアウトを実機でダンプすると
		// **テキスト 1 つだけ**でロクスは無い。中身の並びを見本へ合わせる（ローカル確認の
		// 構造ダンプで判明。draw/Tag.h「実機から持ち帰った見本」）。
		void RemoveDefaultLoci(MCObjectHandle layout)
		{
			// 走査しながら消すとリンクが切れるので、先に集めてから消す。
			std::vector<MCObjectHandle> loci;
			for (MCObjectHandle h = gSDK->FirstMemberObj(layout); h != nil; h = gSDK->NextObject(h))
				if (gSDK->GetObjectTypeN(h) == kLocusNode)
					loci.push_back(h);
			for (MCObjectHandle h : loci)
				gSDK->DeleteObject(h, true);
		}

		// タグレイアウト（＝タグの中身を描くグループ）を**そのタグ自身**へ持たせる。組めたら
		// そのレイアウトを、組めなければ nil を返す（呼び出し側は寸法が空のタグとして数える）。
		//
		// **中身を入れてから渡す。** 以前は空のグループを先に SetCustomObjectProfileGroup で
		// 渡し、返ってきたハンドルへテキストを足していたが、それだと**渡した時点で VW が
		// グループを複製して持った場合に、足したテキストが迷子のグループへ入る**——実機では
		// これが「オブジェクトは出るのにタグレイアウトが空」という形で現れた（ローカル確認）。
		// 順序を逆にすれば、複製されても中身ごと複製される。
		//
		// 渡した後は**実際にタグが持っているレイアウトを取り直して**数を確かめ、複製された
		// ときはこちらのグループを消す（図面に空のグループを残さない）。取り直したものが
		// 空だったときだけ、そちらへフィールドを作り直す。
		MCObjectHandle ResolveTagLayout(MCObjectHandle pio, const TXString& formula,
										TagCounts& counts)
		{
			// 既に持っていればそれを使う（生成したばかりのデータタグは既定のレイアウトを
			// 持っているので、通常はこちら）。
			MCObjectHandle held = HeldProfileGroup(pio);
			if (held != nil)
			{
				RemoveDefaultLoci(held);
				if (!CreateTagField(held, formula, counts))
					return nil;
				return ContainerCount(held) == 0 ? nil : held;
			}

			MCObjectHandle group = gSDK->CreateGroup();
			if (group == nil)
				return nil;
			if (!CreateTagField(group, formula, counts))
			{
				gSDK->DeleteObject(group, true);
				return nil;
			}

			if (!gSDK->SetCustomObjectProfileGroup(pio, group))
			{
				gSDK->DeleteObject(group, true);
				return nil;
			}

			held = HeldProfileGroup(pio);
			if (held == nil)
			{
				// タグが持ってくれなかった（＝データタグのレイアウトはプロファイルグループ
				// ではない）。こちらのグループは図面上の residue なので消す。
				gSDK->DeleteObject(group, true);
				return nil;
			}

			if (held != group)
			{
				// VW が複製して持った。中身まで複製されていなければフィールドを作り直し、
				// こちらのグループは消す。
				const bool filled =
					ContainerCount(held) != 0 || CreateTagField(held, formula, counts);
				gSDK->DeleteObject(group, true);
				if (!filled)
					return nil;
			}

			return ContainerCount(held) == 0 ? nil : held;
		}

		// 高さの注記を部材の高さに連動させられるか。解析側が基準名を付けた（水平な）材で、
		// 関連付け先の横架材があり、**その横架材が居るレイヤが階に属している**こと。
		// **階に属さないレイヤでは IPZS は黙って絶対Z を返す**（エラーにも空にもならず、
		// 注記が違う数を出し続ける。Findings「Data Tags」の落とし穴）ので、確かめられない
		// ときは解析側の文字へ倒し、件数を診断へ回す。
		bool LinksHeight(const core::TagCommand& tag, MCObjectHandle member, TagCounts& counts)
		{
			if (tag.noteDatum.empty() || member == nil)
				return false;
			const MCObjectHandle layer = VWObject(member).GetParentLayer();
			if (layer != nil && gSDK->GetStoryOfLayer(layer) != nil)
				return true;
			++counts.heightUnlinked;
			return false;
		}

		// タグ 1 つを注釈として置く。置けたら true。support は呼び出し側が 1 回だけ作った
		// VCOM のデータタグ支援インターフェース（タグごとに QueryInterface しない）。
		// 置けたタグのハンドルは outPlaced へ積む（クラスを表示へ戻すのに使う）。
		bool PlaceOne(MCObjectHandle viewport, const core::TagCommand& tag, MCObjectHandle member,
					  const VectorWorks::Extension::IDataTagSupportPtr& support, TagCounts& counts,
					  std::vector<MCObjectHandle>& outPlaced, std::vector<PendingTag>& outPending)
		{
			// 第 4 引数 bInsert=true でカレントレイヤへ入る。この後 AddViewportAnnotationObject で
			// 注釈へ移すので、レイヤ上に残るのは失敗したときだけ（下記で消す）。
			MCObjectHandle object = nil;
			{
				VW_DRAW_TIME("タグ:生成");
				object = gSDK->CreateCustomObject(TXString(kDataTagPlugin),
												  WorldPt(tag.position.x, tag.position.y),
												  tag.angle, true);
			}
			if (object == nil)
			{
				++counts.failed;
				return false;
			}

			// **タグオブジェクト自体も寸法クラス**にし、描画属性はクラス属性に従わせる
			// （中のテキストは CreateTagField が同じクラスに置く）。注釈へ移した後にビュー
			// ポートのクラス表示を戻す後処理（ShowAllViewportClasses）が下にあるので、ここで
			// 新しいクラスを持ち込んでもタグが映らなくなることはない。
			// **ここを区間で包まない。** 中の SetClassByName / SetAllAttributesByClass が
			// 自分で呼び出しごとに区間を開く（draw/DrawUtil の【計測】）ので、包むと
			// 入れ子になる（core/DrawTiming.h「使う側の作法」）。
			SetClassWithAttributes(object, kTagClass);

			// **関連付けを先に行う**（中身を組むより前）。タグの本文は関連付け先のレコードから
			// 取るので、相手を決めてからレイアウトを組み、最後に UpdateDataTag で流し込む。
			// フォールバックの直線になった横架材はハンドルが無いので関連付けを省く（タグ自体
			// は置く）。
			if (member != nil && support)
			{
				VW_DRAW_TIME("タグ:関連付け");
				support->AssociateWithObject(object, member);
			}
			else
				++counts.unassociated;

			// **タグの中身（タグレイアウト）はこのタグへ直接組む**——スタイルは作らないし
			// 当てない（draw/Tag.h の ★。スラブ・壁が構成層を各オブジェクトへ直接与えるのと
			// 同じ）。組めなくても**タグは置く**——タグを失うより、位置だけでも正しいタグを
			// 残した方が原因を追いやすい（寸法が空になるので件数を数えて診断へ回す）。
			// **レイアウトはタグ 1 本ごとに組み直している**（スタイルを作らない方針の裏返し。
			// 文字スタイル資源の引き当て・式の組み立て・リンク支援の取得が 1 本ごとに走る）。
			// 400〜530 本ぶんが伏図・軸組図の時間に溶け込んでいる。
			// **それでもここは区間にしない。** 中の CreateTagField がタグ内のテキストへ
			// SetClassByName / SetAllAttributesByClass を呼ぶので、包むと入れ子になる
			// （draw/DrawUtil の【計測】）。round 1 の実測は 531 回で 221ms と軽い。
			const TXString formula = TagFieldFormula(tag, LinksHeight(tag, member, counts));
			if (ResolveTagLayout(object, formula, counts) == nil)
			{
				++counts.layoutFailed;
			}
			else if (support)
			{
				// **レイアウトへ入れたテキストをタグフィールドとして認識させる。** これを
				// しないとタグが式を拾わず、寸法が空のまま出る（スタイルを作っていた頃に
				// スタイルに対して行っていたのと同じ呼び出し。SDK のコメントにも
				// 「データタグ**または**データタグスタイルの」とある）。
				support->UpdateUserDefinedTextsUIDs(object);
			}

			// 引出線を OFF にする（**タグを部材の面ちょうどに置く**ので、既定 ON のままだと
			// 引出線が描かれる。draw/Tag.h）。**パラメータ名の解決がここで 1 本ごとに走る**
			// （TurnOffLeader → ResolveParamName）ので、区間として分けておく。
			{
				VW_DRAW_TIME("タグ:引出線");
				if (!TurnOffLeader(object))
					++counts.leaderLeft;
			}

			{
				VW_DRAW_TIME("タグ:リセット");
				gSDK->ResetObject(object);
			}

			// ビューポートの注釈へ移す。入らなければタグを消す（冒頭「注釈に入らなかった
			// タグは消す」）。
			bool annotated = false;
			{
				VW_DRAW_TIME("タグ:注釈へ移す");
				annotated = gSDK->AddViewportAnnotationObject(viewport, object);
			}
			if (!annotated)
			{
				gSDK->DeleteObject(object, true);
				++counts.failed;
				return false;
			}

			// 関連付け後の再計算。これをしないと、関連付けた横架材の断面寸法が本文へ流し込ま
			// れない。
			if (support)
			{
				VW_DRAW_TIME("タグ:本文の流し込み");
				support->UpdateDataTag(object);
			}

			// **ここで実位置と実寸を測る**。ここまででタグが本文を流し込み、実寸が
			// 確定している。動かすのは全部置いてから（診断へ出す実測を先頭から数件そろえる
			// ため。MovePendingTags）。
			WorldRect bounds;
			bool measured = false;
			{
				VW_DRAW_TIME("タグ:実測");
				measured = gSDK->GetObjectBounds(object, bounds);
			}
			if (!measured)
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
			pending.width = std::abs(bounds.right - bounds.left);
			pending.height = std::abs(bounds.top - bounds.bottom);
			outPending.push_back(pending);

			outPlaced.push_back(object);
			++counts.drawn;
			return true;
		}

	} // namespace

	void prepareDataTagPlugin()
	{
		PrepareCustomObjectDefinition(kDataTagPlugin);
	}

	std::size_t drawViewportTags(MCObjectHandle viewport, const core::ViewportCommand& command,
								 const ObjectHandleTable& memberHandles, TagCounts& counts)
	{
		if (viewport == nil || command.tags.empty())
			return 0;

		// VCOM のデータタグ支援インターフェース（関連付け・フィールドの認識・更新）。
		// ビューポート 1 枚につき 1 回だけ取る。取れなければ**タグは置くが関連付けと
		// フィールドの認識は省く**（位置だけでも正しいタグが残る方が原因を追いやすい）。
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

			if (PlaceOne(viewport, tag, member, support, counts, placed, pending))
				++drawn;
		}

		MovePendingTags(pending);

		// **置いたタグ 1 本の実際の姿を控える**（診断行へ出す。1 枚目のビューポートの 1 本目
		// だけ）。「タグはあるのに中身が空」のとき、原因がタグレイアウトを持てていないこと
		// なのかどうかを実機から持ち帰るための目。**正常なら黙る**——中身が載っているなら
		// この行は読み手にとって雑音でしかない（タグを見れば分かる）。
		if (counts.firstTag.empty() && !placed.empty())
		{
			const MCObjectHandle layout = HeldProfileGroup(placed.front());
			const std::size_t items = layout == nil ? 0 : ContainerCount(layout);
			if (items == 0)
				counts.firstTag = layout == nil ? std::string("レイアウト無し")
												: "レイアウト" + std::to_string(items) + "件";
		}

		// クラスを表示へ戻し、ビューポートを更新して反映する。ConfigureViewport は**タグを
		// 置く前**に走っているので、**タグの中身がその時点で文書に無かったクラスを持ち込んだ
		// 場合**、ここで戻さないと注釈だけが空白のまま残る。
		// 戻すのはビューポートと同じく**全クラス**（draw/DrawUtil の ShowAllViewportClasses）
		// ——タグが身に付けているクラスを数え上げる必要はない。命令が隠すクラスは
		// ConfigureViewport と同じく非表示のまま保つ。
		if (!placed.empty())
		{
			counts.classesShown += ShowAllViewportClasses(viewport, command.hiddenClasses);
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
		// 異常が無ければ 1 行も出さない（うまくいった取り込みでは雑音でしかない）。
		if (counts.failed == 0 && counts.unassociated == 0 && counts.layoutFailed == 0 &&
			counts.leaderLeft == 0 && counts.updateFailed == 0 && counts.unmeasured == 0 &&
			counts.heightUnlinked == 0 && !classesBroken && !counts.textStyleMissing &&
			!counts.linkMissing && counts.firstTag.empty())
			return {};

		std::string text = label + "の断面寸法タグの診断: ";
		if (!counts.firstTag.empty())
			text += "置いたタグの実際: " + counts.firstTag + "。";
		AppendCount(text, "タグレイアウトを組めなかったタグ", counts.layoutFailed, "件",
					"断面寸法が空になります");
		if (counts.textStyleMissing)
			text += std::string("文字スタイル「") + kTextStyleName +
					"」が文書に無いので大きさだけを与えました。";
		if (counts.linkMissing)
			text += "タグフィールドの式を入れられませんでした（寸法が空になります）。";
		AppendCount(text, "タグを置けなかった命令", counts.failed, "件");
		AppendCount(text, "引出線を消せなかったタグ", counts.leaderLeft, "件");
		AppendCount(text, "関連付け先の横架材が無いタグ", counts.unassociated, "件",
					"断面寸法が空になります");
		if (classesBroken)
			text += "タグのクラスを表示に戻せませんでした（タグが図に出ません）。";
		AppendCount(text, "クラスを戻した後に更新できなかったビューポート", counts.updateFailed,
					"枚");
		AppendCount(text, "実位置を測れず動かせなかったタグ", counts.unmeasured, "件");
		AppendCount(text, "階に属さないレイヤの横架材で、高さの注記を連動させられなかったタグ",
					counts.heightUnlinked, "件");
		return text;
	}
} // namespace HomeskzIfcImport::draw
