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
//	      AssociateWithObject … 対象の横架材へ関連付け
//	      UpdateDataTag       … 関連付け後の再計算
//
//	スタイルを作るとき（createTagStyle）に使う SDK API:
//	  * gSDK->GetNamedObject                        … 名前が空いているかを見る
//	  * gSDK->CreateSymbolDefinition(inoutName)     … スタイルの実体＝シンボル定義
//	  * gSDK->SetSymbolDefSubType(symDef, 内部 ID)  … そのシンボル定義を**スタイルにする**
//	  * gSDK->AddObjectToContainer                  … PIO・テキストを容れ物へ入れる
//	  * gSDK->GetCustomObjectProfileGroup / SetCustomObjectProfileGroup … タグレイアウト
//	  * gSDK->CreateGroup / CreateTextBlock / SetTextStyleRef … レイアウトの中身
//	  * gSDK->GetCustomObjectProfileGroupInAux                          … レイアウトのもう 1 つの入り口
//	  * gSDK->FirstMemberObj / NextObject / GetObjectTypeN / GetObjectName … 構造ダンプ（診断）
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
		bool layoutCopied = false; // 渡したグループを VW が複製して持った
		bool styleMapSet = false; // スタイルのパラメータ対応表を「スタイル依存」にできた
		bool notAStyle = false; // 作った資源を VW がプラグインスタイルとみなさない
		std::size_t lociRemoved = 0; // 既定レイアウトから取り除いたロクスの数
		bool layoutRejected = false; // 渡してもレイアウトとして持ってくれなかった
		std::size_t layoutPrefilled = 0; // PIO が最初から持っていたレイアウトの中身の数
		std::size_t layoutCount = 0; // 最終的にレイアウトへ載った中身の数（0 なら空のまま）
		std::string structure; // 作ったスタイルの構造（実機から持ち帰る目）
		std::string existingStructure; // 同名の既存スタイルの構造（参照見本）
		bool textStyleMissing = false; // 文字スタイル（"寸法(6pt)"）が文書に無かった
		bool linkMissing = false; // テキストをタグフィールドにできなかった（式が入らない）
		bool leaderLeft = false; // スタイル側の引出線を OFF にできなかった
	};

	namespace
	{
		// データタグの内部プラグイン名。VW 標準のデータタグツールの universal 名で、
		// 表示名（"データタグ"）とは別物。
		constexpr const char* kDataTagPlugin = "Data Tag";

		// --- 生成するスタイルの中身（draw/Tag.h「スタイルは作って使う」）-----------------

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

		// 「引出線を表示」パラメータ（既定 ON）。部材の面ちょうどに置いても ON のままだと引出
		// 線が描かれるので OFF にする。universal 名で見つからなければ OIP の日本語名で引き直
		// す（draw/DrawUtil の ResolveParamName。名前が 1 つ違うだけで setter は黙って無視され
		// る）。
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

			// **関連付けを先に行う**（スタイルより前）。関連付け先の無いタグにスタイルを当て
			// ると、VW が「互換性のないデータタグスタイルを選択しています」の警告ダイアログを
			// 出してインポートが止まる（ローカル確認で判明。タグの数だけ出る）。
			// フォールバックの直線になった横架材はハンドルが無いので関連付けを省く（タグ自体
			// は置く）。
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

			// 関連付け後の再計算。これをしないと、関連付けた横架材の断面寸法が本文へ流し込ま
			// れない。
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
			// const にしない: 返すときの自動 move が効かなくなる（clang-tidy
			// performance-no-automatic-move）。
			TXString wanted(base.c_str());
			if (gSDK->GetNamedObject(wanted) == nil)
				return wanted;

			outRenamed = true;
			for (int suffix = 2; suffix <= kNameSuffixLimit; ++suffix)
			{
				TXString candidate((base + "-" + std::to_string(suffix)).c_str());
				if (gSDK->GetNamedObject(candidate) == nil)
					return candidate;
			}
			return {};
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

		// レイアウトへ置く断面寸法フィールドを 1 つ作って container へ入れる。フィールドの
		// 実体は**式を持たせたテキスト**（リンクされたテキスト）で、ラベルはテキストの名前。
		bool CreateTagField(MCObjectHandle container, TagStyleRecord& record)
		{
			const TXString formula = TagFieldFormula();

			// 式そのものを本文にしておく（スタイルが評価するまでの見た目であり、評価後は
			// 断面寸法に置き換わる）。fixedSize=false で幅は中身なり。
			const MCObjectHandle text = gSDK->CreateTextBlock(formula, WorldPt(0.0, 0.0), false, 0);
			if (text == nil)
				return false;

			if (!gSDK->AddObjectToContainer(text, container))
			{
				gSDK->DeleteObject(text, true);
				return false;
			}

			ApplyFieldTextStyle(text, static_cast<Sint32>(formula.GetLength()), record);

			// **フィールドラベルはテキストの名前ではない。** 実機の構造ダンプで、手で作った
			// （寸法が出ている）スタイルのレイアウトのテキストには**名前が付いていない**ことを
			// 確かめた（draw/Tag.h「実機から持ち帰った見本」）。名前を付けようとすると文書の
			// 資源名とぶつかるだけなので、何もしない。

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

		// 既定のタグレイアウトに入っているロクス（kLocusNode）を取り除いて、取り除いた数を返す。
		//
		// **なぜ消すか**: 生成したばかりのデータタグはレイアウトにロクスを 1 つ持っている。
		// ユーザーが手で作った（実際に寸法が出ている）スタイルのレイアウトを実機でダンプすると
		// **テキスト 1 つだけ**でロクスは無い。中身の並びを見本へ合わせる（ローカル確認の
		// 構造ダンプで判明。draw/Tag.h「実機から持ち帰った見本」）。
		std::size_t RemoveDefaultLoci(MCObjectHandle layout)
		{
			// 走査しながら消すとリンクが切れるので、先に集めてから消す。
			std::vector<MCObjectHandle> loci;
			for (MCObjectHandle h = gSDK->FirstMemberObj(layout); h != nil; h = gSDK->NextObject(h))
				if (gSDK->GetObjectTypeN(h) == kLocusNode)
					loci.push_back(h);
			for (MCObjectHandle h : loci)
				gSDK->DeleteObject(h, true);
			return loci.size();
		}

		// PIO がいま持っているタグレイアウト。**2 つの入り口を両方見る**——VW2020 で
		// 「プロファイルグループは aux コンテナに持つ」経路が足されており
		// （ISDK::GetCustomObjectProfileGroupInAux）、スタイルの中の PIO のように図面上に
		// 無いオブジェクトではそちらにしか出ないことがある。
		MCObjectHandle HeldTagLayout(MCObjectHandle pio)
		{
			const MCObjectHandle direct = gSDK->GetCustomObjectProfileGroup(pio);
			if (direct != nil)
				return direct;
			return gSDK->GetCustomObjectProfileGroupInAux(pio);
		}

		// タグレイアウト（＝タグの中身を描くグループ）を PIO へ持たせる。
		//
		// **中身を入れてから渡す。** 以前は空のグループを先に SetCustomObjectProfileGroup で
		// 渡し、返ってきたハンドルへテキストを足していたが、それだと**渡した時点で VW が
		// グループを複製して持った場合に、足したテキストが迷子のグループへ入る**——実機では
		// これが「オブジェクトは出るのにタグレイアウトが空」という形で現れた（ローカル確認）。
		// 順序を逆にすれば、複製されても中身ごと複製される。
		//
		// 渡した後は**実際に PIO が持っているレイアウトを取り直して**数を確かめ、複製された
		// ときはこちらのグループを消す（図面に空のグループを残さない）。取り直したものが
		// 空だったときだけ、そちらへフィールドを作り直す。
		MCObjectHandle ResolveTagLayout(MCObjectHandle pio, TagStyleRecord& record)
		{
			// 既に持っていればそれを使う（データタグ PIO が生成時にレイアウトを持つ実装で
			// あれば、こちらが作る必要はない）。
			MCObjectHandle held = HeldTagLayout(pio);
			if (held != nil)
			{
				record.layoutPrefilled = ContainerCount(held);
				record.lociRemoved = RemoveDefaultLoci(held);
				if (!CreateTagField(held, record))
					return nil;
				record.layoutCount = ContainerCount(held);
				return held;
			}

			MCObjectHandle group = gSDK->CreateGroup();
			if (group == nil)
				return nil;
			record.layoutCreated = true;
			if (!CreateTagField(group, record))
			{
				gSDK->DeleteObject(group, true);
				return nil;
			}

			if (!gSDK->SetCustomObjectProfileGroup(pio, group))
			{
				gSDK->DeleteObject(group, true);
				return nil;
			}

			held = HeldTagLayout(pio);
			if (held == nil)
			{
				// PIO が持ってくれなかった（＝データタグのレイアウトはプロファイルグループ
				// ではない）。こちらのグループは図面上の residue なので消し、スタイルは
				// 中身無しとして扱う（呼び出し側がスタイルごと捨てる）。
				record.layoutRejected = true;
				gSDK->DeleteObject(group, true);
				return nil;
			}

			if (held != group)
			{
				// VW が複製して持った。中身まで複製されていなければフィールドを作り直し、
				// こちらのグループは消す。
				record.layoutCopied = true;
				if (ContainerCount(held) == 0 && !CreateTagField(held, record))
					return nil;
				gSDK->DeleteObject(group, true);
			}

			record.layoutCount = ContainerCount(held);
			return held;
		}

		// オブジェクト 1 つの見出し（型番号と名前）。診断へ出す構造ダンプの部品。
		std::string DescribeObject(MCObjectHandle object)
		{
			std::string text = "型" + std::to_string(gSDK->GetObjectTypeN(object));
			TXString name;
			gSDK->GetObjectName(object, name);
			if (!name.IsEmpty())
				text += "「" + name.GetStdString() + "」";

			// テキストなら**タグフィールドになっているか**が知りたい（式が入っていなければ
			// タグは空で出る）。
			const VectorWorks::Extension::IDataTagTextLinkSupportPtr link(
				VectorWorks::Extension::IID_DataTagTextLinkSupport);
			if (link && link->IsSupported(object))
				text += link->GetIsLinked(object) ? "[式あり]" : "[式なし]";
			return text;
		}

		// データタグスタイル（シンボル定義）の中身を 1 行に畳む。**実描画はローカルの VW で
		// しか見られない**ので、「レイアウトがどこに入っているか」を実機から持ち帰るための
		// 目。ユーザーが手で作った既存のスタイルへ当てれば、VW が本当はどこへレイアウトを
		// 置くのかがそのまま読める（こちらの作り方と突き合わせる）。
		std::string DescribeStyle(MCObjectHandle symDef)
		{
			std::string text;
			for (MCObjectHandle h = gSDK->FirstMemberObj(symDef); h != nil; h = gSDK->NextObject(h))
			{
				if (!text.empty())
					text += ", ";
				text += DescribeObject(h);

				// PIO ならレイアウトの入り口を両方見て、中身の数まで出す。
				const MCObjectHandle direct = gSDK->GetCustomObjectProfileGroup(h);
				const MCObjectHandle aux = gSDK->GetCustomObjectProfileGroupInAux(h);
				if (direct != nil)
					text += "{プロファイル " + std::to_string(ContainerCount(direct)) + "件}";
				if (aux != nil && aux != direct)
					text += "{aux " + std::to_string(ContainerCount(aux)) + "件}";
				const MCObjectHandle layout = direct != nil ? direct : aux;
				if (layout != nil)
					for (MCObjectHandle in = gSDK->FirstMemberObj(layout); in != nil;
						 in = gSDK->NextObject(in))
						text += "(" + DescribeObject(in) + ")";
			}
			return text.empty() ? std::string("空") : text;
		}

		// 文書に既にある同名のデータタグスタイル（＝ユーザーが手で作ったもの）の構造。
		// **参照見本**として診断へ出す。無ければ空文字。
		std::string DescribeExistingStyle(const std::string& baseName)
		{
			const MCObjectHandle existing = gSDK->GetNamedObject(TXString(baseName.c_str()));
			if (existing == nil || gSDK->GetSymbolDefSubType(existing) != kInternalID_DataTag)
				return {};
			return DescribeStyle(existing);
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

		// **参照見本を先に控える**（スタイルを作る前）。基準名がそのまま空いていれば見本は
		// 無い＝空文字。**ここが実機から持ち帰る唯一の目**で、ユーザーが手で作った既存の
		// 「断面寸法」スタイルの構造がそのまま診断行に出る（VW が本当はどこへタグレイアウトを
		// 置くのか、こちらの作り方と突き合わせられる）。
		record.existingStructure = DescribeExistingStyle(record.requested);

		// タグレイアウト（断面寸法フィールド 1 つ）。**ここが空だとタグは何も表示しない**ので、
		// 作れなければスタイルごと捨ててスタイル無しへ落とす（中身の無いスタイルを文書へ
		// 残さない）。中身は ResolveTagLayout が入れる（**空のグループを先に渡さない**。
		// 同関数の頭の説明）。
		const MCObjectHandle layout = ResolveTagLayout(pio, record);
		if (layout == nil || record.layoutCount == 0)
		{
			gSDK->DeleteSymbolDefinition(symDef, true);
			record.name.clear();
			record.failure = record.layoutRejected
								 ? "データタグがタグレイアウトを受け取りませんでした"
								 : "タグレイアウトを作れませんでした";
			return;
		}

		gSDK->ResetObject(pio);

		// スタイルにフィールドを認識させる（これをしないとタグ側が式を拾わない）。
		const VectorWorks::Extension::IDataTagSupportPtr support(
			VectorWorks::Extension::IID_DataTagSupport);
		if (support)
			support->UpdateUserDefinedTextsUIDs(symDef);

		// **スタイルのパラメータ対応表を「スタイル依存」にする。** これを作らないと、
		// シンボル定義は形だけのスタイルで、当てたインスタンスは自分の値（＝空の既定
		// レイアウト）を使い続ける——実機で「オブジェクトは出るのにタグの中身が空」に
		// なったのがこれ（ローカル確認）。手で作ったスタイルでは VW の
		// 「スタイルを作成」がこの表を用意している。
		gSDK->SetAllPluginStyleParameters(symDef, kPluginStyleParameter_ByStyle);
		record.styleMapSet = true;
		record.notAStyle = !gSDK->IsPluginStyle(symDef);

		// 作ったスタイルの構造を控える（診断行へ出す。上記「参照見本」と並べて読む）。
		record.structure = DescribeStyle(symDef);

		record.style = static_cast<RefNumber>(gSDK->GetObjectInternalIndex(symDef));
		if (record.style == 0)
			record.failure = "作ったスタイルを参照できませんでした";

		// **取り消しでスタイルも消えるようにする**（draw/DrawUtil の RecordCreatedObject）。
		// スタイルはレイヤの上の図形ではなく**資源**なので、レイヤを消しても残る——申告して
		// おかないと、取り込みと取り消しを繰り返すたびに `断面寸法`、`断面寸法-2` … が
		// 文書に積み上がる。**最後に（＝作り切ってから）申告する**——途中で捨てる経路では
		// こちらが削除するので、先に申告すると undo 表に消えたものが残ってしまう。
		// 中のテキスト・グループは申告しない（スタイルごと消えるものを二重に登録しない）。
		RecordCreatedObject(symDef);
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
		// **構造は異常のときだけ出す。** 中身の並びのダンプは「タグが空で出る」を実機から
		// 追うために足したもので（draw/Tag.h）、うまくいっている取り込みでは読み手に
		// とって雑音でしかない。作り方に引っかかりがあったとき——スタイルとして認識され
		// ない・レイアウトが空・VW が複製した——だけ、見本と並べて出す。
		const bool suspicious = record.notAStyle || record.layoutCount == 0 || record.layoutCopied;
		if (suspicious)
		{
			if (record.notAStyle)
				text += "VW がこの資源をプラグインスタイルとみなしていません。";
			if (record.layoutCopied)
				text += "渡したレイアウトは VW 側で複製されました。";
			if (record.layoutCreated)
				text += "タグレイアウトは新しく作りました。";
			if (!record.styleMapSet)
				text += "スタイルのパラメータ対応表を作れませんでした。";
			text += "データタグが最初から持っていた中身 " + std::to_string(record.layoutPrefilled) +
					" 件（うちロクス " + std::to_string(record.lociRemoved) + " 個を外した）。";
			text += "レイアウトの中身 " + std::to_string(record.layoutCount) + " 件。";
			text += "構造: " + record.structure + "。";
			if (!record.existingStructure.empty())
				text += "同名の既存スタイルの構造: " + record.existingStructure + "。";
		}
		if (record.textStyleMissing)
			text += std::string("文字スタイル「") + kTextStyleName +
					"」が文書に無いので大きさだけを与えました。";
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

		// **置いたタグ 1 本の実際の姿を控える**（診断行へ出す。1 枚目のビューポートの 1 本目
		// だけ）。「スタイルは作れているのにタグの中身が空」のとき、原因が
		//   * インスタンスにスタイルが当たっていない（スタイル名が出ない）
		//   * 当たってはいるがレイアウトが空（中身 0 件）
		// のどちらなのかを実機から持ち帰るための目。
		if (counts.firstTag.empty() && !placed.empty())
		{
			const MCObjectHandle tag = placed.front();
			MCObjectHandle styleSymbol = nil;
			const bool styled = gSDK->GetPluginStyleSymbol(tag, styleSymbol) && styleSymbol != nil;
			const MCObjectHandle layout = HeldTagLayout(tag);
			const std::size_t items = layout == nil ? 0 : ContainerCount(layout);

			// **正常なら黙る。** スタイルが当たっていてレイアウトに中身があるなら、この行は
			// 読み手にとって雑音でしかない（タグを見れば分かる）。当たっていない・中身が
			// 無い——つまり「タグはあるのに寸法が出ない」ときだけ、どちらなのかを実機から
			// 持ち帰る。
			if (!styled || items == 0)
			{
				std::string text;
				if (styled)
				{
					TXString name;
					gSDK->GetObjectName(styleSymbol, name);
					text = "スタイル「" + name.GetStdString() + "」";
				}
				else
				{
					text = "スタイル無し";
				}
				text += "・レイアウト" +
						(layout == nil ? std::string("無し") : std::to_string(items) + "件");
				counts.firstTag = std::move(text);
			}
		}

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
		// **タグを置けたときは実際の姿を必ず出す**（中身が空で出る問題の切り分けが
		// 実機の 1 回で済むように）。異常が無ければ他の行は付かない。
		if (counts.failed == 0 && counts.unassociated == 0 && counts.leaderLeft == 0 &&
			counts.updateFailed == 0 && counts.unmeasured == 0 && !classesBroken &&
			!counts.styleMissing && counts.firstTag.empty())
			return {};

		std::string text = label + "の断面寸法タグの診断: ";
		if (!counts.firstTag.empty())
			text += "置いたタグの実際: " + counts.firstTag + "。";
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
