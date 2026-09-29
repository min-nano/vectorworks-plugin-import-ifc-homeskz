//
//	draw/Dimension.cpp
//
//	寸法・レベル記号の描画（意図と作法は draw/Dimension.h）。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するので、プラグインビルド（SDK あり）でのみコンパイルする。
//
//	使う SDK API:
//	  * gSDK->CreateLinearDimension(p1, p2, startOffset, 0, (0,0), dimType) … 直線寸法
//	  * gSDK->SetObjectVariable(h, ovDimStandardName, 名前)                … 寸法規格
//	  * gSDK->CreateChainDimension(h1, h2)                                  … 連続寸法へ繋ぐ
//	  * gSDK->AddViewportAnnotationObject(viewport, h)                      … 注釈へ移す
//	  * gSDK->CreateCustomObject("Elevation Benchmark2", 点, 0, true)       … レベル基準線
//	  * VWParametricObj::SetPointObjectPos / SetParamString                 … 位置・Axis
//	  * gSDK->GetCustomObjectProfileGroup / SetCustomObjectProfileGroup     … マーカーレイアウト
//	  * gSDK->CreateTextBlock / AddObjectToContainer                        … 名前のテキスト
//

#include "PluginPrefix.h"
#include "draw/Dimension.h"
#include "draw/DrawUtil.h"
#include "draw/Verify.h"
#include "core/Document.h"
#include "core/Layout.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 直線寸法の種類（dimType ＝ ovDimClass）。0 は fix_ang（角度固定）。**水平／垂直は
		// dimType ではなく 2 点の取り方で決まる**（Findings「Dimensions」）ので、測点を
		// 水平・垂直に並べて渡せばよい。
		constexpr short kDimTypeFixedAngle = 0;

		// 縮尺が決まらなかった（用紙が読めない等）ときに寸法線までの距離を測る縮尺の分母。
		// 取り込みの既定の縮尺（1/100）と同じ。
		constexpr double kFallbackScale = 100.0;

		// レベル基準線の universal 名（ローカライズ名「レベル基準線」。Findings
		// 「Level Objects」の実測表）。
		constexpr const char* kLevelMarkPlugin = "Elevation Benchmark2";

		// ストーリレベルへ結ぶ 3 つ組（Findings「Level Objects」）。**3 つ揃って初めて効く**
		// （Datum 単独では GroundPlane へ倒される）。Axis は既定の ZAxis3DMode のまま触らない
		// ——YAxis2DMode にすると関連付けが外れる。結べば高さはストーリレベルから来るので、
		// 記号をドラッグしても数値は動かない（round 3 のご指摘）。
		constexpr const char* kParamStoryName = "__StoryName";
		constexpr const char* kParamLevelTypeName = "__LevelTypeName";
		constexpr const char* kParamDatum = "Datum";
		constexpr const char* kDatumStoryLevel = "StoryLevel";

		// 描いた高さ（読み取り用の静的文字。Findings「Level Objects」のパラメータ表）。
		constexpr const char* kParamShownElevation = "Elevation";

		// 描いた高さが命令の高さからこれ以上ずれていたら「合わない」と数える（mm）。
		constexpr double kLevelHeightTol = 0.5;

		// マーカーレイアウトの中で「名前」を出しているテキストの目印（ストーリレベル名の
		// 動的テキスト 〈#STLT#-#STPS#〉。Findings「Level Objects」の実測）。このテキストを
		// 固定の名前に差し替える。
		constexpr const char* kLevelNameToken = "#STLT#";

		// 測点 a → b の直線寸法を 1 本作る。axis=Horizontal なら (a, base)→(b, base) を、
		// Vertical なら (base, a)→(base, b) を測る。offset は根元から寸法線までの距離
		// （符号は図面の座標軸で + が上／右。draw/Dimension.h）。
		MCObjectHandle CreateOne(core::DimensionAxis axis, double a, double b, double base,
								 double offset)
		{
			const bool horizontal = axis == core::DimensionAxis::Horizontal;
			const WorldPt p1 = horizontal ? WorldPt(a, base) : WorldPt(base, a);
			const WorldPt p2 = horizontal ? WorldPt(b, base) : WorldPt(base, b);
			return gSDK->CreateLinearDimension(p1, p2, offset, 0.0, Vector2(0.0, 0.0),
											   kDimTypeFixedAngle);
		}

		// 仕上がった寸法（連続寸法または繋げなかった直線寸法）を注釈へ移す。移せなければ
		// 消す（アクティブレイヤに寸法だけが浮かないように）。
		bool Annotate(MCObjectHandle viewport, MCObjectHandle object)
		{
			if (gSDK->AddViewportAnnotationObject(viewport, object))
				return true;
			gSDK->DeleteObject(object, true);
			return false;
		}

