//
//	ExtMenu.h
//
//	A minimal Vectorworks menu command extension. When the user runs the
//	command it pops up an alert announcing that the plug-in has started.
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
