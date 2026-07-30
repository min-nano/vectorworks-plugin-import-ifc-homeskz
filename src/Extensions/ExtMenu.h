//
//	ExtMenu.h
//
//	The plug-in's "IFC インポート" menu command extension. Running it asks for a
//	Homes-kun IFC file, parses it into the instruction set (Phase 1, core/parse)
//	and draws it into the document (Phase 2, draw/), then reports what was drawn
//	in an alert. See ExtMenu.cpp for the flow.
//

#pragma once

#include "VectorworksSDK.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// The code that actually runs when the menu command is picked.
	class CImportIfcMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CImportIfcMenu_EventSink(IVWUnknown* parent);
		~CImportIfcMenu_EventSink() override;

		// メニュー項目を実行したときの本体（ファイル選択→解析→描画）。
		//
		// 文書が開いていないときのグレーアウトは menuDef() の Needs =
		// EMenuEnableFlags::DocIsActive（ExtMenu.cpp）で宣言的に行うため、
		// GetItemEnabled() の override は持たない（基底の常に true に委ねる）。
		void DoInterface() override;
	};

	// ------------------------------------------------------------------------
	// The menu command extension itself.
	class CExtMenuImportIfc : public VWExtensionMenu
	{
		DEFINE_VWMenuExtension;

	public:
		CExtMenuImportIfc(CallBackPtr cbp);
		~CExtMenuImportIfc() override;
	};
} // namespace HomeskzIfcImport
