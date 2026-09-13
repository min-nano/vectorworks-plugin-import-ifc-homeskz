//
//	UpdaterParse.h
//
//	The pure, platform- and SDK-independent helpers used by the updater
//	(Updater.cpp). They are factored out here so they can be unit-tested on any
//	toolchain WITHOUT the Vectorworks SDK: every function below operates only on
//	std::string / std::vector and has no dependency on gSDK, dladdr, Win32, or
//	the VWFC dialog classes.
//
//	Three families of helpers live here:
//	  * Parsing the updater script's machine-readable output
//	    (Trim / ValueOf / ParseDevBuilds).
//	  * Building safe command lines and deriving install paths from the plug-in's
//	    own binary location (ShellQuote / CmdQuote / the *FromBinary path helpers).
//	  * The update flows' branch-y decisions (EvaluateStable / ResolveCurrentDevBuild /
//	    DevSwitchCandidates / InstallReportedOk / NeedsRestartAfterInstall).
//
//	**再起動のコマンドを組み立てる関数はここには無い。** 以前は終了と起動し直しを
//	切り離したヘルパープロセスへ任せていて、その 1 行をここで組み立てていたが、
//	更新の確認が「起動中」から**メニューコマンド**へ移ったことで、Vectorworks 自身に
//	頼めるようになった（SDK の CloseAllFilesAndQuitVectorworks。src/Updater.cpp の
//	CVectorworksUpdaterHost::Restart）。
//
//	Updater.cpp keeps only the genuinely platform-specific glue (locating its own
//	binary via dladdr/GetModuleFileName, spawning the script, showing native
//	dialogs) and delegates all string work to the functions here.
//

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::UpdaterParse
{
	// ---------------------------------------------------------------------
	// Script-output parsing. The two bundled scripts (vw-update.sh /
	// vw-update.ps1) print the same machine-readable format on both platforms:
	// "key=value" lines, plus tab-separated "build\t..." rows for q-dev.
	// ---------------------------------------------------------------------

	// Strip leading/trailing ASCII whitespace. Returns "" for an all-blank input.
	inline std::string Trim(const std::string& s)
	{
		std::string::size_type const b = s.find_first_not_of(" \t\r\n");
		if (b == std::string::npos)
			return "";
		std::string::size_type const e = s.find_last_not_of(" \t\r\n");
		return s.substr(b, e - b + 1);
	}

	// Value of the first "key=value" line whose key matches (key without '='),
	// or "" if absent.
	inline std::string ValueOf(const std::string& out, const std::string& key)
	{
		std::string const needle = key + "=";
		std::string::size_type pos = 0;
		while (pos < out.size())
		{
			std::string::size_type const eol = out.find('\n', pos);
			std::string const line =
				out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
			if (line.starts_with(needle))
				return Trim(line.substr(needle.size()));
			if (eol == std::string::npos)
				break;
			pos = eol + 1;
		}
		return "";
	}

	// **dev プレリリースの表示名からブランチを取り出す。** CI が付ける題は
	// "Dev: <branch> (<short sha>)"（.github/workflows/build.yml の
	// `--title "Dev: ${branch} (${short})"`）。その形でなければ空を返す。
	//
	// **これは保険で、正規の出どころは q-dev の 5 列目**（DevBuild::branch）である。
	// 列を出さない**古い同梱スクリプト**が走ることがあり（インストール済みのものが
	// 走るので、新しい殻＋古いスクリプトという組み合わせが必ず起こる）、そのときに
	// ブランチが分からないと「同じブランチの新しいビルド」を拾う経路が丸ごと死ぬ。
	// 題からでも確実に取れるので、空欄はここで埋める（ParseDevBuilds）。
	inline std::string DevBuildBranch(const std::string& name)
	{
		const std::string prefix = "Dev: ";
		if (!name.starts_with(prefix))
			return "";
		const std::string::size_type open = name.rfind(" (");
		if (open == std::string::npos || open <= prefix.size())
			return "";
		return name.substr(prefix.size(), open - prefix.size());
	}

	struct DevBuild
	{
		std::string commit;
		std::string name;
		std::string url;
		// そのビルドが出たブランチ（"feature/x"）。**取り込みのついでの確認**と
		// **実機フィードバックの往復**が「いま動いているのと同じブランチの新しい
		// ビルド」だけを拾うために要る。正規の出どころは q-dev の 5 列目で、その列を
		// 出さない古い同梱スクリプトのときは表示名から補う（DevBuildBranch）。
		std::string branch;
	};

	// Parse the "build<TAB>commit<TAB>name<TAB>url[<TAB>branch]" lines from q-dev
	// output. Lines that are not "build\t..." rows, or that are missing fields or
	// a URL, are skipped.
	//
	// **branch は任意。** インストール済みの（＝古い）同梱スクリプトが走ることが
	// あるので、4 列しか出さない出力も読めなければならない（src/Updater.cpp）。
	// その場合は**表示名から補う**（DevBuildBranch）——ここを空のまま通すと、
	// 「同じブランチの新しいビルド」を拾う経路（取り込み時の確認・実機フィードバックの
	// 往復）が、古いスクリプトが入っている間だけ黙って死ぬ。
	inline std::vector<DevBuild> ParseDevBuilds(const std::string& out)
	{
		std::vector<DevBuild> builds;
		std::string::size_type pos = 0;
		while (pos < out.size())
		{
			std::string::size_type const eol = out.find('\n', pos);
			std::string const line =
				out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
			pos = (eol == std::string::npos) ? out.size() : eol + 1;

			if (!line.starts_with("build\t"))
				continue;

			// Split the tab-separated fields after "build".
			std::string const rest = line.substr(6);
			std::string::size_type const t1 = rest.find('\t');
			if (t1 == std::string::npos)
				continue;
			std::string::size_type const t2 = rest.find('\t', t1 + 1);
			if (t2 == std::string::npos)
				continue;
			std::string::size_type const t3 = rest.find('\t', t2 + 1);

			DevBuild b;
			b.commit = Trim(rest.substr(0, t1));
			b.name = Trim(rest.substr(t1 + 1, t2 - (t1 + 1)));
			if (t3 == std::string::npos)
			{
				b.url = Trim(rest.substr(t2 + 1));
			}
			else
			{
				b.url = Trim(rest.substr(t2 + 1, t3 - (t2 + 1)));
				b.branch = Trim(rest.substr(t3 + 1));
			}
			if (b.branch.empty())
				b.branch = DevBuildBranch(b.name);
			if (!b.url.empty())
				builds.push_back(b);
		}
		return builds;
	}

	// ---------------------------------------------------------------------
	// Command-line quoting. Pure string transforms; kept here (rather than under
	// platform #ifdefs) so BOTH quoting rules are exercised by the unit tests on
	// any host.
	// ---------------------------------------------------------------------

	// Wrap a string in single quotes so it is safe as one /bin/sh word, escaping
	// any embedded single quotes (bundle paths and URLs can contain surprises).
	inline std::string ShellQuote(const std::string& s)
	{
		std::string out = "'";
		for (char const c : s)
		{
			if (c == '\'')
				out += "'\\''";
			else
				out += c;
		}
		out += "'";
		return out;
	}

	// Wrap a string in double quotes for a cmd.exe/PowerShell command line. Our
	// arguments are release-asset URLs and fixed plug-in names, which never
	// contain quotes; drop any that somehow appear rather than risk breaking the
	// quoting.
	inline std::string CmdQuote(const std::string& s)
	{
		std::string out = "\"";
		for (char const c : s)
			if (c != '"')
				out += c;
		out += "\"";
		return out;
	}

	// ---------------------------------------------------------------------
	// Deriving install paths from the plug-in's own binary path. The platform
	// code in Updater.cpp resolves its own binary (dladdr / GetModuleFileName);
	// the string surgery that turns that path into the bundled-script path or the
	// Plug-Ins folder is pure, and lives here.
	// ---------------------------------------------------------------------

	// macOS: from the loaded binary path
	//   .../<name>.vwlibrary/Contents/MacOS/<name>
	// derive a bundled script
	//   .../<name>.vwlibrary/Contents/Resources/<baseName>.sh
	// Returns "" if the "/Contents/MacOS/" marker is not present.
	//
	// baseName は**拡張子を除いた名前**（"vw-update" / "vw-feedback"）。同梱スクリプトが
	// 2 本になった（M23）ので名前を引数に取るが、既定はアップデータのまま——呼び出し側の
	// ほとんどはそれで、ここを既定なしにすると綴りが 2 か所に散る。
	inline std::string MacScriptPathFromBinary(const std::string& binaryPath,
											   const std::string& baseName = "vw-update")
	{
		const std::string marker = "/Contents/MacOS/";
		std::string::size_type const at = binaryPath.rfind(marker);
		if (at == std::string::npos)
			return "";

		// substr up to and including "/Contents/" (10 chars), then Resources/…
		std::string const contents = binaryPath.substr(0, at + std::string("/Contents/").size());
		return contents + "Resources/" + baseName + ".sh";
	}

	// macOS: from the loaded binary path
	//   .../<PlugIns>/<name>.vwlibrary/Contents/MacOS/<name>
	// derive the folder that CONTAINS the .vwlibrary bundle (the exact Plug-Ins
	// folder this build was loaded from):
	//   .../<PlugIns>
	// Returns "" if the path does not have the expected shape.
	inline std::string MacPluginsDirFromBinary(const std::string& binaryPath)
	{
		std::string::size_type const at = binaryPath.rfind("/Contents/MacOS/");
		if (at == std::string::npos)
			return "";

		std::string const bundle = binaryPath.substr(0, at); // .../<PlugIns>/<name>.vwlibrary
		std::string::size_type const slash = bundle.rfind('/');
		if (slash == std::string::npos)
			return "";
		return bundle.substr(0, slash); // .../<PlugIns>
	}

	// Windows: directory that contains the given module path. On Windows the
	// plug-in is a bare "<name>.vlb" living directly in the Plug-Ins folder, so
	// this is both where the updater script sits and the Plug-Ins folder to
	// install into. Accepts either separator. Returns "" if there is none.
	inline std::string WinModuleDirFromPath(const std::string& modulePath)
	{
		std::string::size_type const slash = modulePath.find_last_of("\\/");
		if (slash == std::string::npos)
			return "";
		return modulePath.substr(0, slash);
	}

	// Windows: the bundled scripts sit next to the module. baseName は macOS 側と同じく
	// 拡張子を除いた名前で、こちらは .ps1 が付く。
	inline std::string WinScriptPathFromDir(const std::string& moduleDir,
											const std::string& baseName = "vw-update")
	{
		if (moduleDir.empty())
			return "";
		return moduleDir + "\\" + baseName + ".ps1";
	}

	// ---------------------------------------------------------------------
	// Update-flow decisions. These are the branch-y choices the updater makes
	// once it has the script's output in hand — "is there a newer build?",
	// "which builds can I switch to?", "did the install succeed?". They were the
	// last pieces of real logic still inlined in Updater.cpp between gSDK calls;
	// pulled out here (operating only on strings/vectors, no gSDK) they are
	// exercised by the unit tests, while Updater.cpp keeps just the native-dialog
	// glue that acts on the result.
	// ---------------------------------------------------------------------

	// Outcome of interpreting `q-stable` output for the stable-channel startup
	// check. offerUpdate is the single decision Updater.cpp acts on; the other
	// fields feed the dialog it then shows.
	struct StableStatus
	{
		bool offerUpdate = false; // a newer build exists -> prompt to install
		std::string installed;	  // installed commit ("" if none/unknown)
		std::string latest;		  // latest published commit
		std::string url;		  // asset download URL for `latest`
	};

	// Decide whether the stable channel has an update worth prompting for.
	// offerUpdate is true only when the output is well-formed (no error= line,
	// both latest and url present) AND latest differs from the installed commit.
	// A transient error, incomplete output, or an already-current install all
	// yield offerUpdate == false (Updater.cpp then stays silent).
	inline StableStatus EvaluateStable(const std::string& out)
	{
		StableStatus s;
		if (!ValueOf(out, "error").empty())
			return s; // offline / transient -> stay silent
		s.installed = ValueOf(out, "installed");
		s.latest = ValueOf(out, "latest");
		s.url = ValueOf(out, "url");
		if (s.latest.empty() || s.url.empty())
			return s; // incomplete -> stay silent
		if (s.installed == s.latest)
			return s; // already current -> no dialog
		s.offerUpdate = true;
		return s;
	}

	// -----------------------------------------------------------------------
	// **いま効いている開発版ビルドの素性**（ブランチと短縮 sha）。
	//
	// 殻にコンパイルされた VW_BUILD_BRANCH / VW_BUILD_VERSION を「いま動いているビルド」
	// と呼べるのは、**殻ごと入れ替わったときだけ**である。本体（.vwpayload）だけの更新は
	// 再起動せずにその場で効くので（src/PayloadAbi.h）、別のブランチのビルドへ乗り換えた
	// あとも殻のその 2 つの定数は前のブランチを名乗り続ける——そのまま基準にすると、
	//
	//   * 選択ダイアログが「現在: 前のブランチ」と出し、**いま入れたビルドをもう一度
	//     候補に並べる**（選び直しても切り替わっていないように見える）。
	//   * 取り込みのついでの確認と往復の確認が**前のブランチ**の新しいビルドを拾い、
	//     選んだブランチのビルドを黙って上書きして元のブランチへ戻す。
	//
	// という食い違いが起きる（実機で発生。docs/DEV-NOTES.md M26）。
	//
	// 基準にするのは**ディスク上に入っているビルド**——次に読み込まれるのはそれだから
	// で、安定版が最初から `q-stable` の `installed=` を基準にしているのと同じ考え方で
	// ある（EvaluateStable）。
	struct CurrentDevBuild
	{
		std::string branch; // ディスク上のビルドが出たブランチ
		std::string commit; // その短縮 sha
	};

	// q-dev の出力から「いま入っている開発版ビルド」を決める。shellBranch / shellCommit は
	// 殻にコンパイルされた値で、**ディスクから分からなかったときだけ**使う。
	//
	// ブランチの出どころは 3 段構え。**どれも「新しい殻＋古い同梱スクリプト」という組み
	// 合わせが必ず起こる**（走るのはインストール済みの＝古いスクリプト）ことへの備えで、
	// 1 つ上の段が無いときに下へ落ちる。
	//
	//   1. `installed-branch=`（ディスク上のビルドの刻印。mac は Info.plist の
	//      VWBuildBranch、Windows は `<name>.branch`）。
	//   2. 並んでいるビルドの中で **sha が一致する行のブランチ**。古いスクリプトは 1 を
	//      出さないので、手で乗り換えた直後（そのビルドがまだそのブランチの頭）はこれで
	//      足りる。
	//   3. 殻の値。ディスクについて何も分からないときはこれしかない。
	inline CurrentDevBuild ResolveCurrentDevBuild(const std::string& out,
												  const std::string& shellBranch,
												  const std::string& shellCommit)
	{
		CurrentDevBuild current;
		current.commit = ValueOf(out, "installed");
		if (current.commit.empty() || current.commit == "none")
			current.commit = shellCommit;

		current.branch = ValueOf(out, "installed-branch");
		if (current.branch == "none")
			current.branch.clear();
		if (current.branch.empty())
		{
			for (const DevBuild& b : ParseDevBuilds(out))
			{
				if (b.commit == current.commit && !b.branch.empty())
				{
					current.branch = b.branch;
					break;
				}
			}
		}
		if (current.branch.empty())
			current.branch = shellBranch;
		return current;
	}

	// The builds the dev picker offers to switch TO: every parsed build except
	// the one already running (matched by commit). Order is preserved, so
	// candidate i maps to picker entry i+1 (entry 0 is the "keep current" row).
	inline std::vector<DevBuild> DevSwitchCandidates(const std::string& out,
													 const std::string& runningCommit)
	{
		std::vector<DevBuild> others;
		for (const DevBuild& b : ParseDevBuilds(out))
			if (b.commit != runningCommit)
				others.push_back(b);
		return others;
	}

	// Map the picker's 0-based selection back to an index into the candidate list
	// (as returned by DevSwitchCandidates). Entry 0 is "keep the current build",
	// so a selection <= 0 -> -1. A selection past the last candidate is also
	// treated as "keep current" (a safeguard) -> -1. Otherwise -> selection - 1.
	// **同じブランチの、いま動いているものとは違うビルド**を選ぶ（実機フィードバックの
	// 往復。docs/DEV-NOTES.md M23）。見つかった添字、無ければ -1。
	//
	// **ブランチで絞るのが肝。** 絞らずに「自分と違う dev ビルド」を取ると、他人が別の
	// ブランチを push しただけで、まったく関係の無いビルドへ乗り換えてしまう。候補が
	// 複数あるときは先頭（GitHub が返すのは新しい順）を採る。
	inline int FindDevBuildForBranch(const std::vector<DevBuild>& builds, const std::string& branch)
	{
		if (branch.empty())
			return -1;
		for (std::size_t i = 0; i < builds.size(); ++i)
		{
			if (builds[i].branch == branch)
				return static_cast<int>(i);
		}
		return -1;
	}

	inline int ResolveDevSelection(short selection, std::size_t candidateCount)
	{
		if (selection <= 0)
			return -1; // kept the installed build
		std::size_t const idx = static_cast<std::size_t>(selection) - 1;
		if (idx >= candidateCount)
			return -1; // out of range -> keep current
		return static_cast<int>(idx);
	}

	// True if `do-install` reported success (its sole success token is "ok").
	// The installer prints "ok" on a line of its own when it succeeded. It may
	// print other key=value lines BEFORE it (installed-shell=..., below), so this
	// looks for the line rather than comparing the whole output — a stricter
	// "the whole output is ok" test would read every successful install with
	// extra information as a failure.
	inline bool InstallReportedOk(const std::string& out)
	{
		std::size_t pos = 0;
		while (pos <= out.size())
		{
			const std::size_t nl = out.find('\n', pos);
			const std::string line =
				Trim(out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
			if (line == "ok")
				return true;
			if (nl == std::string::npos)
				break;
			pos = nl + 1;
		}
		return false;
	}

	// The message to show when `do-install` did NOT succeed: the script's own
	// "error=" line if it printed one, otherwise the caller's generic fallback
	// (the fallback is passed in so the user-facing wording stays in Updater.cpp).
	inline std::string InstallErrorText(const std::string& out, const std::string& fallback)
	{
		std::string const e = ValueOf(out, "error");
		return e.empty() ? fallback : e;
	}
	// ---------------------------------------------------------------------
	// アップデートの後始末: **Vectorworks の再起動が要るか。**
	//
	// プラグインは 2 つに割れている（src/PayloadAbi.h）——Vectorworks が起動時にしか
	// 読み込めない**殻**と、殻が自分で読み込む**本体（.vwpayload）**。本体だけが新しく
	// なったのなら、次の取り込み・次の PIO リセットで読み直されるので**再起動は要らない**。
	// 殻まで変わっていれば、それを読み込めるのは次の起動だけなので要る。
	// ---------------------------------------------------------------------

	// `do-install` の出力から「いま入れた殻の ID」を取り出す。この行を出さない古い
	// スクリプトが同梱されていた場合は空になる。
	inline std::string InstalledShellId(const std::string& out)
	{
		return ValueOf(out, "installed-shell");
	}

	// 入れ替えたあと、Vectorworks の再起動が要るか。
	//
	// **判断できないとき（どちらかが空）は必ず「要る」へ倒す。** 殻と本体は別々に配られる
	// ので、食い違ったまま動かすほうが危ない——本体の版が合わなければ殻はそれを読み込まず、
	// プラグインは何もできない状態になる（src/PayloadHost.cpp の版チェック）。
	inline bool NeedsRestartAfterInstall(const std::string& runningShellId,
										 const std::string& installedShellId)
	{
		if (runningShellId.empty() || installedShellId.empty())
			return true;
		return runningShellId != installedShellId;
	}
} // namespace HomeskzIfcImport::UpdaterParse
