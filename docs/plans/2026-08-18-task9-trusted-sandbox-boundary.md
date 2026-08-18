# Task 9 Trusted Sandbox Boundary Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make Windows sandbox launch paths valid by construction, grant the complete minimal trusted Qt runtime closure, and preserve exact native failure codes.

**Architecture:** A move-only `SandboxTrustBoundary` factory validates and holds stable handles for Host-owned package-store, sandbox-temp, and immutable runtime roots. Only this boundary can construct a launch configuration, and `SandboxLauncher` revalidates the retained identities before applying transactional ACL grants. A dynamically Qt6Core-linked helper staged with its minimal DLL closure proves that the zero-capability LPAC can load the real runtime and handshake.

**Tech Stack:** C++20, Qt 6.11.1 Core/Test/Network, CMake 3.30, MSVC 2022, Windows AppContainer, Win32 security descriptors and stable file handles, Qt Test.

---

### Task 1: Exact native error results

**Files:**
- Create: `runtime/sandbox/windows/SandboxError.h`
- Modify: `runtime/sandbox/windows/AppContainerProfile.h`
- Modify: `runtime/sandbox/windows/AppContainerProfile.cpp`
- Modify: `runtime/sandbox/windows/AclGrant.h`
- Modify: `runtime/sandbox/windows/AclGrant.cpp`
- Modify: `runtime/sandbox/windows/JobLimits.h`
- Modify: `runtime/sandbox/windows/JobLimits.cpp`
- Modify: `runtime/sandbox/windows/SandboxLauncher.h`
- Modify: `runtime/sandbox/windows/SandboxLauncher.cpp`
- Test: `tests/security/tst_sandbox_launcher.cpp`

**Step 1: Write failing tests**

Add assertions that invalid profile input returns `Win32/ERROR_INVALID_NAME`, an invalid SID returns `Win32/ERROR_INVALID_SID`, a missing ACL target returns the exact `GetLastError()` value, invalid Job limits return `Win32/ERROR_INVALID_PARAMETER`, and launch propagates the exact subordinate error instead of the ambient last-error slot.

**Step 2: Verify RED**

Run:

```powershell
& $CMake --build build\task9-fresh-debug --config Debug --target tst_sandbox_launcher -- /m:1 /nr:false
& $CTest --test-dir build\task9-fresh-debug -C Debug -R '^sandbox_launcher$' --output-on-failure
```

Expected: compile failure because the typed result/error API does not exist.

**Step 3: Implement minimal result types**

Add `SandboxNativeErrorKind { None, Win32, HResult }` and `SandboxNativeError { kind, value }`. Replace `optional`-only factory returns with result structures containing the value and exact error captured at the failing API call. Convert validation failures to explicit documented Win32 values; never consult `GetLastError()` after a helper has returned.

**Step 4: Verify GREEN**

Run the targeted build/test command and require all sandbox tests to pass.

### Task 2: Valid-by-construction trusted roots

**Files:**
- Create: `runtime/sandbox/windows/SandboxTrustBoundary.h`
- Create: `runtime/sandbox/windows/SandboxTrustBoundary.cpp`
- Modify: `runtime/sandbox/windows/CMakeLists.txt`
- Modify: `runtime/sandbox/windows/SandboxLauncher.h`
- Modify: `runtime/sandbox/windows/SandboxLauncher.cpp`
- Test: `tests/security/tst_sandbox_launcher.cpp`

**Step 1: Write failing boundary tests**

Cover rejection of volume roots, broad ancestors, equal/nested/overlapping roots, leaf or ancestor reparse points, non-Host ownership, broad write ACEs, package/temp values outside approved roots, equal-to-root rather than strict descendants, executable outside runtime roots, and changed stable identity. Include `C:\` containment cases. Snapshot DACLs before every rejected construction/request and assert byte-for-byte equality afterward.

**Step 2: Verify RED**

Run the targeted sandbox test. Expected: compile failure because `SandboxTrustBoundary` and boundary-only configuration construction do not exist.

**Step 3: Implement the factory and stable state**

The factory must:

- normalize paths without breaking a trailing volume separator;
- reject volume roots and roots that contain known broad locations such as Windows, Program Files, the user profile, application directory, or current directory;
- open every ancestor with `FILE_FLAG_OPEN_REPARSE_POINT` and reject reparse points;
- hold native root handles, final paths, volume/file identities, owner SID, and DACL validation evidence;
- require Host/SYSTEM ownership and prove no Worker/AppContainer or broad principal has write/delete/DACL/owner rights;
- reject all root equality, nesting, or overlap.

`makeLaunchConfig` accepts only strict descendant package/temp paths and an executable inside one runtime closure root. It opens and records the selected descendants before returning. `SandboxLaunchConfig` has no public raw-path constructor; it retains shared stable state so launcher revalidation is mandatory immediately before any ACL mutation.

**Step 4: Verify GREEN**

Run the targeted sandbox test and require every rejection plus no-mutation assertion to pass.

### Task 3: Minimal dynamic Qt runtime closure

**Files:**
- Create: `tests/helpers/qt_sandbox_probe/CMakeLists.txt`
- Create: `tests/helpers/qt_sandbox_probe/main.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/security/CMakeLists.txt`
- Modify: `tests/security/tst_sandbox_launcher.cpp`
- Modify: `runtime/sandbox/windows/SandboxLauncher.cpp`

**Step 1: Write the failing dynamic launch test**

Build a helper linked to `Qt6::Core`. In the test, create a protected Host/SYSTEM-only runtime staging directory, copy only the helper and `$<TARGET_FILE:Qt6::Core>` into it, and construct a trust boundary with that directory as the runtime closure. Launch the staged helper and assert an IPC JSON handshake containing the Qt version, `TokenIsAppContainer=true`, LPAC semantics, and a successfully queried capability count of zero. Assert the runtime-root DACL is restored after process destruction.

**Step 2: Verify RED**

Run the dynamic helper test. Expected: boundary/config compilation failure or process-load failure because no runtime closure grant exists.

**Step 3: Implement transactional runtime grants**

Before process creation, revalidate all retained root and selected-path identities. Grant recursive read/execute access to each minimal immutable runtime closure root, read-only access to the package version, and read/write access only to worker temp. Validate all relations before the first grant. Keep all grants in `SandboxProcess`, restore in reverse order, and return the exact grant failure if any step fails.

**Step 4: Verify GREEN**

Run the targeted test and require both the static negative probe and dynamic Qt helper to pass under zero capabilities.

### Task 4: Documentation and final verification

**Files:**
- Modify: `docs/plans/2026-08-18-q-browser-mvp.md`

**Step 1: Update Task 9**

Document the trust-boundary factory, stable root/identity/owner/DACL checks, strict descendants, minimal runtime closure, dynamic Qt proof, and exact native error propagation.

**Step 2: Run fresh verification**

Run a fresh Debug configure/build, targeted sandbox tests, all tests, all tests with a polluted `PATH`, a Release build, and a Release `BUILD_TESTING=OFF` build. Confirm no test helper, test macro, or hook is present in production outputs.

**Step 3: Review and commit**

Run `git diff --check`, request an independent Task 9 code review, fix all Critical/Important findings, rerun affected plus full verification, and commit only the Task 9 quality changes.
