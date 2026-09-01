//
//	parse/Context.h
//
//	Phase 1（IFC 解析）の共有コンテキスト。1 回のインポートで**同じ Model に対して
//	何度も同じ答えを出す前処理**を 1 か所へ集め、最初に求めた結果を使い回す。
//
//	【なぜ要るか】parse/BuildDocument は要素ごとの build*Commands を順に呼ぶが、各要素は
//	いずれも「ストーリ一覧」「通り芯のセンタリング中心」「階に属する要素」「屋根面」を
//	必要とする。素直に書くと要素の数だけ同じ走査が走る（実測: ストーリ収集 4 回・通り芯の
//	線分収集 4 回・ロフト床の領域合成 2 回・屋根版 1 枚あたり屋根面の解決 2 回）。いずれも
//	同じ Model に対する純粋な計算なので、結果は毎回同じ——要素が増えるほど倍率だけが乗る。
//	コンテキストはこの重複をなくすためのもので、**振る舞いは一切変えない**。
//
//	【使い方】buildDocument が 1 つだけ作り、各 build*Commands へ参照で渡す。単体テストの
//	都合で `const Model&` を直接取る従来のオーバーロードも残してあり、そちらは内部で
//	コンテキストを 1 つ作って捨てる（＝従来どおりの挙動）。
//
//	【キャッシュの一貫性】保持するのは Model への参照だけで、Model は解析中に変化しない
//	（parse/Step の Model は構築後は読み取り専用）。したがってキャッシュが古くなることは
//	無い。コンテキストは 1 回のインポートより長生きさせない。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//

#pragma once

