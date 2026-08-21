# HTML/CSS migrator

The migrator scans a local HTML file into a deterministic intermediate
representation and can generate a QML skeleton plus a source-located report.
It is an aid for a developer review, not a browser or a security boundary.
Generated QML is not trusted, signed, installed, or declared compatible
automatically.

Install with lifecycle scripts disabled, then build and test:

```powershell
$cache = (Resolve-Path build).Path + '\npm-cache'
npm.cmd ci --prefix tools --ignore-scripts --cache $cache
npm.cmd run build --prefix tools --ignore-scripts
npm.cmd test --prefix tools --ignore-scripts
```

Run the compiled CLI:

```powershell
node tools\migrator\dist\cli.js scan fixtures\migration\dashboard\index.html `
  --json build\migration\dashboard.ir.json
node tools\migrator\dist\cli.js generate fixtures\migration\dashboard\index.html `
  --output build\migration\dashboard-qml `
  --report build\migration\dashboard-report.json
```

The scanner preserves source locations and reports unsupported selectors,
layout, scripting, forms, media, embedded/remote content, and other features.
Fatal diagnostics block generation and return exit code 2. CLI usage errors
return 64. Output and report paths must be distinct, new, safe local paths;
publication is transactional and refuses overwrite. Review every diagnostic,
replace browser-only behavior with brokered Runtime calls, restrict imports,
add tests, then package and sign the reviewed result.
