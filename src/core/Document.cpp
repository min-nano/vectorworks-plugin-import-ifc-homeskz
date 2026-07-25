//
//	core/Document.cpp
//
//	validateDocument の実装。Python 版 document.py の validateDocument に対応する。
//	SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//
//	骨組みの現状では Document は「バージョン＋空の器」なので、検証はバージョンの
//	妥当性だけを見る。各命令リスト（grids / stories / members …）が追加されるたびに、
//	対応する検証規則（必須フィールドの有無・参照整合性・値域）をここへ足していく。
//

#include "core/Document.h"

namespace HomeskzIfcImport::core
{
	bool validateDocument(const Document& document)
	{
		// TODO: 命令リストが増えたら要素ごとの検証をここに積む（ROADMAP.md）。
		return document.version == kDocumentVersion;
	}
} // namespace HomeskzIfcImport::core
