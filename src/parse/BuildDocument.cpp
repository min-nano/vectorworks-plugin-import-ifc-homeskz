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

namespace HomeskzIfcImport::parse
{
	core::Document buildDocument(const std::string& ifcPath)
	{
		// TODO: Loader で読み込み → Step でグラフ化 → 要素ごとに解析して Document へ。
		(void)ifcPath; // 骨組み段階では未使用（各 parse モジュール導入で使う）。
		return core::Document{};
	}
} // namespace HomeskzIfcImport::parse
