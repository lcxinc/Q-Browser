# Release Script Line-Ending Test Design

## Problem

The release-script acceptance test compares two adjacent C# statements using
a string literal containing an LF line ending. On Windows, a clean checkout
converts `scripts/build-release.ps1` to CRLF because `core.autocrlf` is enabled
and the script has no explicit `eol` attribute. The assertion therefore fails
even though the statements and their ordering are unchanged.

## Decision

Keep the repository checkout policy and production script unchanged. Replace
the line-ending-sensitive substring assertion with a regular expression that
accepts either LF or CRLF while still requiring the same two statements to be
adjacent with the expected indentation.

This is narrower than forcing PowerShell scripts to LF through
`.gitattributes`, and narrower than normalizing every script read in the test
suite.

## Verification

The existing failing test is the RED reproduction. After the assertion change,
run that test file in isolation and then run the complete tools test suite.
The C++/Qt test suite does not consume this TypeScript test source and has
already passed on the merged commit.
