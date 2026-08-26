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

#include "Interfaces/VectorWorks/Extension/IDataTagSupport.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <cmath>
#include <cstddef>
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

		// 断面寸法タグのクラス。**タグ本体（PIO）とタグレイアウトの中のテキストの両方**を
		// このクラスに置き、描画属性（ペン・塗り・線の太さ・不透明度…）はすべてクラス属性に
		// 従わせる（SetAllAttributesByClass）。他の要素が「その部材が何か」でクラス分けする
		// のと同じ流儀で、寸法の見え方（色・線の太さ）を図面側のクラスで一括して決められる
		// ようにするため——タグ 1 本ずつへ属性を焼き込むと、後から図面で色を変えられない。
		// 存在しないクラスは SetClassByName（AddClass）が作る。
		//
		// **タグの中のテキストにも要る**——タグレイアウトの中身はタグ本体のクラスを継ぐわけ
		// ではないので、本体だけに与えても文字は既定クラスのままになる。
		constexpr const char* kTagClass = "寸法";

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

		// タグの逃がし量（offset の向きに沿ったタグの差し渡しの半分）。命令の position は
		// **部材の辺の中央**で、そこへタグの下端中央が接するようにしたい。タグの実寸は
		// レイアウトの中身が決めるので、置いてから GetObjectBounds で測る。offset は軸に平行
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
		bool CreateTagField(MCObjectHandle container, TagCounts& counts)
		{
			const TXString formula = TagFieldFormula();

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
			SetClassByName(text, kTagClass);
			SetAllAttributesByClass(text);

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

		// タグがいま持っているタグレイアウト。**2 つの入り口を両方見る**——VW2020 で
		// 「プロファイルグループは aux コンテナに持つ」経路が足されており
		// （ISDK::GetCustomObjectProfileGroupInAux）、どちらに出るかはオブジェクトによって
		// 変わる。
		MCObjectHandle HeldTagLayout(MCObjectHandle pio)
		{
			const MCObjectHandle direct = gSDK->GetCustomObjectProfileGroup(pio);
			if (direct != nil)
				return direct;
			return gSDK->GetCustomObjectProfileGroupInAux(pio);
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
		MCObjectHandle ResolveTagLayout(MCObjectHandle pio, TagCounts& counts)
		{
			// 既に持っていればそれを使う（生成したばかりのデータタグは既定のレイアウトを
			// 持っているので、通常はこちら）。
			MCObjectHandle held = HeldTagLayout(pio);
			if (held != nil)
			{
				RemoveDefaultLoci(held);
				if (!CreateTagField(held, counts))
					return nil;
				return ContainerCount(held) == 0 ? nil : held;
			}

			MCObjectHandle group = gSDK->CreateGroup();
			if (group == nil)
				return nil;
			if (!CreateTagField(group, counts))
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
				// タグが持ってくれなかった（＝データタグのレイアウトはプロファイルグループ
				// ではない）。こちらのグループは図面上の residue なので消す。
				gSDK->DeleteObject(group, true);
				return nil;
			}

			if (held != group)
			{
				// VW が複製して持った。中身まで複製されていなければフィールドを作り直し、
				// こちらのグループは消す。
				const bool filled = ContainerCount(held) != 0 || CreateTagField(held, counts);
				gSDK->DeleteObject(group, true);
				if (!filled)
					return nil;
			}

			return ContainerCount(held) == 0 ? nil : held;
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
			const MCObjectHandle object = gSDK->CreateCustomObject(
				TXString(kDataTagPlugin), WorldPt(tag.position.x, tag.position.y), tag.angle, true);
			if (object == nil)
			{
				++counts.failed;
				return false;
			}

			// **タグオブジェクト自体も寸法クラス**にし、描画属性はクラス属性に従わせる
			// （中のテキストは CreateTagField が同じクラスに置く）。注釈へ移した後にビュー
			// ポートのクラス表示を戻す後処理（ShowAllViewportClasses）が下にあるので、ここで
			// 新しいクラスを持ち込んでもタグが映らなくなることはない。
			SetClassByName(object, kTagClass);
			SetAllAttributesByClass(object);

			// **関連付けを先に行う**（中身を組むより前）。タグの本文は関連付け先のレコードから
			// 取るので、相手を決めてからレイアウトを組み、最後に UpdateDataTag で流し込む。
			// フォールバックの直線になった横架材はハンドルが無いので関連付けを省く（タグ自体
			// は置く）。
			if (member != nil && support)
				support->AssociateWithObject(object, member);
			else
				++counts.unassociated;

			// **タグの中身（タグレイアウト）はこのタグへ直接組む**——スタイルは作らないし
			// 当てない（draw/Tag.h の ★。スラブ・壁が構成層を各オブジェクトへ直接与えるのと
			// 同じ）。組めなくても**タグは置く**——タグを失うより、位置だけでも正しいタグを
			// 残した方が原因を追いやすい（寸法が空になるので件数を数えて診断へ回す）。
			if (ResolveTagLayout(object, counts) == nil)
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
			// 引出線が描かれる。draw/Tag.h）。
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

			// **ここで実位置と実寸を測る**。ここまででタグが本文を流し込み、実寸が
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
			const MCObjectHandle layout = HeldTagLayout(placed.front());
			const std::size_t items = layout == nil ? 0 : ContainerCount(layout);
			if (items == 0)
				counts.firstTag = layout == nil ? std::string("レイアウト無し")
												: "レイアウト" + std::to_string(items) + "件";
		}

		// クラスを表示へ戻し、ビューポートを更新して反映する。ConfigureViewport は**タグを
		// 置く前**に走っているので、**タグの中身がその時点で文書に無かったクラスを持ち込んだ
		// 場合**、ここで戻さないと注釈だけが空白のまま残る。
		// 戻すのはビューポートと同じく**全クラス**（draw/DrawUtil の ShowAllViewportClasses）
		// ——タグが身に付けているクラスを数え上げる必要はない。
		// **ここでは描き直さない。** 描き直しは取り込みのいちばん最後（undo イベントを閉じた
		// 後）に RefreshViewports がまとめて行うので、ここで 1 枚ずつ描き直すと同じ図を 2 度
		// 描くだけになる（軸組図は数十枚あり、取り込みが目に見えて遅くなる）。クラスを表示へ
		// 戻すのは**設定**なので、ここでやっておく必要がある。
		if (!placed.empty())
		{
			counts.classesShown += ShowAllViewportClasses(viewport);
			try
			{
				VWViewportObj(viewport).SetDirty(true);
			}
			catch (...)
			{
				// 立てられなくてもタグ自体は図面に残る（表示は次の描き直しで追いつく）。
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
			!classesBroken && !counts.textStyleMissing && !counts.linkMissing &&
			counts.firstTag.empty())
			return {};

		std::string text = label + "の断面寸法タグの診断: ";
		if (!counts.firstTag.empty())
			text += "置いたタグの実際: " + counts.firstTag + "。";
		if (counts.layoutFailed > 0)
			text += "タグレイアウトを組めなかったタグ " + std::to_string(counts.layoutFailed) +
					" 件（断面寸法が空になります）。";
		if (counts.textStyleMissing)
			text += std::string("文字スタイル「") + kTextStyleName +
					"」が文書に無いので大きさだけを与えました。";
		if (counts.linkMissing)
			text += "タグフィールドの式を入れられませんでした（寸法が空になります）。";
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