#if VW_DRAW_VERIFY
		// 検算（dev だけ）: 寸法 1 本の見え方に効くオブジェクト変数を読んで 1 行にする
		// （round 1: 軸組図だけ寸法値が出なかった。伏図と並べて違いを探す）。
		std::string DescribeDimension(MCObjectHandle dimension)
		{
			const auto real = [dimension](short selector) -> std::string
			{
				TVariableBlock value;
				Real64 number = 0.0;
				if (gSDK->GetObjectVariable(dimension, selector, value) == 0 ||
					!value.GetReal64(number))
					return "?";
				std::array<char, 32> buffer{};
				std::snprintf(buffer.data(), buffer.size(), "%g", number);
				return buffer.data();
			};
			const auto flag = [dimension](short selector) -> std::string
			{
				TVariableBlock value;
				bool on = false;
				if (gSDK->GetObjectVariable(dimension, selector, value) == 0 ||
					!value.GetBoolean(on))
					return "?";
				return on ? "on" : "off";
			};
			std::string name = "?";
			if (TVariableBlock value;
				gSDK->GetObjectVariable(dimension, ovDimStandardName, value) != 0)
			{
				TXString text;
				if (value.GetTXString(text))
					name = static_cast<const char*>(text);
			}
			std::string style = "クラス";
			if (!gSDK->GetTextStyleByClass(dimension))
			{
				TXString styleName;
				gSDK->InternalIndexToNameN(gSDK->GetTextStyleRef(dimension), styleName);
				style = static_cast<const char*>(styleName);
			}
			return "値表示 " + flag(ovDimShowValue) + " / 文字 " + real(ovDimTextSizeInPoints) +
				   "pt・" + real(ovDimFontSize) + "mm / 文字スタイル " + style + " / 規格 " + name;
		}
