//
//	parse/BuildDocument.cpp
//
//	buildDocument の実装。Python 版 ifc/__init__.py build_document に対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	処理の順は、
//	  1. parse/Loader … IFC を読み込む（parse/Step でトークナイズ＋エンティティグラフ構築。
//	                    サニタイズはしない。理由は parse/Loader.h 参照）
//	  2. parse/Story … parse/Grid … parse/Floor … 要素ごとに Document を組み立てる
//	で、以降のマイルストーンでは 2 に要素を足していく（ROADMAP.md）。
//
//	各要素の解析は「ストーリ一覧」「通り芯のセンタリング中心」「階に属する要素」「屋根面」を
//	共通して必要とするので、**共有コンテキスト（parse/Context）を 1 つだけ作って全要素へ
//	渡す**。要素ごとに作り直すと同じ走査が要素の数だけ走る（parse/Context.h の冒頭参照）。
//

#include "parse/BuildDocument.h"
#include "core/Progress.h"
#include "parse/AnchorBolt.h"
#include "parse/Column.h"
#include "parse/ColumnMark.h"
#include "parse/Context.h"
#include "parse/FireBrace.h"
#include "parse/Floor.h"
#include "parse/FloorPost.h"
#include "parse/Footing.h"
#include "parse/Grid.h"
#include "parse/Joint.h"
#include "parse/Loader.h"
#include "parse/Member.h"
#include "parse/Noboribari.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"
#include "parse/Section.h"
#include "parse/Sheet.h"
#include "parse/Story.h"
#include "parse/Tag.h"

#include <utility>

namespace HomeskzIfcImport::parse
{
	core::Document buildDocument(const std::string& ifcPath)
	{
		// 進捗を表示しない呼び出し（単体テスト・従来の呼び出し口）。振る舞いは同じ。
		core::NullProgressReporter noProgress;
		return buildDocument(ifcPath, noProgress);
	}

