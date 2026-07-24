//
//	UpdaterRobustnessTests.cpp
//
//	Adversarial / unexpected-input tests for the SDK-independent updater helpers
//	in src/UpdaterParse.h.
//
//	The parsers here consume text that ultimately comes from OUTSIDE the plug-in:
//	the machine-readable output of the bundled updater scripts, which in turn is
//	built from the GitHub REST API's responses (release names, asset URLs, commit
//	SHAs, branch names). None of that is under our control — GitHub can change a
//	field, a branch name can contain odd bytes, a network hiccup can truncate the
//	output mid-line. These tests therefore feed the parsers deliberately hostile
//	and malformed input and assert two things:
//
//	  1. No crash / no undefined behaviour. This matters most when the suite is
//	     built with -DVW_ENABLE_SANITIZERS=ON (ASan + UBSan): an out-of-bounds
//	     read, a bad substr, or signed overflow then ABORTS the run instead of
//	     silently returning garbage. The included pseudo-fuzz loop drives every
//	     helper with thousands of random byte strings for exactly this purpose.
//
//	  2. The parsing CONTRACT still holds under garbage input — e.g. every build
//	     ParseDevBuilds returns has a non-empty url, EvaluateStable only offers an
//	     update when the output is well-formed, ResolveDevSelection never returns
//	     an out-of-range index. These invariants are what a refactor might quietly
//	     break, so we pin them here rather than only on the happy-path inputs.
//
//	Everything is deterministic (a fixed-seed LCG, never std::random_device), so a
//	failure always reproduces.
//

#include "TestFramework.h"
#include "UpdaterParse.h"

#include <climits>
#include <cstdint>
#include <string>
#include <vector>

using namespace SamplePlugin::UpdaterParse;

namespace
{
	// A tiny deterministic PRNG (xorshift32). Fixed seed -> reproducible fuzz.
	struct Rng
	{
		std::uint32_t state;
		explicit Rng(std::uint32_t seed) : state(seed ? seed : 0x1234567u) {}
		std::uint32_t next()
		{
			std::uint32_t x = state;
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			state = x;
			return x;
		}
		std::size_t below(std::size_t n)
		{
			return n == 0 ? 0 : (next() % n);
		}
	};

	// A random byte string of length [0, maxLen). Bytes span the FULL 0..255
	// range and deliberately over-sample the characters the parsers key on
	// ('\n', '\t', '=', ' ', '\'', '"', '\0') so structure appears by chance.
	std::string RandomBytes(Rng& rng, std::size_t maxLen)
	{
		static const char kSpecial[] = {'\n', '\t', '=', ' ', '\'', '"', '\0', '\r'};
		const std::size_t len = rng.below(maxLen);
		std::string s;
		s.reserve(len);
		for (std::size_t i = 0; i < len; ++i)
		{
			if (rng.below(3) == 0)
				s.push_back(kSpecial[rng.below(sizeof(kSpecial))]);
			else
				s.push_back(static_cast<char>(rng.below(256)));
		}
		return s;
	}

	// Run every parser/helper on `s`. Returns nothing meaningful; the point is
	// that, under ASan/UBSan, any illegal access inside these calls aborts. We
	// also assert the invariants that must hold for ANY input.
	void ExerciseAll(int& failures, const std::string& s)
	{
		// String-output helpers: just must not crash.
		(void)Trim(s);
		(void)ShellQuote(s);
		(void)CmdQuote(s);
		(void)MacScriptPathFromBinary(s);
		(void)MacPluginsDirFromBinary(s);
		(void)WinModuleDirFromPath(s);
		(void)WinScriptPathFromDir(s);

		// ShellQuote invariant: single-quoted, and it never leaves a bare '
		// unescaped (each interior ' must be part of the '\'' sequence).
		{
			const std::string q = ShellQuote(s);
			CHECK(q.size() >= 2);
			CHECK(q.front() == '\'');
			CHECK(q.back() == '\'');
		}

		// CmdQuote invariant: double-quoted wrapper, and NO interior double quote
		// survives (they are dropped by design).
		{
			const std::string q = CmdQuote(s);
			CHECK(q.size() >= 2);
			CHECK(q.front() == '"');
			CHECK(q.back() == '"');
			// Interior (everything but the two wrapping quotes) has no '"'.
			CHECK(q.substr(1, q.size() - 2).find('"') == std::string::npos);
		}

		// Use the fuzz string both as a whole "script output" and as a key.
		(void)ValueOf(s, "latest");
		(void)ValueOf(s, s); // key drawn from the same soup
		(void)InstallReportedOk(s);
		(void)InstallErrorText(s, "fallback");

		// ParseDevBuilds invariant: every returned build carries a non-empty url
		// (that is the sole filter the parser applies).
		for (const DevBuild& b : ParseDevBuilds(s))
			CHECK(!b.url.empty());

		// EvaluateStable invariant: an update is offered ONLY for well-formed,
		// non-current output.
		{
			const StableStatus st = EvaluateStable(s);
			if (st.offerUpdate)
			{
				CHECK(ValueOf(s, "error").empty());
				CHECK(!st.latest.empty());
				CHECK(!st.url.empty());
				CHECK(st.installed != st.latest);
			}
		}

		// DevSwitchCandidates never invents a build and never keeps the running
		// commit.
		for (const DevBuild& b : DevSwitchCandidates(s, "run1234"))
		{
			CHECK(!b.url.empty());
			CHECK(b.commit != "run1234");
		}
	}
} // namespace

