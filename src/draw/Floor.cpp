//
//	draw/Floor.cpp
//
//	床板描画の実装。Python 版 vw/floor.py に対応する。命令セット（FloorCommand）を
//	床ツール（Floor オブジェクト）として配置する。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド（SDK あり）
//	でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	【床だけ VectorScript を経由する理由】VW 2026 の ISDK には**床ツール（Floor
//	オブジェクト）を生成する API が無い**（`CreateRoof` / `CreateSlab` はあるが Floor は
//	`Kernel` の種別定数 `kFloorSubT` があるだけ。`BeginFloor` / `LNewObj` / `Move3D` と
//	いった VectorScript 相当の呼び出しも ISDK には存在しない）。押し出しソリッドや
//	スラブで代替すると Python 版と描かれるオブジェクト種別が変わってしまうため、
//	SDK が公式に提供するスクリプトエンジン
//	（`VectorWorks::Scripting::IVectorScriptEngine::ExecuteScript`）で床ツールを呼ぶ。
//	スクリプト本文の組み立ては SDK 非依存の core::buildFloorScript が担い（無 SDK で
//	単体テスト済み）、ここはレイヤの用意とスクリプト実行だけを行う。
//
//	実描画（Move3D の絶対 Z・厚みの伸びる向き・SetObjectStoryBound のアンカー）は
//	ローカルの VectorWorks で目視確認する方針（ROADMAP.md M5「ローカル確認」）。
//	床下端＝IFC の床位置になること、段差床が offset ぶんずれることを実機で確かめる。
//

#include "PluginPrefix.h"
#include "draw/Floor.h"
#include "core/Document.h"
#include "core/FloorScript.h"

#include "Interfaces/VectorWorks/Scripting/IVectorScriptEngine.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	std::size_t drawFloors(const core::Document& document)
	{
		if (document.floors.empty())
			return 0;

		// VectorScript エンジン（VCOM シングルトン）。取得できなければ床は描けない
		// （1 要素の欠損で全体を止めない＝静かに 0 枚で返る）。
		using VectorWorks::Scripting::IID_VectorScriptEngine;
		using VectorWorks::Scripting::IVectorScriptEngine;
		VCOMPtr<IVectorScriptEngine> engine(IID_VectorScriptEngine);
		if (!engine)
			return 0;

		std::size_t drawn = 0;
		for (const core::FloorCommand& floor : document.floors)
		{
			// 配置先レイヤ（"n-FL"）が無い命令はスキップする。レイヤは story 命令が作る
			// ので、無い＝そのストーリの生成がスキップされたということ。床のために勝手に
			// レイヤを作らない（Python 版 execute_floors と同じ規約）。
			MCObjectHandle layer = gSDK->GetNamedLayer(TXString(floor.layer.c_str()));
			if (layer == nil)
				continue;
			gSDK->SetCurrentLayer(layer);

			// アクティブレイヤに床 1 枚を描くスクリプトを実行する。
			const std::string script = core::buildFloorScript(floor);
			if (engine->ExecuteScript(TXString(script.c_str())) == kVCOMError_NoError)
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
