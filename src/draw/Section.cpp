//
//	draw/Section.cpp
//
//	軸組図（断面ビューポート）描画の実装。Python 版 vw/section.py の execute_sections /
//	_place_section_line / _arrange_viewports に対応する。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド（SDK あり）でのみ
//	コンパイルされ、無 SDK の core/parse ライブラリには入れない。
//
//	使用する SDK API（Vectorworks 2026 SDK。ci-debug の sdk-grep / shell で確認済み）:
//	  * gSDK->CreateSectionViewport(pt1, pt2, pt3, depth, startHeight, endHeight, layer)
//	                                      … 断面ビューポートの新規作成。ヘッダのコメントに
//	                                        「クラス・レイヤの表示はこの呼び出しでは扱わない。
//	                                        呼び出し後に設定し、そのあとで更新すること」と
//	                                        あるので、後段は伏図と同じ手順（draw/DrawUtil の
//	                                        ConfigureViewport）で仕上げる。
//	  * gSDK->SetObjectVariable(h, 1064/1035/1065, …) … 断面の見え方（下記）
//	  * gSDK->AddViewportAnnotationObject / ParentObject / FirstMemberObj / NextObject /
//	    DeleteObject … ビューポート注釈の走査と後始末（下記「通り芯の写り込み」）
//	  * gSDK->GetObjectVariable(h, …)        … 上を**読み戻して効いたか確かめる**（下記）
//	  * gSDK->GetObjectBounds(h, WorldRect&) … できたビューポートの実寸（並べるのに使う）
//	  * gSDK->MoveObject(h, dx, dy)          … シートレイヤ上での移動
//
//	【切断面の与え方（ローカル確認で実証済み）】ISDK の引数名は 3 点とも "sectionLinePt" だが、
//	VW の UI は「切断線の点を 2 つ以上クリック → **切断の向き**をクリック → 奥行きを指定」
//	という順で、API の引数もこの順（点 2 つ＋向きの点＋奥行き）に対応する:
//	  * pt1 / pt2 … 切断線（通りの上を端から端まで。命令の lineStart / lineEnd）
//	  * pt3       … **見る側**を示す点（命令の viewPoint。指示線の中点から視線方向へ離した点）
//	**実機で断面指示線が直線になり、視線の向きも意図どおりであることを確認済み**（pt3 が
//	「切断線の 3 点目」であれば指示線は L 字に折れるはずで、そうならなかった）。視線の向きの
//	決め方（X通り＝−X 方向・Y通り＝+Y 方向。図面の右へ座標が増える＝通り名が左から右へ並ぶ）は
//	parse/Section が持つ。
//
//	【軸組図としての見え方（要件）】**切断面より奥も手前も表示せず**、**プレイナー（レイヤ
//	平面）図形は表示しない**（下記のオブジェクト変数）。**2D コンポーネントは表示したいが
//	SDK からは設定できない**（下記「通り芯の写り込み」）。
//	断面の範囲は、**奥行きは無制限**（0 を渡す）・**高さは建物を包む実寸＋余白**・**長さは
//	断面線の長さ**（指示線を十分外まで延ばして実質無制限にする）。
//
//	【通り芯（グリッド線）の写り込みと、その対処】VW のグリッド線は**デザインレイヤの
//	レイヤ平面に置かれた平面図形**で、ビューポートには**インスタンス**として注釈へ作られる。
//	断面・立面ビューポートに出せるのは**視線に直交するグリッド線だけ**のはずだが、この
//	プラグインが作った断面ビューポートには**切断面に平行な通り芯まで、平面の姿がそのまま
//	寝た水平の一点鎖線として作られてしまう**。実機での切り分け（ROADMAP.md M14 に経過）:
//	  * **同じ書類で手で作った断面ビューポートには出ない**（＝通り芯オブジェクトやレイヤの
//	    表示ではなく、CreateSectionViewport が作るビューポート側の設定差）。
//	  * 表示設定を読み戻した実測で、**手作りは 2D コンポーネント＝表示／プラグイン製は
//	    非表示**と判明した。そして**1059 は true を書いても・更新後に当て直しても入らない**
//	    （1064・1035・1065 は同じ書き方で入る）。VW の UI でもオブジェクト情報パレットや
//	    オーガナイザからは変更できず、**作成時のダイアログかビューポートスタイルでしか
//	    決まらない**設定で、後から書き換える手立てが無い。
//	  * **オブジェクト情報パレットで見比べると、インスタンスが作るビューポートには
//	    「2D コンポーネントを表示」の項目そのものが無い**（手作りには在ってチェック済み）。
//	そこで**設定を直すのは諦め、作られてしまったインスタンスを消す**
//	（RemoveGridAxisInstances）。消すのは**寝ているもの**（外接矩形が横長＝平面の姿のまま
//	写ったもの）だけで、直交する通り芯のインスタンス（鉛直線＋通り名バブル。軸組図に通り名を
//	出す目的そのもの）はそのまま残す。**注釈だけを見た版では消えなかった**（注釈はたどれて
//	通り芯インスタンスも見つかったのに図に残った＝寝た 1 本は注釈の外に居る）ので、
//	ビューポートの子コンテナをすべて見る。居場所は完了ダイアログの棚卸し（原因調査中の
//	一時的な診断）で実機から確かめる。
//
//	【範囲を「無限」にはできない（調べ尽くした結論）】UI で断面ビューポートを手で作ると
//	〈長さ・高さの範囲〉は既定で〈無限〉になるが、**SDK からその状態にする手段は無い**。
//	  * 断面まわりの API は CreateSectionViewport / CreateSectionLineInstance /
//	    IsSectionLineLinkedToViewport / UpdateSectionLineInstances の 4 つだけで、範囲を
//	    切り替える呼び出しは無い（ci-debug で SDK 全体を検索）。
//	  * ovSectionViewport* のオブジェクト変数にも範囲の項目は無い。
//	  * 公開ヘッダの**欠番**（1060〜1090）に隠れている可能性を dev ビルドの一時診断で
//	    調べたが、**手で〈無限〉にした 1 枚とプラグインが作った 65 枚とで値の違う変数は
//	    1 つも無かった**（断面ビューポート 66 枚・値の組み合わせ 1 通り）。範囲はオブジェクト
//	    変数の外に保持されている。
//	そこで**建物の大きさから有限の範囲を決める**（高さ＝core::sectionHeightRange、長さ＝
//	指示線を通り芯 bbox より十分外へ延ばす）。実用上は無限と同じ見え方になる。
//	いずれも「どこを切るか」ではなく「どう描くか」なので、命令セット（core::SectionCommand）
//	には載せずここが持つ。
//

