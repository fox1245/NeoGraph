import assert from 'node:assert/strict'
import path from 'node:path'
import { pathToFileURL } from 'node:url'

const modulePath = path.resolve(process.argv[2] ?? 'build/wasm-release/neograph.js')
const createNeoGraphModule = (await import(pathToFileURL(modulePath).href)).default
const runtime = await createNeoGraphModule()

assert.match(runtime.version(), /^\d+\.\d+\.\d+$/)

function assertCppThrows(callback, expected) {
  let thrown
  try {
    callback()
  } catch (error) {
    thrown = error
  }
  assert.ok(thrown, 'Expected a C++ exception')
  const [, message] = runtime.getExceptionMessage(thrown)
  runtime.decrementExceptionRefcount(thrown)
  assert.match(message, expected)
}

runtime.registerNodeType('double', (encoded) => {
  const request = JSON.parse(encoded)
  const seed = request.state.channels.seed.value
  return JSON.stringify({
    writes: [{ channel: 'doubled', value: seed * 2 }],
  })
})
assert.deepEqual(JSON.parse(runtime.registeredNodeTypes()), ['double'])

const graph = new runtime.Graph(JSON.stringify({
  name: 'wasm-smoke',
  channels: {
    seed: { reducer: 'overwrite' },
    doubled: { reducer: 'overwrite' },
  },
  nodes: {
    double: { type: 'double' },
  },
  edges: [
    { from: '__start__', to: 'double' },
    { from: 'double', to: '__end__' },
  ],
}))

try {
  const result = JSON.parse(graph.run(JSON.stringify({
    threadId: 'wasm-smoke',
    input: { seed: 21 },
  })))
  assert.equal(result.output.channels.doubled.value, 42)
  assert.deepEqual(result.executionTrace, ['double'])
} finally {
  graph.delete()
  runtime.unregisterNodeType('double')
  assert.deepEqual(JSON.parse(runtime.registeredNodeTypes()), [])
}

runtime.registerNodeType('approval', (encoded) => {
  const request = JSON.parse(encoded)
  if (!request.context.resumeValue) {
    return JSON.stringify({
      interrupt: {
        reason: 'Approval required',
        value: { action: 'publish' },
      },
    })
  }
  return JSON.stringify({
    writes: [{
      channel: 'approved',
      value: request.context.resumeValue.approved,
    }],
  })
})

const approvalGraph = new runtime.Graph(JSON.stringify({
  name: 'wasm-hitl-smoke',
  channels: {
    approved: { reducer: 'overwrite', initial: false },
  },
  nodes: {
    approval: { type: 'approval' },
  },
  edges: [
    { from: '__start__', to: 'approval' },
    { from: 'approval', to: '__end__' },
  ],
}))

try {
  const paused = JSON.parse(approvalGraph.run(JSON.stringify({
    threadId: 'wasm-hitl',
    input: {},
  })))
  assert.equal(paused.interrupted, true)
  assert.equal(paused.interruptNode, 'approval')
  assertCppThrows(
    () => approvalGraph.resume('wasm-hitl', 'null'),
    /resume value must not be null/,
  )

  const resumed = JSON.parse(approvalGraph.resume(
    'wasm-hitl',
    JSON.stringify({ approved: true }),
  ))
  assert.equal(resumed.interrupted, false)
  assert.equal(resumed.output.channels.approved.value, true)
} finally {
  approvalGraph.delete()
  runtime.unregisterNodeType('approval')
}

console.log('NeoGraph WASM smoke passed')
