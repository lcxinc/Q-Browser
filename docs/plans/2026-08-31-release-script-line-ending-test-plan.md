# Release Script Line-Ending Test Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make the release-script acceptance test pass for both LF and CRLF clean checkouts without changing production behavior or repository line-ending policy.

**Architecture:** Keep `scripts/build-release.ps1` and `.gitattributes` unchanged. Replace one literal substring assertion with a regular expression that permits only an optional carriage return before the newline while preserving the exact adjacent C# statements and indentation.

**Tech Stack:** TypeScript, Vitest, Node.js 24, npm workspaces

---

### Task 1: Make the release-script assertion line-ending agnostic

**Files:**
- Modify: `tools/migrator/test/package.test.ts:113-115`
- Test: `tools/migrator/test/package.test.ts`

**Step 1: Confirm the failing clean-checkout behavior**

Run from `tools`:

```powershell
& 'C:\Program Files\nodejs\npx.cmd' vitest run migrator/test/package.test.ts --pool=threads --maxWorkers=1
```

Expected: FAIL in `revalidates focus and bounds retries for a missed native file click` because the current LF-only string does not match CRLF input.

**Step 2: Replace the assertion with the minimal cross-platform form**

Change the assertion to:

```typescript
expect(script).toMatch(
  /if \(!FocusWorker\(worker\)\) return false;\r?\n      Input down = new Input\(\); down\.type = InputMouse;/u,
);
```

Do not modify the release script, `.gitattributes`, or unrelated assertions.

**Step 3: Run the focused test**

Run from `tools`:

```powershell
& 'C:\Program Files\nodejs\npx.cmd' vitest run migrator/test/package.test.ts --pool=threads --maxWorkers=1
```

Expected: the test file passes.

**Step 4: Run the complete tools test suite**

Run from `tools`:

```powershell
& 'C:\Program Files\nodejs\npm.cmd' test
```

Expected: all 9 test files and all 172 tests pass.

**Step 5: Check and commit the focused change**

```powershell
git diff --check
git add -- tools/migrator/test/package.test.ts
git commit -m "test: accept Windows line endings in release checks"
```

Expected: the commit contains only the assertion change.