#include "PluginPrefix.h"
#include "draw/Section.h"
#include "draw/DrawUtil.h"
#include "draw/Tag.h"
#include "core/Document.h"
#include "core/Progress.h"

// 注釈の走査に使う VWFC ラッパー（通り芯インスタンスの判別＝VWParametricObj、注釈グループを
// 特定する使い捨て図形＝VWPolygon2DObj）。
#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// シートレイヤ上でビューポートを並べるレイアウト（用紙上・mm。Python 版 vw/section.py の
		// _ARRANGE_ORIGIN / _ARRANGE_COLUMNS / _ARRANGE_GAP と同じ値）。左上を基準に 1 行
		// kArrangeColumns 枚ずつ、各ビューポートの実寸に余白を足して詰める。
		constexpr double kArrangeOriginX = 0.0;
		constexpr double kArrangeOriginY = 0.0;
		constexpr int kArrangeColumns = 5;
		constexpr double kArrangeGap = 300.0;

		// 切断面より奥の範囲に渡す値。**0 が「無限」**（ローカル確認で実測: 0 を渡した
		// ビューポートの「断面の詳細設定」で〈切断面より奥の範囲: 無限〉になっていた）。
		// 奥は表示しない設定（下記 1064）なので実際には効かないが、範囲の指定としては
		// 無制限にしておく。
		constexpr double kInfiniteDepth = 0.0;

		// **高さの範囲だけは実寸を渡す**（core::sectionHeightRange。建物の上下＋余白）。
		// 同じ 0 を高さへ渡すと〈高さの範囲: 有限・始点 0・終点 0〉になり、断面から建物が
		// 消えてしまうことがローカル確認で分かった。**高さ・長さを「無限」にする手段は
		// SDK に無い**（ファイル冒頭「範囲を『無限』にはできない」）ので、実寸＋余白の
		// 有限範囲で建物全体を収める（ROADMAP.md M14）。
		//
		// 長さの範囲も同じ理由で〈断面線の長さ〉のままになる。こちらは**指示線を通り芯の
		// bbox より十分外まで延ばす**ことで実質無制限にしている（parse/Section の
		// kSectionLineMargin）。

		// 断面ビューポートの表示設定（オブジェクト変数。Kernel/API/ObjectVariables.h。
		// 名前と番号は ci-debug の sdk-grep で確認済み）。
		//   1064 ovSectionViewportDisplayObjectsBeyondCutPlane … 切断面より**奥**を出すか
		//   1035 ovViewportDisplayPlanar … **プレイナー（レイヤ平面）図形と 2D 図形**を出すか
		//   1059 ovViewportDisplay2DComponents … ハイブリッドの 2D コンポーネントを出すか
		//   1065 ovSectionViewportDisplayObjectsBeforeCutPlane … 切断面より**手前**を出すか
		// **どれもビューポートの更新より前に設定する**（更新時の描画へ効かせるため。
		// CreateSectionViewport のヘッダコメントも「表示設定は呼び出し後、更新はその後」）。
		constexpr short kOVDisplayObjectsBeyondCutPlane = 1064;
		constexpr short kOVDisplayPlanarObjects = 1035;
		constexpr short kOVDisplayObjectsBeforeCutPlane = 1065;

		// 軸組図としてこうあってほしい値と、診断に出す表示名。
		//
		// **1059（2D コンポーネント）はここに入れない**——実機の読み戻しで、
		// CreateSectionViewport 直後は非表示・true を書いても・更新後に当て直しても**非表示の
		// まま**だと分かった（1064・1035・1065 は同じ書き方で入っている＝読み出しは正しい）。
		// VW 側でも**オブジェクト情報パレットやオーガナイザからは変更できず、作成時のダイアログ
		// かビューポートスタイルでしか決まらない**設定で、後から書き換える手立てが無い。
		// この設定が非表示のままだと**切断面に平行な通り芯のインスタンスが水平線として
		// 注釈に作られてしまう**ので、そちらは作られたインスタンスを消して対処する
		// （RemoveGridAxisInstances。ファイル冒頭「通り芯（グリッド線）の写り込み」）。
		struct DisplayOption
		{
			short selector = 0;
			bool wanted = false;
			const char* label = "";
		};
		constexpr std::array<DisplayOption, 3> kDisplayOptions = {{
			{kOVDisplayObjectsBeyondCutPlane, false, "1064 奥"},
			{kOVDisplayPlanarObjects, false, "1035 プレイナー"},
			{kOVDisplayObjectsBeforeCutPlane, false, "1065 手前"},
		}};

		// オブジェクト変数へ真偽値を書き込む（draw/Footing の SetBooleanVariable と同じ流儀）。
		void SetBooleanVariable(MCObjectHandle object, short variable, Boolean value)
		{
			gSDK->SetObjectVariable(object, variable, TVariableBlock(value));
		}

		// オブジェクト変数の真偽値を読む。読めなければ false を返し out は触らない
		// （＝「読めない」と「false だった」を取り違えない）。
		bool ReadBooleanVariable(MCObjectHandle object, short variable, bool& out)
		{
			TVariableBlock block;
			if (!gSDK->GetObjectVariable(object, variable, block))
				return false;
			return block.GetBoolean(out);
		}

		// 断面ビューポートを 1 枚作る。作れなければ nil。奥行きは無制限、高さは建物を包む
		// 実寸（上記）。
		MCObjectHandle CreateSectionViewport(const core::SectionCommand& command,
											 MCObjectHandle sheetLayer, double startHeight,
											 double endHeight)
		{
			const WorldPt start(command.lineStart.x, command.lineStart.y);
			const WorldPt end(command.lineEnd.x, command.lineEnd.y);
			const WorldPt view(command.viewPoint.x, command.viewPoint.y);
			return gSDK->CreateSectionViewport(start, end, view, kInfiniteDepth, startHeight,
											   endHeight, sheetLayer);
		}

		// 軸組図としての見え方を整える（要件。ConfigureViewport＝更新より**前**に呼び、
		// 効いていなければ更新の**後**にもう一度呼ぶ）。
		void ApplySectionDisplayOptions(MCObjectHandle viewport)
		{
			for (const DisplayOption& option : kDisplayOptions)
				SetBooleanVariable(viewport, option.selector, static_cast<Boolean>(option.wanted));
		}

		// **設定が効いたかを読み戻して確かめる**（要件どおりでない項目だけを "1035 プレイナー=
		// 表示（期待: 非表示）" の形で返す。すべて要件どおりなら空文字）。
		//
		// 【なぜ読むか】SetObjectVariable は**変数が読み取り専用・型違い・そのオブジェクトに
		// 無い**とき黙って何もしない（返り値も無い）。「設定したつもりで効いていない」は目視で
		// 見抜けないので、図面から読み戻して確かめる——1059（2D コンポーネント）が書けない
		// ことも、この読み戻しで初めて分かった（kDisplayOptions のコメント）。
		std::string CheckDisplayOptions(MCObjectHandle viewport)
		{
			std::string text;
			for (const DisplayOption& option : kDisplayOptions)
			{
				bool actual = false;
				if (!ReadBooleanVariable(viewport, option.selector, actual))
					continue; // 読めない項目は判定材料にしない
				if (actual == option.wanted)
					continue;
				if (!text.empty())
					text += " / ";
				text += std::string(option.label) + "=" + (actual ? "表示" : "非表示") +
						"（期待: " + (option.wanted ? "表示" : "非表示") + "）";
			}
			return text;
		}

		// --- 切断面に平行な通り芯インスタンスの削除 -----------------------------------
		//
		// VW はビューポートごとに通り芯（グリッド線）の**インスタンス**を注釈へ作る。本来
		// 断面ビューポートに作られるのは**視線に直交する**通り芯だけだが、2D コンポーネント
		// 表示が非表示のまま（＝SDK から直せない。上記）だと、**切断面に平行な通り芯まで
		// 水平の一点鎖線として作られてしまう**。そこで作られてしまったものを消す。
		//
		// 見分け方は**向き**: 直交する通り芯のインスタンスは鉛直線＋足元のバブル（＝縦長）、
		// 平行なものは平面の姿がそのまま寝た水平線＋左端のバブル（＝横長）になる。外接矩形の
		// 幅が高さを上回るものを「寝ている」と判定する。

		// グリッド線の PIO 名（VW 標準。draw/Grid が CreateCustomObjectPath へ渡すのと同じ
		// universal 名）。インスタンスも同じ PIO なので、この名前で拾える。
		constexpr const char* kGridAxisPlugin = "GridAxis";

		// 注釈の走査結果（診断用）。
		struct AnnotationCounts
		{
			std::size_t viewports = 0;	 // 注釈をたどれたビューポート
			std::size_t unreachable = 0; // 注釈グループを取れなかったビューポート
			std::size_t gridInstances = 0; // 見つけた通り芯インスタンス
			std::size_t removed = 0;	   // うち寝ていて消したもの
			// **1 枚目のビューポートの中身の棚卸し**（原因調査中の一時的な診断）。
			// 「注釈はたどれて通り芯インスタンスも見つかったのに、寝た 1 本が消えない」という
			// 実機の症状は、**その 1 本が注釈以外のコンテナに居る**か**PIO 名が違う**かの
			// どちらかでしか説明が付かない。憶測を止めるために、ビューポートの子コンテナと
			// その中身を種別・向きごとに数えて出す。
			std::string inventory;
		};

		// オブジェクトの向き。外接矩形の幅と高さで決める（測れなければ Unknown）。
		enum class Orientation
		{
			Flat,	 // 横長＝寝ている（平面の姿のまま写ったもの）
			Upright, // 縦長＝立っている（断面用に描かれたもの）
			Unknown, // 測れなかった
		};

		Orientation OrientationOf(MCObjectHandle object)
		{
			WorldRect bounds;
			if (!gSDK->GetObjectBounds(object, bounds))
				return Orientation::Unknown;
			const double width = std::abs(bounds.right - bounds.left);
			const double height = std::abs(bounds.top - bounds.bottom);
			return width > height ? Orientation::Flat : Orientation::Upright;
		}

		// オブジェクトの種別名。PIO なら universal 名（"GridAxis" 等）、そうでなければ
		// "t<型番号>"（GetObjectTypeN）。棚卸しの見出しに使う。
		std::string TypeLabelOf(MCObjectHandle object)
		{
			try
			{
				const VWParametricObj pio(object);
				return std::string(pio.GetParametricName().GetData());
			}
			catch (...)
			{
				// PIO ではない。型番号で表す（名前は要らない——「何が何個あるか」だけ分かれば
				// 寝た 1 本の居場所は特定できる）。
				return "t" + std::to_string(static_cast<int>(gSDK->GetObjectTypeN(object)));
			}
		}

		// オブジェクトが通り芯（GridAxis）の PIO か。PIO でなければ VWParametricObj の構築が
		// 例外を投げるので、それを掴んで false にする。
		bool IsGridAxisObject(MCObjectHandle object)
		{
			try
			{
				const VWParametricObj pio(object);
				return pio.GetParametricName() == TXString(kGridAxisPlugin);
			}
			catch (...)
			{
				// PIO ではない（＝通り芯インスタンスではない）。注釈には自前のデータタグや
				// VW の作る図形も入るので、ここは素通りが正常。
				return false;
			}
		}

		// コンテナの中身を「種別 横n/縦m/不明k」の形に数え上げる（棚卸し）。中身が無ければ
		// "空"。**コンテナでないもの**（FirstMemberObj が nil）も "空" になる。
		std::string InventoryOf(MCObjectHandle container)
		{
			// 種別ごとの [横, 縦, 不明]。並びを決定的にするため map（名前順）で持つ。
			std::map<std::string, std::array<std::size_t, 3>> tally;
			std::size_t members = 0;
			for (MCObjectHandle member = gSDK->FirstMemberObj(container); member != nil;
				 member = gSDK->NextObject(member))
			{
				++members;
				// 数が多い図面で文字列が際限なく伸びないよう、数える対象は上限で打ち切る
				// （打ち切っても「何が居るか」は分かる）。
				if (members > 200U)
					break;
				std::array<std::size_t, 3>& counts = tally[TypeLabelOf(member)];
				switch (OrientationOf(member))
				{
				case Orientation::Flat:
					++counts[0];
					break;
				case Orientation::Upright:
					++counts[1];
					break;
				case Orientation::Unknown:
					++counts[2];
					break;
				}
			}
			if (tally.empty())
				return "空";
			std::string text;
			for (const auto& [label, counts] : tally)
			{
				if (!text.empty())
					text += ", ";
				text += label;
				if (counts[0] > 0)
					text += " 横" + std::to_string(counts[0]);
				if (counts[1] > 0)
					text += " 縦" + std::to_string(counts[1]);
				if (counts[2] > 0)
					text += " 不明" + std::to_string(counts[2]);
			}
			return text;
		}

		// ビューポートの**注釈グループ**を返す。取れなければ nil。
		//
		// 【なぜ回りくどいのか】ビューポートのグループ（Crop=1 / Annotation=2 / Cache=3）を
		// 直に取る API は **VectorScript の GetVPGroup にはあるが SDK には無い**（ci-debug で
		// SDK 全体を検索）。そこで**注釈へ入れた図形の親**をたどる——AddViewportAnnotationObject
		// は渡した図形を注釈グループへ移すので（draw/Tag が頼っているのと同じ挙動）、使い捨ての
		// 図形を 1 つ入れて親を控え、すぐ消せば注釈グループのハンドルだけが手に入る。
		MCObjectHandle AnnotationGroupOf(MCObjectHandle viewport)
		{
			// 使い捨ての 2 点ポリライン（draw/Grid のパスと同じ作り方。カレントレイヤに出る）。
			const VWPolygon2DObj probe({VWPoint2D(0.0, 0.0), VWPoint2D(1.0, 0.0)});
			const MCObjectHandle probeHandle = probe.GetThisObject();
			if (probeHandle == nil)
				return nil;
			if (!gSDK->AddViewportAnnotationObject(viewport, probeHandle))
			{
				// 注釈へ入らなかった。カレントレイヤに残る（draw/Tag の「注釈に入らなかった
				// タグは消す」と同じ理由）ので必ず消す。
				gSDK->DeleteObject(probeHandle, true);
				return nil;
			}
			const MCObjectHandle group = gSDK->ParentObject(probeHandle);
			gSDK->DeleteObject(probeHandle, true);
			return group;
		}

		// ビューポートの子コンテナを棚卸しして 1 行にまとめる（原因調査中の一時的な診断）。
		// 注釈グループには印を付ける（どれが注釈かを実機で確かめるため）。
		std::string InventoryOfViewport(MCObjectHandle viewport, MCObjectHandle annotation)
		{
			std::string text;
			std::size_t index = 0;
			for (MCObjectHandle child = gSDK->FirstMemberObj(viewport); child != nil;
				 child = gSDK->NextObject(child))
			{
				++index;
				if (index > 8U)
				{
					text += " …";
					break;
				}
				if (!text.empty())
					text += " ";
				text += "[";
				if (child == annotation)
					text += "注釈 ";
				text += "t" + std::to_string(static_cast<int>(gSDK->GetObjectTypeN(child))) + ": " +
						InventoryOf(child) + "]";
			}
			if (text.empty())
				text = "（子コンテナ無し）";
			return text;
		}

		// コンテナ 1 つから、寝ている通り芯インスタンスを消す。見つけた数・消した数を足し込む。
		void RemoveFlatGridAxes(MCObjectHandle container, AnnotationCounts& counts)
		{
			// **消す前に一覧を作る**——走査しながら消すと、消した図形から NextObject を
			// たどることになる。
			std::vector<MCObjectHandle> doomed;
			for (MCObjectHandle member = gSDK->FirstMemberObj(container); member != nil;
				 member = gSDK->NextObject(member))
			{
				if (!IsGridAxisObject(member))
					continue;
				++counts.gridInstances;
				if (OrientationOf(member) == Orientation::Flat)
					doomed.push_back(member);
			}
			for (const MCObjectHandle member : doomed)
			{
				gSDK->DeleteObject(member, true);
				++counts.removed;
			}
		}

		// ビューポートから、寝ている通り芯インスタンスを消す。
		//
		// **注釈だけでなくビューポートの子コンテナすべてを見る**——注釈だけを見た版では
		// 「注釈はたどれて通り芯インスタンスも見つかったのに、寝た 1 本が図に残る」という
		// 実機の症状になった（＝寝た 1 本は注釈の外に居る）。どこに居るかは棚卸し
		// （counts.inventory）で分かるので、それまでは見つけたところで消す。
		void RemoveGridAxisInstances(MCObjectHandle viewport, AnnotationCounts& counts)
		{
			const MCObjectHandle annotation = AnnotationGroupOf(viewport);
			// **1 枚目だけ棚卸しする**（全命令で同じ構造なので、診断行を命令の数だけ並べない）。
			const bool inspect = counts.viewports == 0 && counts.unreachable == 0;
			if (inspect)
				counts.inventory = InventoryOfViewport(viewport, annotation);
			if (annotation == nil)
				++counts.unreachable;
			else
				++counts.viewports;

			for (MCObjectHandle child = gSDK->FirstMemberObj(viewport); child != nil;
				 child = gSDK->NextObject(child))
				RemoveFlatGridAxes(child, counts);

			// 消した後をもう一度棚卸しする（**消えたことを図面から確かめる**ため。消えているのに
			// 図に残るなら、VW があとで作り直しているということになる）。
			if (inspect)
				counts.inventory += " → 削除後: " + InventoryOfViewport(viewport, annotation);
		}

		// 走査の結果を人が読める 1 行にする。**原因調査中は毎回出す**——「注釈はたどれて
		// 通り芯インスタンスも見つかったのに、寝た 1 本が図に残る」という症状の居場所を
		// 実機の棚卸しから突き止めるため（原因が確定したら異常時だけに戻す）。
		std::string AnnotationDiagnostics(const AnnotationCounts& counts)
		{
			std::string text =
				"軸組図の通り芯インスタンス: 注釈 " + std::to_string(counts.viewports) + " 枚";
			if (counts.unreachable > 0)
				text += "（たどれず " + std::to_string(counts.unreachable) + " 枚）";
			text += " / 見つけた " + std::to_string(counts.gridInstances) + " / 消した " +
					std::to_string(counts.removed);
			if (!counts.inventory.empty())
				text += " ｜ 1 枚目: " + counts.inventory;
			return text;
		}

		// できたビューポートをシートレイヤ上で重ならないように格子状へ並べる（Python 版
		// _arrange_viewports）。**実寸は描いてみるまで分からない**ので、GetObjectBounds で
		// 測ってから左上を合わせる。測れないものはその場に残す（並びが崩れるだけで図は残る）。
		void ArrangeViewports(const std::vector<MCObjectHandle>& viewports)
		{
			double cursorX = kArrangeOriginX;
			double cursorY = kArrangeOriginY;
			double rowHeight = 0.0;
			int column = 0;
			for (const MCObjectHandle viewport : viewports)
			{
				WorldRect bounds;
				if (!gSDK->GetObjectBounds(viewport, bounds))
					continue;
				const double width = bounds.right - bounds.left;
				// WorldRect は top > bottom（Y 上向き）。高さは絶対値で見る。
				const double height = std::abs(bounds.top - bounds.bottom);
				gSDK->MoveObject(viewport, cursorX - bounds.left, cursorY - bounds.top);
				cursorX += width + kArrangeGap;
				rowHeight = std::max(rowHeight, height);
				++column;
				if (column >= kArrangeColumns)
				{
					column = 0;
					cursorX = kArrangeOriginX;
					cursorY -= rowHeight + kArrangeGap;
					rowHeight = 0.0;
				}
			}
		}
	} // namespace

	std::size_t drawSections(const core::Document& document, core::ProgressReporter& progress,
							 std::string* note, const ObjectHandles* memberHandles)
	{
		const std::vector<core::SectionCommand>& commands = document.sections;
		if (commands.empty())
			return 0;

		// レイヤの走査とクラスの数え上げは全命令で共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup(document);

		// 断面の高さ範囲も全命令で共通（建物を包む実寸＋余白。core::sectionHeightRange）。
		// **求まらないときは描かない**——高さの分かる要素が 1 つも無い文書では out が
		// 触られず 0〜0 のままで、その範囲で作ると「軸組図はあるのに空」になる（高さに 0 を
		// 渡すと〈高さの範囲: 有限・0〜0〉になる。ファイル冒頭）。parse/Section を通った
		// 文書ならここへは来ないが、drawSections は**任意の Document を取れる公開関数**
		// なので、暗黙の不変条件に寄りかからず理由を残して抜ける。
		double startHeight = 0.0;
		double endHeight = 0.0;
		if (!core::sectionHeightRange(document, startHeight, endHeight))
		{
			if (note != nullptr)
			{
				if (!note->empty())
					*note += "\n";
				*note += "軸組図の診断: 建物の高さが求まらないため作成しませんでした。";
			}
			return 0;
		}

		// 描画の前後でカレントレイヤが変わらないようにする（伏図と同じ作法）。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;
		std::size_t classesApplied = 0;
		// 表示設定の実測（要件どおりにならなかった項目だけ。CheckDisplayOptions）。全命令で
		// 同じ設定なので 1 枚目だけ見る。
		std::string displayNote;
		// 注釈から消した通り芯インスタンスの集計（RemoveGridAxisInstances）。
		AnnotationCounts annotations;
		// 断面寸法データタグ（M13）。伏図と同じ受け渡し・同じ実装（draw/Tag）。
		const ObjectHandles emptyHandles;
		const ObjectHandleTable& members =
			memberHandles != nullptr ? memberHandles->table() : emptyHandles.table();
		TagCounts tags;
		// タグ PIO の定義を先に用意する（伏図と同じ。draw/Tag.h）。タグが 1 つも無い文書では
		// 定義そのものを作らない。
		if (std::ranges::any_of(commands, [](const core::SectionCommand& section)
								{ return !section.viewport.tags.empty(); }))
			prepareDataTagPlugin();
		std::vector<MCObjectHandle> viewports;
		viewports.reserve(commands.size());

		for (const core::SectionCommand& command : commands)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。
			if (progress.cancelled())
				break;
			progress.step();

			const MCObjectHandle sheetLayer = PrepareSheetLayer(command.number, command.title);
			if (sheetLayer == nil)
			{
				++missingSheetLayers;
				continue;
			}

			const MCObjectHandle viewport =
				CreateSectionViewport(command, sheetLayer, startHeight, endHeight);
			if (viewport == nil)
			{
				// 断面ビューポートを作れなかった。件数を診断へ残して「命令はあるのに軸組図が
				// 無い」原因を切り分けられるようにする（1 枚の失敗で残りを止めない）。
				++missingViewports;
				continue;
			}

			// 表示の作法（奥・手前を出さない／プレイナー図形を出さない）は**更新より前**に
			// 設定する（ConfigureViewport の最後が更新）。効いたかは 1 枚目だけ読み戻す
			// （全命令で同じ設定なので、診断行を命令の数だけ並べない）。
			ApplySectionDisplayOptions(viewport);
			classesApplied += ConfigureViewport(viewport, sheetLayer, setup, command.viewport);
			if (drawn == 0)
				displayNote = CheckDisplayOptions(viewport);

			// **更新のあと**に、切断面へ平行なまま作られてしまった通り芯インスタンスを消す
			// （上記 RemoveGridAxisInstances）。インスタンスは更新で作られるので、
			// ConfigureViewport（最後が更新）より前にやっても消すものが無い。
			RemoveGridAxisInstances(viewport, annotations);

			// 断面寸法データタグ。**並べ替え（ArrangeViewports）より前**に置く——注釈は
			// ビューポートと一緒に動くので、先に置いておけば移動しても図の上に留まる。
			drawViewportTags(viewport, command.viewport, members, tags);
			viewports.push_back(viewport);
			++drawn;
		}

		// 作ったぶんをシート上に並べる（中止で途中まででも、描けたものは重ならないようにする）。
		ArrangeViewports(viewports);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		// 表示設定が効かなかったときの診断（原因が別物なので他とは別行にする）。**図は出て
		// いるのに見え方だけが違う**という不具合は目視で見落としやすいので、実測値を出す。
		if (note != nullptr && !displayNote.empty())
		{
			if (!note->empty())
				*note += "\n";
			*note += "軸組図の表示設定: " + displayNote;
		}

		// 通り芯インスタンスの診断（同上、別行）。
		const std::string annotationNote = AnnotationDiagnostics(annotations);
		if (note != nullptr && !annotationNote.empty())
		{
			if (!note->empty())
				*note += "\n";
			*note += annotationNote;
		}

		const bool classesBroken = drawn > 0 && classesApplied == 0;
		if (note != nullptr && (missingSheetLayers > 0 || missingViewports > 0 || classesBroken))
		{
			std::string text = "軸組図の診断: ";
			if (missingSheetLayers > 0)
				text += "シートレイヤを作れなかった命令 " + std::to_string(missingSheetLayers) +
						" 件。";
			if (missingViewports > 0)
				text += "断面ビューポートを作れなかった命令 " + std::to_string(missingViewports) +
						" 件。";
			if (classesBroken)
				text += "クラスを表示に戻せませんでした（対象 " +
						std::to_string(setup.classes.size()) + " クラス）。図形が映りません。";
			if (!note->empty())
				*note += "\n";
			*note += text;
		}

		// タグの診断は軸組図の診断とは別行にする（原因が別物なので混ぜない）。
		const std::string tagNote = tagDiagnostics("軸組図", tags);
		if (note != nullptr && !tagNote.empty())
		{
			if (!note->empty())
				*note += "\n";
			*note += tagNote;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
