//
//	parse/BuildDocument.cpp
//
//	buildDocument の骨組み実装。Python 版 ifc/__init__.py build_document に対応。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	現状は空の Document を返すだけの器。以降のマイルストーンで、
//	  1. parse/Loader … サニタイズ込みで IFC を読み込む（IFCFOOTINGTYPE 除去等）
//	  2. parse/Step   … STEP トークナイズ＋エンティティグラフを構築する
//	  3. parse/Grid … parse/Section … 要素ごとに Document を組み立てる
//	という順で肉付けしていく（ROADMAP.md）。
//

#include "parse/BuildDocument.h"
#include "parse/Grid.h"
#include "parse/Loader.h"

namespace HomeskzIfcImport::parse
{
	core::Document buildDocument(const std::string& ifcPath)
	{
		// Phase 1 の入口: Loader で読み込み・サニタイズし、最小 STEP リーダで
		// エンティティグラフ（Model）を構築する。読み込み失敗（存在しない・空）でも
		// 例外を漏らさず、空の Model として先へ進む（1 要素の欠損で全体を止めない）。
		Model const model = loadIfc(ifcPath);

		core::Document document;

		// M1 通り芯: IfcGridAxis を解析して GridCommand を積む（parse/Grid）。
		// 以降のマイルストーンで Story / Member … の解析を同様に足していく（ROADMAP.md）。
		document.grids = buildGridCommands(model);

		return document;
	}
} // namespace HomeskzIfcImport::parse
