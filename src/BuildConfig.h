//
//	BuildConfig.h
//
//	Central place for the plug-in's build-time identity. The exact same source
//	code is compiled into two coexisting plug-ins:
//
//	  * the STABLE plug-in ("HomeskzIfcImport"),    built from the `main` branch, and
//	  * the DEV plug-in    ("HomeskzIfcImportDev"), built from feature / PR branches.
//
//	They must have DIFFERENT identifiers (bundle name, .vwr identifier, VCOM
//	universal name and extension UUID) so Vectorworks can load BOTH at the same
//	time without them colliding. The DEV build is selected by defining
//	VW_DEV_BUILD (done per-target in CMakeLists.txt); everything else here is
//	derived from that single switch. Only identifiers that the C++ code actually
//	uses live here — see the note on the build channel below.
//

#pragma once

#ifdef VW_DEV_BUILD
// Dev plug-in identity.
#	define PLUGIN_VWR_ID "HomeskzIfcImportDev"
#	define PLUGIN_UNIVERSAL_NAME "CExtMenuImportIfc_HomeskzIfcImportDev"
#else
// Stable plug-in identity.
#	define PLUGIN_VWR_ID "HomeskzIfcImport"
#	define PLUGIN_UNIVERSAL_NAME "CExtMenuImportIfc_HomeskzIfcImport"
#endif

// NB: there is deliberately no build-channel macro here. The channel a build
// belongs to ("stable" / "dev") is stamped into the packaged build by CMake —
// the mac Info.plist's VWBuildChannel key, filled from the add_vw_plugin
// argument — and that is the only place anything reads it (the updater scripts).
// The C++ side identifies itself by branch + commit (below), so a macro would be
// a third spelling of the same fact, and it would collide in name with CMake's
// VW_BUILD_CHANNEL option, which has a DIFFERENT domain ("stable"/"dev"/"both").

// Short identifier of the exact build (git commit, or "local" for a local
// build). CMake passes this in via -DVW_BUILD_VERSION=...; it is stamped into
// each build (mac: the bundle's Info.plist, win: a "<name>.commit" sidecar) so
// the updater scripts can compare the installed build against the published
// one, and it is compiled in here so the dev build picker can name the build
// that is actually loaded (see Updater.cpp RunDevStartupCheck).
#ifndef VW_BUILD_VERSION
#	define VW_BUILD_VERSION "local"
#endif

// Git branch the build came from ("main" for stable, the feature/PR branch for
// a dev build, or "local" for a local build). CMake passes this in via
// -DVW_BUILD_BRANCH=...; like the commit it is stamped into the Info.plist and
// shown in the dev build picker at start-up, so a dev build can be traced to
// its branch.
#ifndef VW_BUILD_BRANCH
#	define VW_BUILD_BRANCH "local"
#endif