#endif

		// 寸法の文字の当て方（ビューポート 1 枚ぶん。drawViewportDimensions が決める）。
		//   style    … 寸法規格の文字スタイル（ref number。0 なら当てない）
		//   fontSize … 文字の図面上の大きさ（mm）＝ 文字スタイルの紙の pt × 25.4/72 ×
		//              ビューポートの縮尺（0 なら書かない）
		struct DimensionText
		{
			InternalIndex style = 0;
			double fontSize = 0.0;
		};

		// 列 1 本を置く。1 本でも注釈へ置けたら true。scale は寸法線までの距離に使う縮尺の
		// 分母。
		bool PlaceChain(MCObjectHandle viewport, const core::DimensionChainCommand& chain,
						const std::string& standard, double scale, const DimensionText& text,
						DimensionCounts& counts)
		{
			const double line = core::dimensionLineCoord(chain.base, chain.side, chain.tier, scale);
			const double offset = line - chain.base;

			// 繋ぎかけの連続寸法（または 1 本目の直線寸法）と、その中の寸法の本数。
			MCObjectHandle current = nil;
			std::size_t inCurrent = 0;
			std::size_t placed = 0;
			const auto flush = [&]()
			{
				if (current == nil)
					return;
				if (Annotate(viewport, current))
					placed += inCurrent;
				else
					counts.failed += inCurrent;
				current = nil;
				inCurrent = 0;
			};

			for (std::size_t i = 0; i + 1 < chain.stops.size(); ++i)
			{
				const MCObjectHandle dimension =
					CreateOne(chain.axis, chain.stops[i], chain.stops[i + 1], chain.base, offset);
				if (dimension == nil)
				{
					// 1 本作れなければそこで列が切れる（前の区間までを先に仕上げる）。
					++counts.failed;
					flush();
					continue;
				}
				// **規格は繋ぐ前に当てる**（連続寸法そのものには効かない）。図面に無い
				// 名前は false で弾かれ、文書の既定の規格のまま残る（数えて診断へ）。
				if (!SetTextVariable(dimension, ObjectVariable::DimStandardName, standard))
					++counts.standardRejected;
				// 寸法値は**明示して出す**（round 1: 軸組図の注釈で値が出なかった。伏図では出た）。
				SetBooleanVariable(dimension, ObjectVariable::DimShowValue, true);
				// **文字スタイルを明示し、文字の大きさをビューポートの縮尺で書いて引き直す**
				// （Findings「Dimensions」#157 / #161）。
				//   * 注釈に置く寸法の文字の大きさは、作るとき（SetTextStyleRef を呼ぶとき）の
				//     アクティブレイヤの縮尺で焼き付く。軸組図はシートレイヤ（1:1）がアクティブ
				//     なので、そのままでは 1/125 の図の上で紙 0.02mm になり値が見えなかった
				//     （round 1〜3。OIP で文字スタイルを選び直すと出たのは、注釈の縮尺で焼き直す
				//     ため）。
				//   * 大きさ（ovDimFontSize）は**書いただけでは絵に出ない**——ResetObject で
				//     引き直す。読み戻しは書いた値を返すので、呼び忘れても気付けない。
				//   * **繋ぐ前に**済ませる。文字スタイルが明示してあれば、繋いで作り直された
				//     中の直線寸法も書いた大きさを保つ（〈クラスの文字スタイル〉のままだと、
				//     繋ぐときのアクティブレイヤの縮尺で焼き直される。#155）。
				//   * 文字スタイルは寸法規格のもの（「寸法(6pt)」）をそのまま使い、縮尺ごとの
				//     文字スタイルは作らない（名前付きリソースを増やさない。docs/DEV-NOTES.md M31）。
				if (text.style != 0)
				{
					gSDK->SetTextStyleRef(dimension, text.style);
					if (text.fontSize > 0.0)
						SetRealVariable(dimension, ObjectVariable::DimFontSize, text.fontSize);
					else
						++counts.textSizeUnread;
					gSDK->ResetObject(dimension);
				}
				else
					++counts.textStyleMissing;
#if VW_DRAW_VERIFY
				if (counts.dimensionProbe.empty())
					counts.dimensionProbe = DescribeDimension(dimension);
#endif
				SetClassByName(dimension, kDimensionClass);

				if (current == nil)
				{
					current = dimension;
					inCurrent = 1;
					continue;
				}
				const MCObjectHandle joined = gSDK->CreateChainDimension(current, dimension);
				if (joined == nil)
				{
					// 繋げなかった。ここまでを仕上げ、この寸法から新しく繋ぎ直す。
					++counts.unjoined;
					flush();
					current = dimension;
					inCurrent = 1;
					continue;
				}
				current = joined;
				++inCurrent;
			}
			// 連続寸法も寸法クラスへ（中の直線寸法は作ったときに置いてある）。
			if (current != nil && inCurrent > 1)
				SetClassByName(current, kDimensionClass);
			flush();

			counts.dimensions += placed;
			return placed > 0;
		}

		// レイアウトの中から名前のテキスト（ストーリレベル名の動的テキスト）を探す。
		MCObjectHandle FindNameText(MCObjectHandle layout)
		{
			for (MCObjectHandle h = gSDK->FirstMemberObj(layout); h != nil; h = gSDK->NextObject(h))
			{
				if (gSDK->GetObjectTypeN(h) != kTextNode)
					continue;
				const std::string text = static_cast<const char*>(gSDK->GetTextChars(h));
				if (text.find(kLevelNameToken) != std::string::npos)
					return h;
			}
			return nil;
		}

		// 新しいテキストを古いテキストの位置へ合わせる。**揃える辺は古いテキストの揃え方で
		// 決める**（左揃えなら左端、中央なら中心、右揃えなら右端。縦は中心）——名前の長さは
		// トークンと違うので、外形の中心を合わせるだけだと左揃えの文字が左へはみ出す。
		void AlignText(MCObjectHandle text, MCObjectHandle reference)
		{
			WorldRect from;
			WorldRect to;
			if (!gSDK->GetObjectBounds(reference, from) || !gSDK->GetObjectBounds(text, to))
				return;
			short justification = kTextLeftJustify;
			gSDK->GetTextJustification(reference, justification);
			double dx = 0.0;
			if (justification == kTextRightJustify)
				dx = from.right - to.right;
			else if (justification == kTextCenterJustify)
				dx = ((from.left + from.right) - (to.left + to.right)) / 2.0;
			else
				dx = from.left - to.left;
			const double dy = ((from.top + from.bottom) - (to.top + to.bottom)) / 2.0;
			gSDK->MoveObject(text, dx, dy);
		}

		// マーカーレイアウトの名前のテキストを固定の文字へ差し替えて、レイアウトを渡し直す。
		// **中身を入れ替えるだけでは絵に出ない**ので、新しいテキストを作って入れ、古いものを
		// 消してから SetCustomObjectProfileGroup で渡す（Findings「Level Objects」の実測手順）。
		bool ReplaceLevelName(MCObjectHandle mark, const std::string& name)
		{
			const MCObjectHandle layout = HeldProfileGroup(mark);
			if (layout == nil)
				return false;
			const MCObjectHandle old = FindNameText(layout);
			if (old == nil)
				return false;

			const TXString chars(name.c_str());
			const MCObjectHandle text = gSDK->CreateTextBlock(chars, WorldPt(0.0, 0.0), false, 0);
			if (text == nil)
				return false;
			if (!gSDK->AddObjectToContainer(text, layout))
			{
				gSDK->DeleteObject(text, true);
				return false;
			}
			// 書式は古いテキストから写す（文字スタイル・揃え方・大きさ）。
			if (const InternalIndex style = gSDK->GetTextStyleRef(old); style != 0)
				gSDK->SetTextStyleRef(text, style);
			short justification = kTextLeftJustify;
			gSDK->GetTextJustification(old, justification);
			gSDK->SetTextJustification(text, justification);
			short vertical = 0;
			gSDK->GetTextVerticalAlignment(old, vertical);
			gSDK->SetTextVerticalAlignment(text, vertical);
			WorldCoord size = 0.0;
			gSDK->GetTextSize(old, 0, size);
			if (size > 0.0)
				gSDK->SetTextSize(text, 0, static_cast<Sint32>(chars.GetLength()), size);
			AlignText(text, old);

			gSDK->DeleteObject(old, true);
			return gSDK->SetCustomObjectProfileGroup(mark, layout) != 0;
		}

