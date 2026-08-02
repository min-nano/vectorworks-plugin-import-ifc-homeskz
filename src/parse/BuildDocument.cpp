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
#include "parse/Context.h"
#include "parse/Floor.h"
#include "parse/Grid.h"
#include "parse/Loader.h"
#include "parse/Member.h"
#include "parse/Noboribari.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"
#include "parse/Story.h"

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

		// M7 横架材: 先に組み立てる。ストーリ（母屋・登り梁レイヤを作るか）・垂木（差し込みの
		// 桁幅）・登り梁の位置補正が同じ結果を必要とするため、Context が 1 回だけ解析して
		// 全員へ配る（parse/Context.h の members）。**登り梁は屋根面へスナップ補正**して
		// から Document に載せる（形状先行。parse/Noboribari）。受ける材は横架材のみで、
		// 柱を参照する最終化は M8 で足す（ROADMAP.md M7）。
		document.members = correctNoboribari(context, context.members());
		progress.step();

		// M3 ストーリ: IfcBuildingStorey を解析して StoryCommand を積む（parse/Story）。
		// 以降の要素はここで作られたレベルへ高さをバインドするため、grids より先に置く。
		// 母屋・登り梁レベルは横架材命令の配置先レイヤから決まる（補正はレイヤを変えない
		// ので、Context の補正前の命令で判定して同じ結果になる）。
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

		return document;
	}
} // namespace HomeskzIfcImport::parse