	core::Document buildDocument(const std::string& ifcPath, core::ProgressReporter& progress)
	{
		// Phase 1 の入口: Loader でファイルを読み、最小 STEP リーダで
		// エンティティグラフ（Model）を構築する。読み込み失敗（存在しない・空）でも
		// 例外を漏らさず、空の Model として先へ進む（1 要素の欠損で全体を止めない）。
		//
		// 読み込みは進み具合を刻めない（STEP リーダが 1 回で走り切る）ので、総数 0 の
		// フェーズ＝見出しだけを出す。配分は core/Progress.h の kLoadShare。
		progress.beginPhase("IFC ファイルを読み込んでいます…", core::kLoadShare, 0);
		Model const model = loadIfc(ifcPath);

		// 以降は要素ごとに 1 ステップ進める（回数は core::kParseSteps と一致させる）。
		progress.beginPhase("IFC を解析しています…", core::kParseShare, core::kParseSteps);

		// Phase 1 の共有キャッシュ。以降の build*Commands はすべてこの 1 つを通すので、
		// ストーリ収集・通り芯の線分収集・ロフト床の合成・屋根面の解決はそれぞれ 1 回で済む。
		Context context(model);

		core::Document document;

		// M8 柱: 横架材（Context が 1 回だけ解析する）を入力に柱命令を組み立てる。span の
		// to レベル判定に上階の横架材下端が要るためで、Python 版 build_document の順序
		// （members → columns → correct_noboribari）と同じ。柱もストーリ（span レベル）・
		// 登り梁の端部詰めが共有するので、Context がキャッシュする（parse/Context.h の columns）。
		document.columns = context.columns();
		progress.step();

		// M7 横架材: ストーリ（母屋・登り梁レイヤを作るか）・垂木（差し込みの桁幅）・登り梁の
		// 位置補正が同じ結果を必要とするため、Context が 1 回だけ解析して全員へ配る
		// （parse/Context.h の members）。**登り梁は屋根面へスナップ補正**してから Document に
		// 載せる（形状先行。parse/Noboribari）。受ける材は横架材と柱の両方（M8 で最終化）。
		document.members = correctNoboribari(context, context.members(), document.columns);
		progress.step();

		// M3 ストーリ: IfcBuildingStorey を解析して StoryCommand を積む（parse/Story）。
		// 以降の要素はここで作られたレベルへ高さをバインドするため、grids より先に置く。
		// 母屋・登り梁レベルは横架材命令の配置先レイヤから、span 柱レベルは柱命令の配置先
		// レイヤから決まる（登り梁の補正はレイヤを変えないので、Context の補正前の命令で
		// 判定して同じ結果になる）。
		document.stories = buildStoryCommands(context);
		progress.step();

		// M1 通り芯: IfcGridAxis を解析して GridCommand を積む（parse/Grid）。
		document.grids = buildGridCommands(context);
		progress.step();

		// M5 床板: 床版（IfcSlab "床版"）を解析して FloorCommand を積む（parse/Floor）。
		// 床は建物形状の一次情報で、以降の横架材・柱はこの位置に合わせる（形状先行）。
		document.floors = buildFloorCommands(context);
		progress.step();

		// M6 屋根面・屋根組: 屋根版（IfcSlab "屋根版"）から垂木・野地板を導出する
		// （parse/Rafter / parse/Roof）。屋根面は建物形状の要で、上の登り梁はここで確定した
		// 屋根面へスナップ補正されている（形状先行）。垂木の差し込みに使う桁幅は**補正後の**
		// 横架材命令から引く（Python 版 build_document と同じ順序）。以降のマイルストーンで
		// Column … の解析を同様に足していく（ROADMAP.md）。
		document.rafters = buildRafterCommands(context, document.members);
		progress.step();
		document.roofs = buildRoofCommands(context);
		progress.step();

		// M9 基礎: 立上り（壁）→ 底盤（スラブ）。立上りは自由端を柱芯へ寄せるので柱命令を、
		// 底盤は外周を立上りの外面へ合わせるので立上りの命令を入力に取る（Python 版
		// build_document と同じ依存の向き）。Context が立上りを 1 回だけ組み立てて両者へ配る。
		// 立上りには人通口の分割・切り下げまで反映されている（M10。parse/Footing.h）。
		document.walls = context.walls();
		progress.step();
		// M10 壁結合: 交差する立上りどうしの結合命令。命令の a / b は **walls の添字**なので、
		// walls を確定させた**直後**に組み立てる（並びが変わると添字がずれる）。続けて、その
		// 結合から各立上りの**端部を閉じるか**（capStart / capEnd）を決めて書き戻す
		// （VW の壁のキャップは結合任せにせず解析側で決める。core/Document.h 参照）。
		document.wallJoins = buildWallJoinCommands(document.walls);
		applyWallCaps(document.walls, document.wallJoins);
		document.slabs = buildSlabCommands(context, document.walls);
		progress.step();

		// 基礎要素があれば**基礎ストーリを stories の先頭（最下層）へ**置く（Python 版
		// build_document と同じ）。Elevation=0 で最下層になり、レイヤの希望スタック順
		// （最上階→最下階）でも最下段に積まれる。基礎ストーリは FL 階の命令を変えない。
		core::StoryCommand foundationStory;
		if (buildFoundationStoryCommand(model, foundationStory))
			document.stories.insert(document.stories.begin(), std::move(foundationStory));

		// M11 シンボル置換系（アンカーボルト・床束・火打・仕口）。互いに独立だが、
		// **仕口だけは横架材・柱の命令から導出する**ので、上の members / columns が確定した
		// 後に置く（Python 版 build_document と同じ順序）。仕口が見る横架材は登り梁の屋根
		// スナップ**後**——受ける材との取り合いは補正後の位置で決まる。
		document.anchorBolts = buildAnchorBoltCommands(context);
		progress.step();
		document.floorPosts = buildFloorPostCommands(context);
		progress.step();
		document.fireBraces = buildFireBraceCommands(context);
		progress.step();
		document.joints = buildJointCommands(document.members, document.columns);
		progress.step();

		// M12 断面記号・伏図記号。柱の命令だけから決まる（IFC は見ない）ので columns の
		// 後ならどこでもよいが、**伏図より前**に置く必要がある——伏図は伏図記号レイヤを
		// 表示レイヤに載せるため、そのレイヤ名を決める側が先に確定していないといけない。
		document.columnMarks = buildColumnMarkCommands(document.columns);
		progress.step();

		// M13 シート（伏図）: 基礎伏図・各階の柱梁伏図・母屋伏図。**どの伏図に何を映すかは
		// 他の要素が出した答え（基礎の有無・柱の span・横架材の配置先レイヤ・屋根版の有無）
		// から決まる**ので、それらが確定した後＝最後に組み立てる（parse/Sheet）。
		document.sheets = buildSheetCommands(context);
		progress.step();

		// M14 軸組図（断面ビューポート）: 柱梁の芯を通る通りを切断位置にし、そこへ断面
		// ビューポートを 1 枚ずつ作る。**組み立て済みの Document を入力に取る**——切断位置は
		// 柱・横架材の命令から、映すレイヤはストーリの命令から、断面の高さ範囲は各要素の Z から
		// 決まるので、ここが最後になる（parse/Section）。
		document.sections = buildSectionCommands(context, document);
		progress.step();

		// M13 断面寸法データタグ: 伏図・軸組図の**両方**のビューポート注釈に、横架材の断面
		// 寸法を示すデータタグを載せる（Python 版は伏図だけ）。タグはビューポート命令の中に
		// 入るので、**sheets / sections が確定した後**でなければ置き場所が決まらない
		// ——したがってここが最後になる（parse/Tag）。
		attachTagCommands(document);
		progress.step();

		return document;
	}
} // namespace HomeskzIfcImport::parse
