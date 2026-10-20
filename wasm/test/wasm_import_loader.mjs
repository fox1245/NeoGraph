// A Node module-loader hook that makes `import module from './x.wasm'` give a WebAssembly.Module, as the bundler of a
// Cloudflare Worker does. It lets the Worker example run in plain Node, in the test that calls its fetch handler.
import { fileURLToPath } from 'node:url';

export async function load(url, context, nextLoad) {
  if (!url.endsWith('.wasm')) return nextLoad(url, context);
  const file = JSON.stringify(fileURLToPath(url));
  return {
    format: 'module',
    shortCircuit: true,
    source: `import { readFileSync } from 'node:fs'; export default new WebAssembly.Module(readFileSync(${file}));`,
  };
}
