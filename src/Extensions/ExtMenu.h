//
//	ExtMenu.h
//
//	A minimal Vectorworks menu command extension. When the user runs the
//	command it pops up an alert announcing that the plug-in has started.
//

#pragma once

#include "VectorworksSDK.h"

namespace SamplePlugin
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// The code that actually runs when the menu command is picked.
	class CSampleMenu_EventSink : public VWMenu_EventSink
	{
	public:
						CSampleMenu_EventSink(IVWUnknown* parent);
		virtual			~CSampleMenu_EventSink();

		virtual void	DoInterface() override;
	};

	// ------------------------------------------------------------------------
	// The menu command extension itself.
	class CExtMenuSample : public VWExtensionMenu
	{
		DEFINE_VWMenuExtension;
	public:
						CExtMenuSample(CallBackPtr cbp);
		virtual			~CExtMenuSample();
	};
}
