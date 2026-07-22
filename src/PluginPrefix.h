//
//	PluginPrefix.h
//
//	Shared prefix header for the plug-in. Pulls in the whole Vectorworks SDK
//	and opens the namespaces the SDK examples rely on. Every implementation
//	file includes this first.
//

#pragma once

#ifdef _WINDOWS
#	include <Windows.h>
#endif

// Main Vectorworks SDK umbrella header. The platform (GS_MAC / GS_WIN) is
// auto-detected from __APPLE__ / _WINDOWS, so we must NOT define _WINDOWS on mac.
#include "VectorworksSDK.h"

using namespace VWFC::Math;
using namespace VWFC::VWObjects;
using namespace VWFC::Tools;
using namespace VWFC::VWUI;
