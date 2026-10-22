// The Program store conformance suites against the JavaScript adapters, under Node (#556, #557).
//
// The WebAssembly module store_conformance is the suites of tests/ (ProgramTransitionStoreConformance,
// ProgramStoreConformance, the graph migration capsule row) built with googletest for the single-thread Emscripten
// build. Its JavaScript backend (tests/fixtures/program_store_backends.h) hands every case stores through the adapters
// of wasm/js_stores.h over the reference store of wasm/stores.mjs, so a case runs through the whole contract: C++ value,
// canonical text, JavaScript method, in-memory C++ store, and back. This script installs the store bridge and the
// two functions the deployment of the test module calls, runs the gtest main, prints a summary, and exits non-zero on
// any failure.
//
//   node wasm/test/store_conformance_test.mjs <directory with store_conformance.mjs, store_conformance.wasm and stores.mjs> [gtest flags]
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const directory = path.resolve(process.argv[2] ?? '.');
const flags = process.argv.slice(3);
const { default: createModule } = await import(pathToFileURL(path.join(directory, 'store_conformance.mjs')).href);
const { installStoreBridge, registerStores, releaseStores, memoryStores } = await import(
  pathToFileURL(path.join(directory, 'stores.mjs')).href);

const lines = [];
let exitCode;
const module = await createModule({
  noInitialRun: true,
  print: (text) => lines.push(text),
  printErr: (text) => lines.push(text),
  onExit: (code) => { exitCode = code; },
});
installStoreBridge(module);
// The deployment of a JavaScript-backed case asks for a registered reference store over an in-memory set it made.
module.ngTestRegisterStores = (set) => registerStores(module, memoryStores(module, { set }));
module.ngTestReleaseStores = (handle) => releaseStores(module, handle);

let returned;
try {
  returned = module.callMain(['--gtest_color=no', ...flags]);
} catch (error) {
  // The module trapped or threw out of a test: say where it was.
  console.log(lines.slice(-30).join('\n'));
  console.log(`store conformance aborted: ${error?.stack ?? error}`);
  process.exit(1);
}
const code = exitCode ?? returned ?? 1;
if (flags.some((flag) => flag.startsWith('--gtest_list_tests'))) {
  console.log(lines.join('\n'));
  process.exit(code);
}

// What the run did, from the googletest output: per backend and form, and the reasons for what was skipped.
const counts = new Map();
const skipReasons = new Map();
const failures = [];
let current = null;
// A parameterized case is `Instantiation/Suite.Test/Backend`; the Backend is a store backend, plus `Awaited` for the
// form that calls the awaitable operations.
const backendOf = (name) => {
  const match = /^[^/\s]+\/[^/\s]+\/([A-Za-z]+)/.exec(name);
  return match ? match[1] : 'not parameterized by backend';
};
const bump = (backend, kind) => {
  const row = counts.get(backend) ?? { passed: 0, skipped: 0, failed: 0 };
  row[kind] += 1;
  counts.set(backend, row);
};
for (const line of lines) {
  let match;
  if ((match = /^\[ RUN      \] (.*)$/.exec(line))) {
    current = { name: match[1], body: [] };
  } else if ((match = /^\[       OK \] (.*?)(?: \(\d+ ms\))?$/.exec(line))) {
    bump(backendOf(match[1]), 'passed');
    current = null;
  } else if ((match = /^\[  SKIPPED \] (.*?) \(\d+ ms\)$/.exec(line)) && current) {
    bump(backendOf(match[1]), 'skipped');
    const at = current.body.findIndex((text) => /: Skipped$/.test(text));
    const reason = (at < 0 ? '(no reason given)' : current.body.slice(at + 1).join(' ')).trim();
    const key = `${backendOf(match[1])}: ${reason}`;
    skipReasons.set(key, (skipReasons.get(key) ?? 0) + 1);
    current = null;
  } else if ((match = /^\[  FAILED  \] (.*?)(?: \(\d+ ms\))?$/.exec(line)) && current) {
    bump(backendOf(match[1]), 'failed');
    failures.push([...current.body, line].join('\n'));
    current = null;
  } else if (current) {
    current.body.push(line);
  }
}

const summary = lines.filter((line) => /^\[(==========|  PASSED  |  FAILED  |  SKIPPED )\]/.test(line) || /^ \d+ FAILED TEST/.test(line));
for (const failure of failures) console.log(failure);
console.log('--- per backend (passed / skipped / failed) ---');
for (const [backend, row] of [...counts].sort()) console.log(`${backend}: ${row.passed} / ${row.skipped} / ${row.failed}`);
console.log('--- skipped, by backend and reason ---');
for (const [reason, count] of [...skipReasons].sort()) console.log(`${count} x ${reason}`);
console.log('--- googletest summary ---');
console.log(summary.filter((line) => !/^\[  SKIPPED \] .*\//.test(line) || /^\[  FAILED  \]/.test(line)).join('\n'));
if (code !== 0 || failures.length > 0) {
  console.log(`store conformance failed (exit ${code})`);
  process.exit(code === 0 ? 1 : code);
}
console.log('all checks passed');
