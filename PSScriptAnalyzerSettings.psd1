# PSScriptAnalyzer configuration — https://learn.microsoft.com/powershell/utility-modules/psscriptanalyzer/overview
#
# Static analysis for the Windows updater script (scripts/vw-update.ps1). This is
# the PowerShell analogue of what clang-tidy does for the C/C++ updater logic and
# shellcheck does for the bash scripts: it flags bug-prone patterns, unapproved
# cmdlet verbs, unused parameters, unsafe comparisons and the like, BEFORE they
# ship. CI (.github/workflows/lint.yml) runs it with every finding treated as an
# error, and scripts/lint.sh runs the exact same check locally.
#
# Like clang-tidy (which lints the SDK-free src/ logic, not the tests) and
# shellcheck (which lints scripts/*.sh, not tests/*.sh), the analyzer is pointed
# at the production updater script under scripts/, not the test harness under
# tests/ — a test double legitimately overrides built-in cmdlets, leaves stub
# parameters unused and calls helpers positionally, none of which is a defect.
#
# The full default rule set is enabled; only the handful of rules that conflict
# with this script's deliberate, documented design are excluded, each with its
# reason below. Everything else — the rules that catch real mistakes — stays on.

@{
    # Run every built-in rule (this is the default, stated explicitly).
    IncludeDefaultRules = $true

    # Report findings at every severity, so style-level (Information) issues are
    # surfaced too, not just Warnings and Errors.
    Severity = @('Error', 'Warning', 'Information')

    ExcludeRules = @(
        # The interactive, run-from-a-terminal fallback (Invoke-Stable / Invoke-Dev
        # and the top-level menu) is a deliberate human-facing console UI: it writes
        # coloured prompts and status with Write-Host -ForegroundColor. This is the
        # intended use of Write-Host. The machine-readable back end the plug-in
        # consumes already uses Write-Output for its parseable key=value / TSV lines.
        'PSAvoidUsingWriteHost'

        # The empty catch blocks are intentional best-effort operations that must
        # NOT abort the caller: optional TLS 1.2 / UTF-8 console setup that older
        # hosts may reject, and the rename/remove cleanup around an in-use .vlb (a
        # locked file that cannot be deleted is expected, not an error). Each is
        # explained in a comment at its call site.
        'PSAvoidUsingEmptyCatchBlock'

        # The repository standard is UTF-8 WITHOUT a BOM (enforced for every text
        # file by .editorconfig / editorconfig-checker), and the script itself sets
        # UTF-8-no-BOM console output on purpose so the plug-in reads its Japanese
        # messages without mojibake. A BOM would fight both.
        'PSUseBOMForUnicodeEncodedFile'
    )
}