#if VW_DRAW_VERIFY
		// 検算（dev だけ）: container の中のテキストを**入れ子のグループまで**辿って集める
		// （PIO が吐いた図形は、レイアウトを写したグループの中にテキストを持つことがある）。
		// 深さは有限に留める（グループの入れ子は数段しか無い）。
		void CollectTexts(MCObjectHandle container, std::vector<std::string>& out, int depth = 0)
		{
			constexpr int kMaxDepth = 4;
			for (MCObjectHandle h = gSDK->FirstMemberObj(container); h != nil;
				 h = gSDK->NextObject(h))
			{
				const short type = gSDK->GetObjectTypeN(h);
				if (type == kTextNode)
					out.emplace_back(static_cast<const char*>(gSDK->GetTextChars(h)));
				else if (type == kGroupNode && depth < kMaxDepth)
					CollectTexts(h, out, depth + 1);
			}
		}

		std::string JoinTexts(const std::vector<std::string>& texts)
		{
			std::string joined;
			for (const std::string& text : texts)
				joined += "〈" + text + "〉";
			return joined.empty() ? std::string("（なし）") : joined;
		}

		// 検算（dev だけ）: レベル基準線が描いた文字に name があるか（Findings「Level
		// Objects」の「描いた文字を機械で読む」）。見つからなければ 1 個目について
		// 「描いた文字」と「レイアウトの中身」を probe へ控える（実機で何が起きたかを
		// 持ち帰る。差し替えが絵に届いていないのか、読み方が違うのかを分ける）。
		bool DrawsText(MCObjectHandle mark, const std::string& name, std::string& probe)
		{
			std::vector<std::string> drawn;
			CollectTexts(mark, drawn);
			if (std::ranges::find(drawn, name) != drawn.end())
				return true;
			if (probe.empty())
			{
				std::vector<std::string> layout;
				const MCObjectHandle direct = gSDK->GetCustomObjectProfileGroup(mark);
				const MCObjectHandle aux = gSDK->GetCustomObjectProfileGroupInAux(mark);
				const MCObjectHandle held = direct != nil ? direct : aux;
				if (held != nil)
					CollectTexts(held, layout);
				const char* source = "無し";
				if (direct != nil)
					source = "直接";
				else if (aux != nil)
					source = "aux";
				probe = name + ": 描いた文字 " + JoinTexts(drawn) + " / レイアウト（" + source +
						"）" + JoinTexts(layout);
			}
			return false;
		}