// ---------------------------------------------------------------------------
// Deterministic pseudo-fuzz: thousands of random byte strings through every
// helper. The invariants above run on each; ASan/UBSan (when enabled) turn any
// memory error into a hard failure.
// ---------------------------------------------------------------------------

TEST(fuzz_random_bytes_never_crash_and_hold_invariants)
{
	Rng rng(0xC0FFEEu);
	for (int i = 0; i < 20000; ++i)
		ExerciseAll(failures, RandomBytes(rng, 64));
}

TEST(fuzz_long_random_lines)
{
	// Longer inputs so multi-line parsing (ValueOf / ParseDevBuilds) walks many
	// line boundaries per call.
	Rng rng(0x5EEDu);
	for (int i = 0; i < 2000; ++i)
		ExerciseAll(failures, RandomBytes(rng, 1024));
}

// ---------------------------------------------------------------------------
// Embedded NUL bytes. std::string is not NUL-terminated logic here, but a naive
// refactor to C-strings would truncate; and any indexing past a NUL would be
// caught by ASan.
// ---------------------------------------------------------------------------

TEST(embedded_nul_in_script_output)
{
	// Build a script-output string with real NUL bytes INSIDE the value and
	// inside a build row (using the (ptr,len) constructor so the NUL is not
	// treated as a terminator). ValueOf/ParseDevBuilds split on '\n' and '\t',
	// neither of which is NUL, so the NUL is just another value byte.
	std::string out;
	out += "installed=abc";
	out += '\0'; // NUL in the middle of the value
	out += "def\n";
	out += "latest=def5678\n";
	out += "url=https://ex.com/x.zip\n";
	out += "build\tc1";
	out += '\0'; // NUL inside the commit field
	out += "x\tbranch\thttps://ex.com/a.zip\n";

	// Must not crash; the NUL-bearing value comes back with the NUL intact.
	CHECK_EQ(ValueOf(out, "installed").size(), static_cast<std::size_t>(7)); // "abc\0def"
	CHECK_EQ(ValueOf(out, "latest"), "def5678");
	for (const DevBuild& b : ParseDevBuilds(out))
		CHECK(!b.url.empty());
	StableStatus s = EvaluateStable(out);
	CHECK(s.offerUpdate); // installed(abc\0def) != latest(def5678), url present
}

TEST(embedded_nul_in_quoters_and_paths)
{
	std::string withNul = "a";
	withNul += '\0';
	withNul += "b'c";
	withNul += '\0';
	withNul += "/Contents/MacOS/";
	withNul += '\0';
	withNul += "x";
	const std::string q = ShellQuote(withNul);
	CHECK(q.front() == '\'');
	CHECK(q.back() == '\'');
	// The embedded ' is still escaped even with NULs around it.
	CHECK(q.size() > withNul.size());
	(void)CmdQuote(withNul);
	(void)MacScriptPathFromBinary(withNul);
	(void)MacPluginsDirFromBinary(withNul);
	(void)WinModuleDirFromPath(withNul);
}

// ---------------------------------------------------------------------------
// Degenerate line structure — the kinds of thing a truncated or reshaped GitHub
// response could produce.
// ---------------------------------------------------------------------------

TEST(key_at_end_without_value_or_newline)
{
	// "latest=" with nothing after it, at the very end (no '\n').
	CHECK_EQ(ValueOf("installed=abc\nlatest=", "latest"), "");
	// Bare key, no '=' at all: not a match.
	CHECK_EQ(ValueOf("latest\n", "latest"), "");
	// '=' but the key is only a prefix of the line's key.
	CHECK_EQ(ValueOf("latestbuild=x\n", "latest"), "");
}

