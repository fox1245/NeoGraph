// Types of wasm/neograph.mjs, the JavaScript API of the NeoGraph WebAssembly module.

/** A provider the module calls through the host's `fetch`. `baseUrl` and `path`, or a `descriptor`, name the endpoint. */
export interface ProviderConfig {
  baseUrl?: string;
  path?: string;
  /** Wire protocol family; `openai.chat` unless given. */
  family?: string;
  /** The SDK descriptor itself, in place of `baseUrl`, `path` and `family`. */
  descriptor?: object;
  paths?: { buffered: string; streaming: string };
  /**
   * The model catalogue (`{ version: 1, models: [...] }`, the SDK's format), laid over the embedded one. A run of a Program
   * calls a model only when the catalogue gives its input and output limits.
   */
  catalogue?: object;
  id?: string;
  apiKey?: string;
  model: string;
  instructions?: string;
  maxOutputTokens?: number;
  temperature?: number;
}

export interface NeoGraphErrorDetail {
  type?: string;
  message?: string;
  kind?: string;
  reason?: string;
  http_status?: number;
  code?: string;
  diagnostics?: ProgramDiagnostic[];
  witness?: unknown;
}

/**
 * A failed run, an invalid graph or provider, or a refused Program call. `type` is "provider", "cancelled" (also a resume
 * or a fork that was cancelled before it was sent), "error", "compile" or "admission" (a refused Program source: see
 * `diagnostics`), "program" (a runtime refusal, see `code`) or "refused" (a call that the host loop cannot wait for).
 */
export class NeoGraphError extends Error {
  constructor(detail: NeoGraphErrorDetail);
  type: string;
  kind?: string;
  reason?: string;
  httpStatus?: number;
  code?: string;
  diagnostics?: ProgramDiagnostic[];
  witness?: unknown;
}

export interface ProgramDiagnostic {
  code: string;
  message: string;
  /** JSON pointer into the source document the diagnostic is about. */
  pointer: string;
  witness: unknown;
}

export interface GraphEvent {
  type: string;
  node: string;
  data: unknown;
}

export interface GraphRunOptions {
  onEvent?: (event: GraphEvent) => void;
  signal?: AbortSignal;
  timeoutMs?: number;
}

export interface GraphResult {
  status: string;
  text: string;
  output: any;
  trace: unknown;
  usage: unknown;
}

export interface Graph {
  run(input?: object, options?: GraphRunOptions): Promise<GraphResult>;
  release(): void;
}

// ---- Programs ----

/** A source for `Programs.admit`: the text of a JavaScript control source, or an object naming it. */
export type ProgramSource =
  | string
  | { javascript: string; name?: string }
  | { document: object; name?: string };

/** An admitted version of a Program. Pass it, or its `id`, to `Programs.start`. */
export interface ProgramVersion {
  id: string;
  bundleId: string;
  name: string;
}

/**
 * What a run may spend. Every member is optional where a budget is given; one that is left out keeps the default of the
 * Programs object.
 */
export interface RunBudget {
  /** Wall time in milliseconds. */
  wallTimeMs?: number;
  modelTokens?: number;
  monetaryMicrounits?: number;
  maxConcurrency?: number;
  maxProgramOperations?: number;
  maxCoreSteps?: number;
  maxDynamicCompiles?: number;
  maxChildDepth?: number;
  maxTotalChildren?: number;
}

export interface ProgramUsage {
  wallTimeMs: number;
  modelTokens: number;
  programOperations: number;
  coreSteps: number;
}

export interface ProgramInterrupt {
  /** The graph node that raised the interrupt. */
  node: string;
  /** What the node asks. */
  value: unknown;
  kind?: 'input' | 'effect';
  /** Names the pending question. `ProgramRun.resume` and `fork` answer it. */
  pendingId?: string;
}

export interface ProgramFailure {
  code: string;
  message: string;
  node: string;
  attempts: number;
  witness: unknown;
}

export type ProgramStatus = 'completed' | 'failed' | 'cancelled' | 'interrupted' | (string & {});

/** How a run ended or where it waits. */
export interface ProgramResult {
  runId: string;
  versionId: string;
  status: ProgramStatus;
  output: any;
  trace: unknown;
  usage: ProgramUsage;
  remainingBudget: Required<RunBudget>;
  /** Present when the run waits at an interrupt. */
  checkpointId?: string;
  interrupt?: ProgramInterrupt;
  failure?: ProgramFailure;
}

