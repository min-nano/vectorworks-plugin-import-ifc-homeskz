//
//	UpdaterParse.h
//
//	The pure, platform- and SDK-independent helpers used by the updater
//	(Updater.cpp). They are factored out here so they can be unit-tested on any
//	toolchain WITHOUT the Vectorworks SDK: every function below operates only on
//	std::string / std::vector and has no dependency on gSDK, dladdr, Win32, or
//	the VWFC dialog classes.
//
//	Two families of helpers live here:
//	  * Parsing the updater script's machine-readable output
//	    (Trim / ValueOf / ParseDevBuilds).
//	  * Building safe command lines and deriving install paths from the plug-in's
//	    own binary location (ShellQuote / CmdQuote / the *FromBinary path helpers).
//
//	Updater.cpp keeps only the genuinely platform-specific glue (locating its own
//	binary via dladdr/GetModuleFileName, spawning the script, showing native
//	dialogs) and delegates all string work to the functions here.
//

#pragma once

#include <string>
#include <vector>

namespace SamplePlugin
{
namespace UpdaterParse
{
	// ---------------------------------------------------------------------
	// Script-output parsing. The two bundled scripts (vw-update.sh /
	// vw-update.ps1) print the same machine-readable format on both platforms:
	// "key=value" lines, plus tab-separated "build\t..." rows for q-dev.
	// ---------------------------------------------------------------------

	// Strip leading/trailing ASCII whitespace. Returns "" for an all-blank input.
	inline std::string Trim(const std::string& s)
	{
		std::string::size_type b = s.find_first_not_of(" \t\r\n");
		if (b == std::string::npos) return "";
		std::string::size_type e = s.find_last_not_of(" \t\r\n");
		return s.substr(b, e - b + 1);
	}

	// Value of the first "key=value" line whose key matches (key without '='),
	// or "" if absent.
	inline std::string ValueOf(const std::string& out, const std::string& key)
	{
		std::string needle = key + "=";
		std::string::size_type pos = 0;
		while (pos < out.size())
		{
			std::string::size_type eol = out.find('\n', pos);
			std::string line = out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
			if (line.compare(0, needle.size(), needle) == 0)
				return Trim(line.substr(needle.size()));
			if (eol == std::string::npos) break;
			pos = eol + 1;
		}
		return "";
	}

	struct DevBuild
	{
		std::string	commit;
		std::string	name;
		std::string	url;
	};

	// Parse the "build<TAB>commit<TAB>name<TAB>url" lines from q-dev output.
	// Lines that are not "build\t..." rows, or that are missing fields or a URL,
	// are skipped.
	inline std::vector<DevBuild> ParseDevBuilds(const std::string& out)
	{
		std::vector<DevBuild> builds;
		std::string::size_type pos = 0;
		while (pos < out.size())
		{
			std::string::size_type eol = out.find('\n', pos);
			std::string line = out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
			pos = (eol == std::string::npos) ? out.size() : eol + 1;

			if (line.compare(0, 6, "build\t") != 0)
				continue;

			// Split the three tab-separated fields after "build".
			std::string rest = line.substr(6);
			std::string::size_type t1 = rest.find('\t');
			if (t1 == std::string::npos) continue;
			std::string::size_type t2 = rest.find('\t', t1 + 1);
			if (t2 == std::string::npos) continue;

			DevBuild b;
			b.commit = Trim(rest.substr(0, t1));
			b.name   = Trim(rest.substr(t1 + 1, t2 - (t1 + 1)));
			b.url    = Trim(rest.substr(t2 + 1));
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
		for (char c : s)
		{
			if (c == '\'')	out += "'\\''";
			else			out += c;
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
		for (char c : s)
			if (c != '"') out += c;
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
	// derive the bundled script
	//   .../<name>.vwlibrary/Contents/Resources/vw-update.sh
	// Returns "" if the "/Contents/MacOS/" marker is not present.
	inline std::string MacScriptPathFromBinary(const std::string& binaryPath)
	{
		const std::string marker = "/Contents/MacOS/";
		std::string::size_type at = binaryPath.rfind(marker);
		if (at == std::string::npos)
			return "";

		// substr up to and including "/Contents/" (10 chars), then Resources/…
		std::string contents = binaryPath.substr(0, at + std::string("/Contents/").size());
		return contents + "Resources/vw-update.sh";
	}

	// macOS: from the loaded binary path
	//   .../<PlugIns>/<name>.vwlibrary/Contents/MacOS/<name>
	// derive the folder that CONTAINS the .vwlibrary bundle (the exact Plug-Ins
	// folder this build was loaded from):
	//   .../<PlugIns>
	// Returns "" if the path does not have the expected shape.
	inline std::string MacPluginsDirFromBinary(const std::string& binaryPath)
	{
		std::string::size_type at = binaryPath.rfind("/Contents/MacOS/");
		if (at == std::string::npos)
			return "";

		std::string bundle = binaryPath.substr(0, at);		// .../<PlugIns>/<name>.vwlibrary
		std::string::size_type slash = bundle.rfind('/');
		if (slash == std::string::npos)
			return "";
		return bundle.substr(0, slash);						// .../<PlugIns>
	}

	// Windows: directory that contains the given module path. On Windows the
	// plug-in is a bare "<name>.vlb" living directly in the Plug-Ins folder, so
	// this is both where the updater script sits and the Plug-Ins folder to
	// install into. Accepts either separator. Returns "" if there is none.
	inline std::string WinModuleDirFromPath(const std::string& modulePath)
	{
		std::string::size_type slash = modulePath.find_last_of("\\/");
		if (slash == std::string::npos)
			return "";
		return modulePath.substr(0, slash);
	}

	// Windows: the bundled updater script sits next to the module.
	inline std::string WinScriptPathFromDir(const std::string& moduleDir)
	{
		if (moduleDir.empty())
			return "";
		return moduleDir + "\\vw-update.ps1";
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
		bool		offerUpdate = false;	// a newer build exists -> prompt to install
		std::string	installed;				// installed commit ("" if none/unknown)
		std::string	latest;					// latest published commit
		std::string	url;					// asset download URL for `latest`
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
			return s;							// offline / transient -> stay silent
		s.installed = ValueOf(out, "installed");
		s.latest    = ValueOf(out, "latest");
		s.url       = ValueOf(out, "url");
		if (s.latest.empty() || s.url.empty())
			return s;							// incomplete -> stay silent
		if (s.installed == s.latest)
			return s;							// already current -> no dialog
		s.offerUpdate = true;
		return s;
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
	inline int ResolveDevSelection(short selection, std::size_t candidateCount)
	{
		if (selection <= 0)
			return -1;							// kept the installed build
		std::size_t idx = static_cast<std::size_t>(selection) - 1;
		if (idx >= candidateCount)
			return -1;							// out of range -> keep current
		return static_cast<int>(idx);
	}

	// True if `do-install` reported success (its sole success token is "ok").
	inline bool InstallReportedOk(const std::string& out)
	{
		return Trim(out) == "ok";
	}

	// The message to show when `do-install` did NOT succeed: the script's own
	// "error=" line if it printed one, otherwise the caller's generic fallback
	// (the fallback is passed in so the user-facing wording stays in Updater.cpp).
	inline std::string InstallErrorText(const std::string& out, const std::string& fallback)
	{
		std::string e = ValueOf(out, "error");
		return e.empty() ? fallback : e;
	}
}
}