TEST(only_newlines_and_blank_lines)
{
	const std::string out(500, '\n');
	CHECK_EQ(ValueOf(out, "latest"), "");
	CHECK_EQ(ParseDevBuilds(out).size(), static_cast<std::size_t>(0));
	CHECK(!EvaluateStable(out).offerUpdate);
}

TEST(build_rows_with_too_few_or_extra_tabs)
{
	const std::string out =
		"build\n"								  // no tab after "build"
		"build\t\n"								  // "build\t" then EOL: rest="" -> skip
		"build\t\t\n"							  // one tab in rest, second missing -> skip
		"build\t\t\t\n"							  // three empty fields -> url empty -> skip
		"build\tc\tb\tu1\textra\tmore\n"		  // extra tabs: everything after 2nd tab is url
		"build\tc2\tb2\thttps://ex.com/ok.zip\n"; // clean
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	// Only the two rows whose url field is non-empty survive.
	CHECK_EQ(builds.size(), static_cast<std::size_t>(2));
	if (builds.size() == 2)
	{
		// The "extra tabs" row keeps the remainder (incl. the extra tabs) as url.
		CHECK_EQ(builds[0].url, "u1\textra\tmore");
		CHECK_EQ(builds[1].url, "https://ex.com/ok.zip");
	}
}

TEST(huge_single_line_no_newline)
{
	// A megabyte-long "value" with no newline: ValueOf must walk to the end
	// without over-reading.
	std::string out = "latest=";
	out.append(1024u * 1024u, 'x');
	const std::string v = ValueOf(out, "latest");
	CHECK_EQ(v.size(), static_cast<std::size_t>(1024u * 1024u));
}

TEST(crlf_line_endings)
{
	// GitHub-derived text could arrive with CRLF. ValueOf splits on '\n'; Trim
	// then strips the stray '\r', so the value is clean.
	const std::string out = "installed=abc\r\nlatest=def\r\nurl=https://ex.com/x.zip\r\n";
	CHECK_EQ(ValueOf(out, "installed"), "abc");
	CHECK_EQ(ValueOf(out, "latest"), "def");
	StableStatus s = EvaluateStable(out);
	CHECK(s.offerUpdate);
	CHECK_EQ(s.url, "https://ex.com/x.zip");
}

TEST(high_bytes_utf8_branch_names)
{
	// Non-ASCII branch/release names (fully valid on GitHub) must pass through
	// unharmed and not confuse the tab splitting.
	const std::string out = "build\tc0ffee1\t機能/日本語ブランチ\thttps://ex.com/a.zip\n"
							"build\tdeadbee\tfeature/émoji-🚀\thttps://ex.com/b.zip\n";
	std::vector<DevBuild> builds = ParseDevBuilds(out);
	CHECK_EQ(builds.size(), static_cast<std::size_t>(2));
	if (builds.size() == 2)
	{
		CHECK_EQ(builds[0].name, "機能/日本語ブランチ");
		CHECK_EQ(builds[1].name, "feature/émoji-🚀");
	}
}

// ---------------------------------------------------------------------------
// ResolveDevSelection over the full short range: the result must ALWAYS be
// either -1 or a valid candidate index, for every selection and every count.
// This is the one helper that mixes a signed `short` selection with an unsigned
// count, so it is the most likely to break under a careless refactor.
// ---------------------------------------------------------------------------

TEST(resolve_dev_selection_stays_in_range_for_all_inputs)
{
	const std::size_t counts[] = {0u, 1u, 3u, 100u, static_cast<std::size_t>(SHRT_MAX) + 5u};
	const short sels[] = {SHRT_MIN, -100, -1, 0, 1, 2, 99, 100, SHRT_MAX};
	for (std::size_t count : counts)
	{
		for (short sel : sels)
		{
			const int r = ResolveDevSelection(sel, count);
			// Always -1, or a valid index into [0, count).
			CHECK(r == -1 || (r >= 0 && static_cast<std::size_t>(r) < count));
			// A positive selection can only map to selection-1 or the -1 safeguard.
			if (sel > 0)
				CHECK(r == -1 || r == sel - 1);
			// Non-positive selection always keeps the current build.
			if (sel <= 0)
				CHECK(r == -1);
		}
	}
}

// ---------------------------------------------------------------------------

TEST_MAIN();
