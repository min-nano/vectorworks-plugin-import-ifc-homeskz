//
//	parse/Floor.h
//
//	Phase 1（IFC 解析）の床板モジュール。ホームズ君 IFC の床板（Name が "床版" の IfcSlab。
//	鉛直押し出しで、押し出しプロファイルがそのまま床の平面外形になる）を各階の FL
//	レイヤ（"n-FL"）へ描くための命令（core::FloorCommand）に変換する（描画オブジェクトはスラブ。
//	draw/Floor.h 参照）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）だけで
//	完結し、通常の C++ ツールチェインでコンパイル・単体テストできる。
//
//	要件（docs/DEV-NOTES.md M5）:
//	  * 床のある場所は IFC から抽出する（床版の平面外形をそのまま床の外形にする）。
//	  * 高さは **IFC の床位置を尊重する**（段差＝スキップフロアを表現する）。命令が持つ
//	    高さは**床仕上げ上端**の絶対 Z で、一般部は FL と同じ、部分的に床レベルを指定して
//	    いる場合は FL ± 差分になる。高さ基準は配置先ストーリの「FL」レベルにバインドし、
//	    その差分を bound.offset に入れる（一般部は offset 0、段差床はここがずれる）。
//	  * スラブ構成（上から）は 床仕上げ＝FL 高さ − 横架材天端高さ − 24、床下地＝24mm 固定。
//	    合計＝FL 高さ − 横架材天端高さなので、スラブ下端は一般部で横架材天端に一致する。
//	    IFC の押し出し厚（実際には 28mm 等が出力される）は使わない。
//	  * **屋根階（最上階）の床＝ロフト（小屋裏収納）**も取り込む。ロフトの標準床レベルは
//	    「軒高 + 36mm」と仮定し（IFC に無いため）、スラブ構成は 床仕上げ 12 ＋ 床下地 24。
//	    高さ基準は一般階が床仕上げ上端（FL レベル）、ロフトは床下地下端（軒高レベル＝
//	    横架材天端。ロフトの FL は仮定値なので確かな構造面を基準にする）。屋根階の FL
//	    レベル／レイヤ（"R-FL"）は、ロフトの床があるときに限り parse/Story が作る。
//	  * **ロフトの外形は床梁から合成する**。ホームズ君は小屋伏図に床梁（床大梁・床小梁）を
//	    出力する一方、ロフトの**床版は出力しない**（実 IFC フィクスチャ 5 件すべてで屋根階に
//	    "床版" が無いことを確認済み。tests/fixtures/README.md）。そこで屋根階に床版が無い
//	    ときに限り、屋根階の床梁が囲む領域（core::filledUnionOutlines）を床の外形と
//	    みなす。根太が渡っていない——どの空隙も
//	    囲っていない——単独の床梁は床にならない。あくまで床版の代替なので、屋根階に床版が
//	    あればそちらを優先する。
//
//	床は「建物形状の一次情報」で、以降の横架材・柱はこの位置に合わせて載せていく
//	（docs/DEV-NOTES.md「形状を先に確定し、支持部材を合わせる」）。
//

#pragma once

#include "core/Document.h"
#include "parse/Step.h"

