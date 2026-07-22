//
//	TestFramework.h
//
//	A tiny, dependency-free unit-test harness. It intentionally pulls in nothing
//	beyond the C++ standard library so the tests build on any toolchain WITHOUT
//	the Vectorworks SDK (or a fetched test framework) — which is exactly what
//	lets them run in a fast, SDK-free coverage job.
//
//	Usage:
//	  #include "TestFramework.h"
//	  TEST(my_case) {
//	      CHECK(1 + 1 == 2);
//	      CHECK_EQ(std::string("a"), "a");
//	  }
//	  // exactly one translation unit provides main():
//	  TEST_MAIN();
//
//	Each TEST registers itself; TEST_MAIN() runs them all, prints a summary, and
//	exits non-zero if any CHECK failed (so CTest reports the failure).
//

#pragma once

#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace vwtest
{
	// One registered test: a name and the function that runs its assertions.
	struct TestCase
	{
		std::string					name;
		void						(*fn)(int& failures);
	};

	// Single global registry. Function-local static so there is no static-init
	// order problem between translation units.
	inline std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> reg;
		return reg;
	}

	// Registering one test is a side effect of constructing this object, so a
	// `static Registrar` at file scope adds the test before main() runs.
	struct Registrar
	{
		Registrar(const char* name, void (*fn)(int&))
		{
			Registry().push_back(TestCase{ name, fn });
		}
	};

	// Run every registered test. Returns the number of tests that had at least
	// one failed assertion.
	inline int RunAll()
	{
		int failedTests = 0;
		std::size_t total = Registry().size();
		for (const TestCase& tc : Registry())
		{
			int failures = 0;
			tc.fn(failures);
			if (failures == 0)
			{
				std::cout << "[ PASS ] " << tc.name << "\n";
			}
			else
			{
				std::cout << "[ FAIL ] " << tc.name
						  << " (" << failures << " assertion(s) failed)\n";
				++failedTests;
			}
		}
		std::cout << "\n"
				  << (total - static_cast<std::size_t>(failedTests)) << "/" << total
				  << " test(s) passed.\n";
		return failedTests;
	}
}

// --- Assertion macros --------------------------------------------------------
// They operate on the enclosing test's `failures` counter (see TEST below) and
// print a file:line message so a failure points straight at the offending line.

#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) {                                                         \
			++failures;                                                        \
			std::cout << "    CHECK failed: " << #cond                         \
					  << " @ " << __FILE__ << ":" << __LINE__ << "\n";         \
		}                                                                      \
	} while (0)

#define CHECK_EQ(a, b)                                                         \
	do {                                                                       \
		auto _va = (a);                                                        \
		auto _vb = (b);                                                        \
		if (!(_va == _vb)) {                                                   \
			++failures;                                                        \
			std::ostringstream _oss;                                           \
			_oss << "    CHECK_EQ failed: " << #a << " == " << #b              \
				 << " @ " << __FILE__ << ":" << __LINE__ << "\n"               \
				 << "        lhs = [" << _va << "]\n"                          \
				 << "        rhs = [" << _vb << "]\n";                         \
			std::cout << _oss.str();                                           \
		}                                                                      \
	} while (0)

// Define a test. The body receives an `int& failures` (used by CHECK*).
#define TEST(name)                                                             \
	static void test_##name(int& failures);                                    \
	static ::vwtest::Registrar registrar_##name(#name, &test_##name);          \
	static void test_##name(int& failures)

// Provide main() in exactly one translation unit.
#define TEST_MAIN()                                                            \
	int main()                                                                 \
	{                                                                          \
		return ::vwtest::RunAll() == 0 ? 0 : 1;                                \
	}
