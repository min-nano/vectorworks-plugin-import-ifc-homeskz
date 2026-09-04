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

// **殻の ID**——「アップデートに Vectorworks の再起動が要るか」を決める鍵。
//
// プラグインは 2 つに割れている（src/PayloadAbi.h）: Vectorworks が起動時にしか読み込め
// ない**殻**（このモジュール）と、殻が自分で読み込む**本体**（`<name>.vwpayload`）。本体
// だけが新しくなったのなら次の操作で読み直されるので再起動は要らず、殻まで変わっていれば
// 要る。CMake が「殻に入るものだけ」のハッシュを -DVW_SHELL_ID で渡し、インストール済みの
// ビルドにも同じ値が控えられる（mac: Info.plist の VWShellId、win: `<name>.shell-id`）ので、
// 更新の直後に両者を突き合わせられる（src/UpdaterParse.h の NeedsRestartAfterInstall）。
//
// 既定の "local" は保険にすぎない——プラグインのビルドでは CMake が必ず実際のハッシュを
// 渡すので（CMakeLists.txt の VW_SHELL_INPUTS）、配布物がこの値を名乗ることはない。
#ifndef VW_SHELL_ID
#	define VW_SHELL_ID "local"
#endif