/** What `Programs.result` finds for a run that has no end result yet. */
export interface ProgramRunning {
  runId: string;
  status: 'running';
}

/** An event of a run, as the runtime journals it. */
export interface ProgramEvent {
  [member: string]: any;
}

export interface ProgramRunOptions {
  /** Called with each event of the run, in order. If it throws, the run is cancelled and its result rejects with the error. */
  onEvent?: (event: ProgramEvent) => void;
  /** Cancels the call while it is pending: when it aborts, or if it was aborted already, the call is cancelled or not made. Once the call has settled (a run that paused, say) it has no effect: use `cancel()`. */
  signal?: AbortSignal;
}

export interface StartOptions extends ProgramRunOptions {
  input?: object;
  budget?: RunBudget;
  /** The run's id. Generated when left out. */
  runId?: string;
}

export interface ForkOptions extends ProgramRunOptions {
  input?: object;
  /** Overrides what the fork takes of the paused run's remaining budget; it must leave the paused run some wall time and Core steps. */
  budget?: RunBudget;
  /** The id of the fork. Generated when left out. */
  runId?: string;
}

export interface ProgramRun {
  /** The run's identity. A resumed run keeps it; a fork has its own. */
  readonly id: string;
  readonly versionId: string;
  /** Resolves when the run ends or waits for an answer. A failed or cancelled run resolves with that status. */
  readonly result: Promise<ProgramResult>;
  /** Answers the interrupt the run waits at and continues it. */
  resume(value?: unknown, options?: ProgramRunOptions): ProgramRun;
  /** Forks the run at the checkpoint it waits at, answering the interrupt in the fork. The paused run stays resumable. */
  fork(value?: unknown, options?: ForkOptions): ProgramRun;
  /**
   * Resolves with whether this call cancelled the run (false when it had already ended). A resume or a fork that has not
   * been sent yet, because it waits for the run it continues, is withdrawn instead: its `result` rejects with a
   * "cancelled" NeoGraphError.
   */
  cancel(): Promise<boolean>;
}

/**
 * The stores of the host. Each member is optional; one that is left out is the module's own in-memory store. Every method
 * may return its value or a Promise of it. See wasm/README.md for the methods and the shape of their values.
 */
export interface ProgramStores {
  transitions?: object;
  programs?: object;
  grants?: object;
  fragments?: object;
  modules?: object;
}

/** The in-memory stores of the module, as stores of the host: `programs({ stores })` takes them. Methods return values. */
export interface MemoryStores extends ProgramStores {
  /** Drops the set. Do that when the runs on it have ended: a call on a dropped set fails. */
  release(): void;
}

export interface ProgramsConfig {
  provider: ProviderConfig;
  /** The owner scope of the Programs and runs. */
  owner?: string;
  stores?: ProgramStores;
  /** The most any admitted Program may be given. */
  limits?: RunBudget;
  /** The budget of a run that gives none. */
  budget?: RunBudget;
}

export interface Programs {
  /** Compiles and admits a Program. Rejects with a NeoGraphError of type "compile" or "admission". */
  admit(source: ProgramSource): Promise<ProgramVersion>;
  /** Starts a run. Returns at once; throws when `version` is missing, and rejects `result` when it names no version. */
  start(version: string | { id: string }, options?: StartOptions): ProgramRun;
  /** Starts a run and resolves with its result. */
  run(version: string | { id: string }, options?: StartOptions): Promise<ProgramResult>;
  /** What the stores hold of a run: its latest result, `{ runId, status: 'running' }` while it has none, or null. */
  result(runId: string): Promise<ProgramResult | ProgramRunning | null>;
  /** Cancels the runs that are executing and frees the object. A run that waits at an interrupt stays as it is. */
  release(): void;
}

export interface LoadOptions {
  wasmModule?: WebAssembly.Module;
  wasmBinary?: ArrayBuffer | Uint8Array;
  locateFile?: (name: string, dir: string) => string;
}

export interface NeoGraph {
  /** Programs over a provider and a set of stores. Throws when the module was built without Program support. */
  programs(config: ProgramsConfig): Programs;
  /** The reference store: the module's in-memory stores through the contract of `programs({ stores })`. */
  memoryStores(): MemoryStores;
  compile(graph: object | string, provider: ProviderConfig): Graph;
  run(options: { graph: object | string; provider: ProviderConfig; input?: object } & GraphRunOptions): Promise<GraphResult>;
}

export function load(options?: LoadOptions): Promise<NeoGraph>;
