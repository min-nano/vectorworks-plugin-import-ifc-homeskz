//
//	ExtMenuDumpAux.h
//
//	**dev ビルド専用の調査コマンド**「診断: 選択図形の内部構造をダンプ」。地中梁を底盤へ
//	噛み合わせる方法を探るための一時的な道具で、stable ビルドには入らない
//	（ModuleMain.cpp の登録が `#ifdef VW_DEV_BUILD` に囲まれている）。
//
//	【なぜ要るか】UI の「3D オブジェクトとスラブを噛み合わせる」で作った図形と、本プラグインが
//	作る削り取りモディファイアは、VectorScript から見える範囲（型・クラス・レコード・
//	プロファイル群）が同一で、違いは**押し出しに付く補助オブジェクト 1 つ**（type=114）
//	だけだった（ROADMAP.md M10）。補助オブジェクトの**データタグ**は VectorScript からは
//	読めず、SDK の `GetDataTag` でしか見えないため、この差を突き止めるにはプラグイン側から
//	覗く必要がある。
//
//	【使い方】比べたい図形（UI で噛み合わせたスラブ／本プラグインが作った底盤）を選択して
//	実行すると、選択図形ごとに
//	  * 型番号（GetObjectTypeN）とデータタグ（GetDataTag。データオブジェクトのみ意味を持つ）
//	  * 補助オブジェクト（FirstAuxObject → 以降）の型番号とデータタグ
//	  * プロファイル群（GetCustomObjectProfileGroup）の中身と、その各要素の補助オブジェクト
//	をテキストファイルへ書き出す（デスクトップ）。
//
//	【消すとき】噛み合わせの実装方針が決まったら**このファイルごと削除する**
//	（ModuleMain.cpp の登録と resources/*Dev.vwr の文字列も一緒に）。
//

#pragma once

#include "VectorworksSDK.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// メニュー項目を実行したときの本体（選択図形の内部構造をファイルへ書き出す）。
	class CDumpAuxMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CDumpAuxMenu_EventSink(IVWUnknown* parent);
		~CDumpAuxMenu_EventSink() override;

		void DoInterface() override;
	};

	// 調査コマンドの拡張本体。
	class CExtMenuDumpAux : public VWExtensionMenu
	{
		DEFINE_VWMenuExtension;

	public:
		CExtMenuDumpAux(CallBackPtr cbp);
		~CExtMenuDumpAux() override;
	};
} // namespace HomeskzIfcImport
