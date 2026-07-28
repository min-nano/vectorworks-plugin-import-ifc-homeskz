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

		// メニュー項目を実行したときの本体（ファイル選択→解析→描画）。
		void DoInterface() override;

		// メニュー項目の有効／無効（グレーアウト）を毎回の表示前に問い合わせる
		// フック。ドキュメントが開いていないときは false を返してコマンドを
		// グレーアウトさせる。本プラグインは開いているドキュメントへ描画するため、
		// 文書が無い状態では実行させない（DoInterface 内の gSDK 描画呼び出しが
		// アクティブレイヤ前提で、文書が無いと無意味・不安定になるのを防ぐ）。
		bool GetItemEnabled() override;
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