#include "core/Geometry.h"
#include "core/Document.h"
#include "core/ImportOptions.h"
#include "parse/Column.h"
#include "parse/Floor.h"
#include "parse/Footing.h"
#include "parse/Grid.h"
#include "parse/IfcGeometry.h"
#include "parse/Member.h"
#include "parse/Step.h"
#include "parse/Story.h"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// 解析中に共有する遅延キャッシュ。アクセサはいずれも**初回に計算して以後は同じ値を
	// 返す**（そのため非 const。const 参照で配りたくなるが、mutable を持ち込むより
	// 「変更しうる」ことを型で示す方が読み違えにくい）。
	class Context
	{
	public:
		explicit Context(const Model& model) : fModel(&model) {}

		// 取り込み設定（置換するシンボルの対応）付き。設定は**取り込み 1 回で固定**なので
		// 値で持ち、要素の解析はここから名前を引く（core/ImportOptions.h）。
		Context(const Model& model, core::ImportOptions options)
			: fModel(&model), fOptions(std::move(options))
		{
		}

		const Model& model() const
		{
			return *fModel;
		}

		// 取り込み設定。設定ダイアログを通さずに作ったコンテキストでは既定（従来の
		// 固定名）が入っている。
		const core::ImportOptions& options() const
		{
			return fOptions;
		}

		// FL ストーリの一覧（Elevation 昇順・末尾が最上階）。parse/Story の collectStories。
		const std::vector<StoryInfo>& stories();

		// 通り芯の線分（重複除去済み・#id 昇順で決定的）。parse/Grid の collectGridLines。
		const std::vector<GridLine>& gridLines();

		// 全要素に共通のセンタリングオフセット（通り芯の bbox 中心）。通り芯が 1 本も
		// 取れなければ (0,0)＝補正しない。
		const core::Vec2& gridCenter();

		// 階（#storeyId）に属する要素の #id。parse/Story の collectStoryElements。
		const std::vector<int>& storyElements(int storeyId);

		// 屋根階（#storeyId）の床梁が囲む領域から合成したロフト床。parse/Floor の
		// loftFloorRegions（セル格子の flood fill を伴うため、特に重複計算を避けたい）。
		const std::vector<LoftFloorRegion>& loftFloorRegions(int storeyId);

		// 屋根版（#elementId）の屋根面。解決できない屋根版は nullptr。垂木（parse/Rafter）と
		// 野地板（parse/Roof）が同じ面を共有するので、押し出しソリッドの解決は 1 回で済む。
		const RoofPlane* roofPlane(int elementId);

		// 階（#storeyId）の屋根版から解決できた屋根面の一覧（要素 #id 順＝決定的）。
		// 「階の要素 → 屋根版判定（isRoofSlab）→ roofPlane → 解決できないものはスキップ」
		// という走査は垂木・野地板・登り梁の 3 者が同一で、**この走査はここに 1 つだけ置く**
		// （判定・関門が三者でズレて拾う面が食い違うのを防ぐ）。返る指す先はキャッシュ
		// （fRoofPlanes。std::map なので要素追加で無効化されない）。
		std::vector<const RoofPlane*> storyRoofPlanes(int storeyId);

		// 横架材の命令（parse/Member の buildMemberCommands。**登り梁の補正前**）。3 者が
		// この 1 回の解析結果を共有する: ストーリ（母屋・登り梁レイヤを作るか）・垂木（差し込みに
		// 使う軒桁の桁幅）・登り梁の位置補正（受ける材）。全 IfcBeam の配置・断面・材種を辿る
		// 重い解析なので、要素ごとに組み立て直すと同じ走査が 3 回走る。
		//
		// **補正前**なのは、この値を使う 2 者（ストーリ・登り梁の補正）が補正前を要するから:
		// ストーリは配置先レイヤ名しか見ず、補正はレイヤを変えない。登り梁の補正自体はこれを
		// 入力に取る。垂木だけは補正後を渡したいので、parse/BuildDocument が補正結果を明示的に
		// 渡す（buildRafterCommands のオーバーロード）。
		const std::vector<core::MemberCommand>& members();

		// 柱の命令（parse/Column の buildColumnCommands）。3 者がこの 1 回の解析結果を
		// 共有する: ストーリ（span 柱レイヤのレベルを作る）・Document の columns・登り梁の
		// 端部詰め（受ける柱）。柱の span 判定は上階の横架材下端を要するので、この計算は
		// members（上記）を入力に取る——要素ごとに組み立て直すと横架材の解析まで巻き添えで
		// 何度も走る。
		const std::vector<core::ColumnCommand>& columns();

		// 基礎の立上りの命令（parse/Footing の buildWallCommands）。2 者がこの 1 回の解析結果を
		// 共有する: Document の walls と、底盤の外面合わせ（辺に沿う立上りの半壁厚）。立上りの
		// 自由端は柱芯へ寄せるので、この計算は columns（上記）を入力に取る。
		const std::vector<core::WallCommand>& walls();

		// アンカーボルトの命令（parse/AnchorBolt の buildAnchorBoltCommands）。2 者がこの
		// 1 回の解析結果を共有する: Document の anchorBolts と、基礎伏図のグラフィック凡例
		// （**アンカーボルトを 1 本も置かないなら凡例も出さない**という判断だけに使う。
		// parse/Sheet）。全 IfcMechanicalFastener を辿るので、要素ごとに組み立て直すと
		// 同じ走査が 2 回走る。
		const std::vector<core::SymbolCommand>& anchorBolts();

	private:
		const Model* fModel = nullptr;
		core::ImportOptions fOptions;

		std::optional<std::vector<StoryInfo>> fStories;
		std::optional<std::vector<GridLine>> fGridLines;
		std::optional<core::Vec2> fGridCenter;
		std::map<int, std::vector<int>> fStoryElements;
		std::map<int, std::vector<LoftFloorRegion>> fLoftFloorRegions;
		std::map<int, std::optional<RoofPlane>> fRoofPlanes;
		std::optional<std::vector<core::MemberCommand>> fMembers;
		std::optional<std::vector<core::ColumnCommand>> fColumns;
		std::optional<std::vector<core::WallCommand>> fWalls;
		std::optional<std::vector<core::SymbolCommand>> fAnchorBolts;
	};
} // namespace HomeskzIfcImport::parse
