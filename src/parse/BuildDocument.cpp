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

#include "parse/BuildDocument.h"
#include "parse/Floor.h"
#include "parse/Grid.h"
#include "parse/Loader.h"
#include "parse/Story.h"

namespace HomeskzIfcImport::parse
{
	core::Document buildDocument(const std::string& ifcPath)
	{
		// Phase 1 の入口: Loader でファイルを読み、最小 STEP リーダで
		// エンティティグラフ（Model）を構築する。読み込み失敗（存在しない・空）でも
		// 例外を漏らさず、空の Model として先へ進む（1 要素の欠損で全体を止めない）。
		Model const model = loadIfc(ifcPath);

		core::Document document;

		// M3 ストーリ: IfcBuildingStorey を解析して StoryCommand を積む（parse/Story）。
		// 以降の要素はここで作られたレベルへ高さをバインドするため、grids より先に置く。
		document.stories = buildStoryCommands(model);

		// M1 通り芯: IfcGridAxis を解析して GridCommand を積む（parse/Grid）。
		document.grids = buildGridCommands(model);

		// M5 床板: 床版（IfcSlab "床版"）を解析して FloorCommand を積む（parse/Floor）。
		// 床は建物形状の一次情報で、以降の横架材・柱はこの位置に合わせる（形状先行）。
		// 以降のマイルストーンで Rafter / Member … の解析を同様に足していく（ROADMAP.md）。
		document.floors = buildFloorCommands(model);

		return document;
	}
} // namespace HomeskzIfcImport::parse