#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 床板を識別する IfcSlab の Name。
	inline constexpr const char* kFloorSlabName = "床版";

	// 床下地の厚み（mm）。要件により 24mm 固定。IFC の押し出し厚は使わない（ホームズ君は
	// 28mm 等を出力するが、作図上は 24mm で統一する）。
	inline constexpr double kSubfloorThickness = 24.0;

	// スラブ構成層の名前。上から 床仕上げ → 床下地。各層のクラス（素材）は
	// parse/StructuralClass.h の CLASS_COMPONENT_*（床仕上げ＝フローリング、
	// 床下地＝合板）。
	inline constexpr const char* kFloorFinishName = "床仕上げ";
	inline constexpr const char* kSubfloorName = "床下地";

	// ロフト（屋根階の床＝小屋裏収納）の標準床レベル。軒高からの高さ（mm）で、**仮定値**。
	// ホームズ君 IFC はロフトの FL を持たないため、軒高（＝横架材天端＝床下地下端）から
	// この高さを床仕上げ上端とみなす。スラブ構成は 床仕上げ（36−24＝12）＋ 床下地（24）。
	inline constexpr double kLoftFloorLevelOffset = 36.0;

	// 要素が床板（IfcSlab かつ Name が "床版"）か。
	bool isFloorSlab(const Entity& element);

	// 階（#storeyId）が床板（Name が "床版" の IfcSlab）を含むか。
	bool storyHasFloorSlab(const Model& model, int storeyId);

	// 同上。共有コンテキストの要素一覧を使う（parse/Context.h）。
	bool storyHasFloorSlab(Context& context, int storeyId);

	// 床梁から合成したロフト床 1 枚（ヘッダ冒頭「ロフトの外形は床梁から合成する」参照）。
	struct LoftFloorRegion
	{
		// 平面外形（IFC の生座標。通り芯センタリングは呼び出し側で行う）。
		std::vector<core::Vec2> boundary;
		// 床下地下端＝床梁天端の、ストーリ原点（軒高）からの相対 Z。段差が無ければ 0。
		double beamTopOffset = 0.0;
	};

	// 屋根階（#storeyId）の床梁が囲む領域からロフト床を合成する。床梁は横架材クラスが
	// 床梁（床大梁・床小梁・甲乙梁）の IfcBeam / IfcMember。押し出しを解決できない部材は
	// 飛ばす。囲まれた領域が無ければ空を返す。
	//
	// 並び・値はエンティティ列挙順に依存しない（外形はセル走査順、天端は床梁天端の最大値）。
	std::vector<LoftFloorRegion> loftFloorRegions(const Model& model, int storeyId);

	// 同上。共有コンテキストの要素一覧を使う（parse/Context.h）。**結果をキャッシュしたい
	// ときは Context::loftFloorRegions を呼ぶこと**（この関数は毎回セル格子の flood fill を
	// やり直す。ストーリのレベル追加と床の合成で 2 回走っていたのがキャッシュ導入の動機）。
	std::vector<LoftFloorRegion> loftFloorRegions(Context& context, int storeyId);

	// 屋根階（#storeyId）にロフトの床（床版、または床梁から合成できる領域）があるか。
	// あるときだけ屋根ストーリへ FL レベル（軒高 + kLoftFloorLevelOffset）を足すために
	// parse/Story が使う。
	bool storyHasLoftFloor(const Model& model, int storeyId);

	// 同上。共有コンテキストのキャッシュ済みロフト床を使う（parse/Context.h）。
	bool storyHasLoftFloor(Context& context, int storeyId);

	// STEP Model から床板の描画命令を組み立てる。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、各階について
	// その階に属する床版（Name=="床版" の IfcSlab）を FloorCommand にする（最上階も対象で、
	// その床はロフト＝小屋裏収納の床として "R-FL" へ・基準面は軒高になる。上記要件参照）。
	// 屋根階に床版が無いときは、代わりに床梁から合成した外形（loftFloorRegions）を使う。
	// 平面外形は通り芯と同じグリッド中心オフセット（parse/Context の gridCenter）で
	// 補正する。押し出しソリッドを解決できない床版はスキップする（1 枚の欠損で全体を
	// 止めない。CLAUDE.md「エラーハンドリング」）。
	//
	// 並びは階（Elevation 昇順）→ 階内は要素の出現順（IfcRelContainedInSpatialStructure の
	// #id 昇順・記述順）で、エンティティ列挙順に依存しない決定的な結果になる。
	std::vector<core::FloorCommand> buildFloorCommands(const Model& model);

	// 同上。共有コンテキストのストーリ一覧・センタリング中心・ロフト床を使う（parse/Context.h）。
	std::vector<core::FloorCommand> buildFloorCommands(Context& context);
} // namespace HomeskzIfcImport::parse