#endif

		// 描いた高さ（"10953.18" のような文字）を数に読む。読めなければ false。
		bool ParseShownHeight(const std::string& shown, double& value)
		{
			std::string digits;
			for (const char c : shown)
			{
				if (c != ',' && c != ' ')
					digits.push_back(c);
			}
			if (digits.empty())
				return false;
			char* end = nullptr;
			value = std::strtod(digits.c_str(), &end);
			return end != digits.c_str();
		}

		// **描いた高さを読み戻して命令の高さと引き比べる**（直さない）。ストーリレベルへ
		// 結んだ個体の高さは書けない出力（Elev）なので、食い違いは数えて診断へ出す——
		// 0 なら結べていないか、断面の向きがビュー行列へ写っていない（CopySectionViewMatrix）。
		void CheckLevelHeight(MCObjectHandle mark, double elevation, DimensionCounts& counts)
		{
			try
			{
				const VWParametricObj pio(mark);
				const std::string shown =
					static_cast<const char*>(pio.GetParamString(kParamShownElevation));
				double value = 0.0;
				if (!ParseShownHeight(shown, value))
				{
					++counts.levelHeightUnread;
					return;
				}
				if (std::abs(value - elevation) <= kLevelHeightTol)
					return;
				++counts.levelHeightMismatch;
				if (counts.levelHeightProbe.empty())
				{
					std::array<char, 128> buffer{};
					std::snprintf(buffer.data(), buffer.size(), "命令 %g に対し描いた高さ %s",
								  elevation, shown.c_str());
					counts.levelHeightProbe = buffer.data();
				}
			}
			catch (...)
			{
				++counts.levelHeightUnread;
			}
		}

		// レベル記号 1 つを置く。注釈へ置けたらそのハンドル、置けなければ nil。
		MCObjectHandle PlaceLevel(MCObjectHandle viewport, const core::LevelMarkCommand& level,
								  DimensionCounts& counts)
		{
			const MCObjectHandle mark = gSDK->CreateCustomObject(
				TXString(kLevelMarkPlugin), WorldPt(level.x, level.elevation), 0.0, true);
			if (mark == nil)
				return nil;
			if (!Annotate(viewport, mark))
				return nil;
			SetClassByName(mark, kDimensionClass);

			try
			{
				VWParametricObj pio(mark);
				// 注釈へ移すと VW が決めた位置へ落ちるので、座標を明示し直す（縦の位置は
				// 絵の置き場所だけで、描く数値には効かない）。
				pio.SetPointObjectPos(VWPoint2D(level.x, level.elevation));
				pio.SetParamString(kParamStoryName, TXString(level.story.c_str()));
				pio.SetParamString(kParamLevelTypeName, TXString(level.levelType.c_str()));
				pio.SetParamString(kParamDatum, kDatumStoryLevel);
			}
			catch (...)
			{
				// 結べなければ高さを拘束できない。名前の差し替えは続ける。
				++counts.levelBindFailed;
			}
			gSDK->ResetObject(mark);

			if (!ReplaceLevelName(mark, level.name))
				++counts.levelNameFailed;
			gSDK->ResetObject(mark);
			// **名前を差し替えた後も結び付きが残っているか**を読み戻す（書いても入らない値が
			// ある。Findings「Level Objects」の作法）。
			try
			{
				const VWParametricObj pio(mark);
				if (std::string(static_cast<const char*>(pio.GetParamString(kParamDatum))) !=
					kDatumStoryLevel)
					++counts.levelBindFailed;
			}
			catch (...)
			{
				++counts.levelBindFailed;
			}
#if VW_DRAW_VERIFY
			if (!DrawsText(mark, level.name, counts.levelNameProbe))
				++counts.levelNameUnseen;
#endif
			return mark;
		}
	} // namespace

	void prepareLevelMarkPlugin()
	{
		PrepareCustomObjectDefinition(kLevelMarkPlugin);
	}

	std::size_t drawViewportDimensions(MCObjectHandle viewport,
									   const core::ViewportCommand& command,
									   const std::vector<core::LevelMarkCommand>& levels,
									   const std::string& standard, double scale,
									   DimensionCounts& counts,
									   std::vector<PlacedLevelMark>* placedLevels)
	{
		if (viewport == nil || (command.dimensions.empty() && levels.empty()))
			return 0;
		const double denominator = scale > 0.0 ? scale : kFallbackScale;
		// 寸法規格の文字スタイルと、紙でその pt に見せる図面上の大きさ（ビューポートごとに
		// 決める——規格は文書に 1 つだが、縮尺はビューポートごとに違う）。縮尺は
		// ビューポートの**実際の**値を読む（注釈はそれで描かれる）。読めなければ寸法線の
		// 距離と同じ縮尺で代える。
		DimensionText text;
		try
		{
			text.style = DimensionStandardTextStyle(standard);
			double viewportScale = denominator;
			if (const double actual = VWViewportObj(viewport).GetScale(); actual > 0.0)
				viewportScale = actual;
			if (const double points = TextStylePoints(text.style); points > 0.0)
				text.fontSize = core::pointsToMillimeters(points) * viewportScale;
		}
		catch (...)
		{
			text = DimensionText{};
		}

		std::size_t drawn = 0;
		bool anyPlaced = false;
		for (const core::DimensionChainCommand& chain : command.dimensions)
		{
			if (PlaceChain(viewport, chain, standard, denominator, text, counts))
			{
				++drawn;
				anyPlaced = true;
			}
		}
		counts.chains += drawn;

		for (const core::LevelMarkCommand& level : levels)
		{
			if (const MCObjectHandle mark = PlaceLevel(viewport, level, counts); mark != nil)
			{
				++counts.levels;
				anyPlaced = true;
				if (placedLevels != nullptr)
					placedLevels->push_back({mark, level.elevation});
			}
			else
				++counts.levelsFailed;
		}

		// 注釈へ後から足した図形のクラスは非表示のままなので、全クラスを表示へ戻して描き直す
		// （draw/Dimension.h。データタグと同じ後処理）。
		if (anyPlaced)
		{
			counts.classesShown += ShowAllViewportClasses(viewport, command.hiddenClasses);
			try
			{
				VWViewportObj(viewport).Update();
			}
			catch (...)
			{
				++counts.updateFailed;
			}
		}
		return drawn;
	}

	void finishLevelMarks(MCObjectHandle viewport, const std::vector<PlacedLevelMark>& marks,
						  DimensionCounts& counts)
	{
		if (marks.empty())
			return;
		if (!CopySectionViewMatrix(viewport))
			++counts.viewMatrixFailed;
		for (const PlacedLevelMark& placed : marks)
		{
			// 写しただけでは描き直されない。作り直したときに初めてストーリレベルの高さが入る。
			gSDK->ResetObject(placed.mark);
			CheckLevelHeight(placed.mark, placed.elevation, counts);
		}
	}

	std::string dimensionDiagnostics(const std::string& label, const DimensionCounts& counts)
	{
		const bool placedAny = counts.dimensions > 0 || counts.levels > 0;
		const bool classesBroken = placedAny && counts.classesShown == 0;
#if VW_DRAW_VERIFY
		const bool verifyIssue = counts.levelNameUnseen > 0;
#else
		constexpr bool verifyIssue = false;
#endif
		if (counts.failed == 0 && counts.standardRejected == 0 && counts.unjoined == 0 &&
			counts.textStyleMissing == 0 && counts.textSizeUnread == 0 &&
			counts.levelsFailed == 0 && counts.levelNameFailed == 0 &&
			counts.levelBindFailed == 0 && counts.viewMatrixFailed == 0 &&
			counts.levelHeightUnread == 0 && counts.levelHeightMismatch == 0 &&
			counts.updateFailed == 0 && !classesBroken && !verifyIssue)
			return {};

		std::string text = label + "の寸法の診断: ";
		AppendCount(text, "置けなかった寸法", counts.failed, "本");
		AppendCount(text, "寸法規格を当てられなかった寸法", counts.standardRejected, "本",
					"図面にその寸法規格がありません。文書の既定の規格で描きました");
		AppendCount(text, "連続寸法へ繋げなかった継ぎ目", counts.unjoined, "箇所");
		AppendCount(text, "文字スタイルを当てられなかった寸法", counts.textStyleMissing, "本",
					"寸法規格が文字スタイルを持っていません。注釈では寸法値が描かれません");
		AppendCount(
			text, "文字の大きさを縮尺に合わせられなかった寸法", counts.textSizeUnread, "本",
			"文字スタイルの大きさを読めませんでした。寸法値が小さすぎて見えない可能性があります");
		AppendCount(text, "置けなかったレベル記号", counts.levelsFailed, "個");
		AppendCount(text, "名前を書けなかったレベル記号", counts.levelNameFailed, "個");
		AppendCount(text, "ストーリレベルへ結べなかったレベル記号", counts.levelBindFailed, "個",
					"高さが拘束されず、数値が 0 と出ることがあります");
		AppendCount(text, "断面の向きをビュー行列へ写せなかった軸組図", counts.viewMatrixFailed,
					"枚", "レベル記号の高さが 0 と出ます");
		AppendCount(text, "描いた高さを読めなかったレベル記号", counts.levelHeightUnread, "個");
		AppendCount(text, "描いた高さが命令と合わないレベル記号", counts.levelHeightMismatch, "個",
					counts.levelHeightProbe.c_str());
#if VW_DRAW_VERIFY
		AppendCount(text, "描いた文字に名前が見つからないレベル記号（検算）",
					counts.levelNameUnseen, "個", counts.levelNameProbe.c_str());
#endif
		if (classesBroken)
			text += "寸法を置いた後にクラスを表示へ戻せませんでした（寸法が映りません）。";
		AppendCount(text, "描き直せなかったビューポート", counts.updateFailed, "枚");
		return text;
	}

	std::string dimensionInfo(const std::string& label, const DimensionCounts& counts)
	{
		// 中身は今のところ dev の検算だけ。本番では引数を使わないので、tidy の
		// misc-unused-parameters / misc-const-correctness に掛からない形にしておく
		// （PR の CI は dev の分岐しか tidy しない）。
#if VW_DRAW_VERIFY
		if (!counts.dimensionProbe.empty())
			return label + "の寸法の記録: 寸法 1 本目（検算）: " + counts.dimensionProbe + "。";
#else
		(void)label;
		(void)counts;
#endif
		return {};
	}
} // namespace HomeskzIfcImport::draw
