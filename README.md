# Agent Runtime

Agent Runtime is the execution lifecycle authority for a long-running autonomous agent.

It answers one systems question:

> What may this autonomous agent do next, under which current model, tool, memory,
> checkpoint, budget, lifecycle, and execution authority — and how does that agent
> survive retries, interruption, process death, restart, and recovery without stale
> actions, duplicate side effects, or invalid state becoming authoritative?

The core runtime lifecycle is:

```
instantiate agent execution
  -> bind current authority
  -> admit next action
  -> classify action
  -> validate dependencies and policy
  -> reserve/authorize action
  -> dispatch model/tool/internal step
  -> observe completion
  -> validate completion authority
  -> commit durable progress
  -> checkpoint where required
  -> continue / retry / suspend / recover / terminate
```

The central runtime guarantee:

> An autonomous agent may advance durable progress only from a completion that is
> still authoritative under the current runtime, run, action, attempt, process,
> policy, budget, model/tool, memory, checkpoint, and scheduler authority relevant
> to that action.

When the system cannot know whether a non-repeatable external side effect occurred,
Agent Runtime preserves that uncertainty instead of inventing safety.

## Product boundary

Agent Runtime owns:

- persistent agent-execution identity;
- agent-run generation;
- runtime incarnation (boot);
- execution lifecycle;
- step/action identity;
- model-call lifecycle;
- tool-call lifecycle;
- memory-binding references;
- checkpoint bindings;
- retry policy and attempt lineage;
- budget-consumption gates;
- action authorization;
- result acceptance;
- side-effect commit authority;
- suspension, resume, runtime restart, conservative recovery;
- cancellation, drain, terminal outcome;
- deterministic inspection and explanations.

Agent Runtime does not decide which persistent agent receives work, which model
provider is selected, how a generic distributed execution attempt is fenced, how a
generic workload lifecycle is stored, how checkpoint bytes are captured, or how a
population of agents is coordinated. Those belong to adjacent systems.

### Relationship to Agent Scheduler

Agent Scheduler answers *which persistent agent should own this work now*.
Agent Runtime answers *now that this agent owns the work, what may its execution do
next, and how does that execution survive interruption and recovery*.

Agent Runtime consumes scheduling authority through a narrow input contract —
`SchedulerAssignmentId`, `AssignmentGeneration`, `AgentId`, `AgentGeneration`,
`AgentBootId`, `SchedulerEpoch`, `CoordinatorEpoch`, `DispatchGeneration`,
lease identity/generation, `WorkId`/`WorkGeneration` — and rejects stale or
superseded assignment authority. A runtime does not continue merely because it once
received an assignment.

Agent Runtime never performs candidate discovery, agent ranking, queue fairness,
scheduler admission, assignment selection, capability ranking or placement policy.

### Relationship to adjacent systems

| Adjacent system | Consumed through | Agent Runtime does not |
| --- | --- | --- |
| Execution Fabric | `ExecutionAuthorityProvider` | rebuild a generic distributed execution substrate |
| Workload Fabric | `WorkId` / `WorkGeneration` binding | duplicate generic workload lifecycle |
| Checkpoint Fabric | `CheckpointProvider` | capture, validate or store checkpoint bytes |
| Context Fabric / State Index / Distributed Cache Directory | `MemoryProvider` | own distributed state identity or placement |
| Model Router | externally resolved target + `ModelBackend` | rank providers or choose models |
| Cost Governor | `BudgetProvider` | reproduce economic policy |
| SLO Fabric | policy/deadline gate | own service-level contract policy |
| Resource Broker | `BackendIncarnation` identity | reserve physical resources |
| Ensemble Fabric | — | coordinate ensembles, voting or consensus |
| Critic Fabric | tool-like external capability | decide critique truth globally |
| Experiment Fabric / Lab Scheduler / Research Ledger / Artifact Promotion | narrow data contracts | become the experiment authority |
| Autonomous Foundry | this library as a primitive | become the Foundry |

Agent Runtime builds and functions without any other repository. Every adjacent
system is consumed through one narrow adapter or data contract; there is no
monolithic dependency chain.

## Runtime, run, step, action, attempt

One `AgentRuntime` instance governs one logical long-running agent execution
context. Several independent instances may live in one process.

| Concept | Identity | Notes |
| --- | --- | --- |
| Runtime instance | `AgentRuntimeId` + `AgentRuntimeGeneration` | one execution context |
| Runtime incarnation | `RuntimeBootId` | process authority; fenced when superseded |
| Agent | `AgentId` + `AgentGeneration` + `AgentBootId` | supplied by the caller |
| Epochs | `RuntimeEpoch`, `CoordinatorEpoch` | advance on recovery |
| Run | `AgentRunId` + `AgentRunGeneration` | one logical execution of the agent |
| Step | `StepId` + `StepGeneration` | durable progress boundary |
| Action | `ActionId` + `ActionGeneration` | one meaningful operation |
| Attempt | `ActionAttemptId` + `AttemptGeneration` | one physical attempt |
| Model call | `ModelCallId` + `ModelCallGeneration` | governed like any action |
| Tool call | `ToolCallId` + `ToolCallGeneration` | governed like any action |
| Memory binding | `MemoryBindingId` + generation | generation-bound reference |
| Checkpoint binding | `CheckpointBindingId` + generation | runtime-side lifecycle only |
| Result | `ResultId` + `CompletionGeneration` | one accepted completion |

Identities are strongly typed: `EntityId<Tag>` and `Generation<Tag>` cannot be
interchanged across semantics, and generations only move forward.

### Lifecycle

`DECLARED`, `INITIALIZING`, `READY`, `RUNNING`, `WAITING_MODEL`, `WAITING_TOOL`,
`WAITING_EXTERNAL`, `CHECKPOINTING`, `SUSPENDING`, `SUSPENDED`, `RECOVERING`,
`REVALIDATION_REQUIRED`, `DRAINING`, `CANCELLING`, `CANCELLED`, `COMPLETED`,
`FAILED`, `RETIRED`.

WAITING, SUSPENDED, LOST, FAILED and CANCELLED are never collapsed into one state.
Legal transitions are explicit and a transition outside the table is a caller
defect, not a recoverable runtime condition.

### Action lifecycle

`DECLARED`, `ADMITTED`, `AUTHORIZED`, `DISPATCHED`, `IN_FLIGHT`,
`COMPLETED_UNVALIDATED`, `COMMITTED`, `RETRY_PENDING`, `SUSPENDED`, `CANCELLED`,
`FAILED`, `SUPERSEDED`, `REVALIDATION_REQUIRED`.

A completed external action never becomes `COMMITTED` until its completion is
validated against current authority. Dispatch is not completion, and completion is
not authority.

## Side-effect semantics

Every action carries an explicit side-effect class:

| Class | Meaning |
| --- | --- |
| `PURE` | no externally observable effect |
| `READ_ONLY` | reads external state only |
| `IDEMPOTENT` | safe to repeat with the same operation key |
| `DEDUPLICATABLE` | backend deduplicates on a stable operation key |
| `COMMIT_TOKEN_REQUIRED` | prepare, receive authority/token, perform, verify, commit receipt |
| `NON_REPEATABLE` | repeating may cause a second real side effect |
| `UNKNOWN` | conservative: treated as neither repeatable nor effect-free |

`UNKNOWN`, `NON_REPEATABLE` and `COMMIT_TOKEN_REQUIRED` require external
reconciliation when completion is ambiguous. `PURE`/`READ_ONLY`/`IDEMPOTENT`/
`DEDUPLICATABLE` actions degrade an unprovable outcome to an explicit retry with a
fresh attempt generation, because the runtime knows repetition is safe.

The runtime never infers that a side effect did not happen merely because a worker
disappeared before reporting success.

## Model and tool adapters

`ModelBackend` and `ToolBackend` are replaceable interfaces. Agent Runtime consumes
a resolved model target and never ranks providers. `BackendIncarnation` carries an
explicit backend identity and generation; a process-bound backend must present a
fresh generation when it restarts, and restarting one backend never invalidates
another.

Two deterministic reference implementations ship with the library:

- `ReferenceModelBackend` — offline, deterministic, reproducible function of the
  request identity and prompt. It is a reference backend, not a language model, and
  it does not call any external provider.
- `ReferenceToolBackend` — deterministic tools: `hash`, `vector-sum`, `echo`,
  `scratch-transform` (bounded file transformation inside an explicit scratch root,
  with traversal and escape rejection) and `commit-token` / `non-repeatable`
  (journalled side effects with stable operation keys and deduplication).

There is no arbitrary command executor.

## Memory and checkpoint bindings

Memory is represented as a generation-bound reference: external state identity,
compatibility identity, external generation, read/write authority, freshness,
provenance and optional digest. A moved memory generation invalidates affected
pending actions and requires revalidation.

Checkpoint bindings bind the runtime generation, run generation, step generation,
runtime epoch, progress generation and memory generation of the state they describe.
Agent Runtime decides *when* a checkpoint is required and binds its authority;
Checkpoint Fabric owns capture, persistence and restore mechanics.

A checkpoint restores only into a compatible runtime state. A checkpoint from a
superseded work, run or runtime generation cannot silently revive old authority, and
recovery distinguishes durable state restored, external bindings reconstructed,
dynamic authority revalidated, and execution actually resumable.

## Retry semantics

Retries are explicit policy, never implicit. Failure classes: `TRANSIENT`,
`PERMANENT`, `STALE_AUTHORITY`, `BUDGET_EXHAUSTED`, `POLICY_REJECTED`,
`CANCELLED`, `INCOMPATIBLE`, `AMBIGUOUS_COMPLETION`, `RESOURCE_UNAVAILABLE`,
`MODEL_UNAVAILABLE`, `TOOL_UNAVAILABLE`, `CORRUPT_STATE`, `UNKNOWN`.

`RetryPolicy` covers maximum attempts, a logical backoff sequence, retryable
classes, checkpoint-before-retry, same-target versus reroute-needed, budget and
deadline gates, side-effect safety and generation revalidation. Backoff is expressed
in logical milliseconds and driven by the injected clock; correctness never depends
on wall-clock passage. Every retry receives a fresh attempt identity, and the old
attempt never becomes current again.

## Suspension and resume

A runtime becomes `SUSPENDED` only when new action admission is closed, in-flight
actions are completed, safely cancelled or explicitly recorded as ambiguous,
required durable state is committed, checkpoint requirements are satisfied,
authority state is persisted, and no action remains falsely dispatchable.

Resume requires fresh process authority (new `RuntimeBootId`/`AgentBootId`, new
epochs), valid work/scheduler authority, compatible checkpoint/state, revalidated
model/tool/memory bindings, revalidated budget and policy, and fresh dynamic
evidence. A resumed runtime receives an explicit new incarnation; it is never a
boolean toggle.

## Cancellation and drain

Cancellation is authoritative: new action admission stops, undispatched action
authority is invalidated, cancellable in-flight actions are requested to cancel,
non-cancellable or ambiguous side effects are recorded accurately, late stale
completions are rejected before they can become current progress, terminal state is
persisted and runtime-owned authority is released. A cancelled runtime never later
reports `COMPLETED`.

Drain is separate from cancellation: no new top-level work is accepted,
already-authorized obligations finish according to policy, a required checkpoint is
taken, final safe state is published, external authority is released and the runtime
terminates cleanly.

## Authority, fencing and stale rejection

Every mutation returns a deterministic typed outcome. Rejections name the exact
authority deficit, including `REJECT_STALE_RUNTIME_EPOCH`, `REJECT_STALE_RUNTIME_BOOT`,
`REJECT_STALE_AGENT_BOOT`, `REJECT_STALE_RUN_GENERATION`, `REJECT_STALE_STEP`,
`REJECT_STALE_ACTION`, `REJECT_STALE_ATTEMPT`, `REJECT_STALE_ASSIGNMENT`,
`REJECT_STALE_MODEL_BINDING`, `REJECT_STALE_TOOL_BINDING`,
`REJECT_STALE_MEMORY_BINDING`, `REJECT_STALE_CHECKPOINT`, `REJECT_STALE_POLICY`,
`REJECT_STALE_BUDGET`, `REJECT_CANCELLED`, `REJECT_COMPLETED`, `REJECT_RETIRED`,
`REJECT_NOT_READY`, `REJECT_NOT_RESUMABLE`, `REJECT_SIDE_EFFECT_UNSAFE`,
`REJECT_RETRY_EXHAUSTED`, `REJECT_BUDGET`, `REJECT_POLICY`, `REJECT_CONFLICT`,
`REJECT_INVALID`, `REJECT_LIMIT`, `AMBIGUOUS_COMPLETION`,
`MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED` and `SHUTTING_DOWN`.

Recovery fences the previous runtime/agent incarnation permanently. A fenced boot can
never regain authority, and `recover` refuses a boot or epoch that is not strictly
newer than the current one.

`Explanation` renders outcome, subject, identities, current versus supplied
generations, lifecycle, authority deficit, retry classification, side-effect class,
policy/budget factors, binding state, whether progress was committed and whether
revalidation is required — always in canonical key order.

## Persistence and recovery

The durable format is versioned, deterministic and integrity-checked:

- magic `ARST`, explicit format version, bounded lengths and checked arithmetic;
- deterministic record ordering and a record count;
- CRC-32C header integrity, CRC-32C payload integrity and trailer integrity;
- SHA-256 semantic digest of canonical state, verified after decoding;
- candidate-state decoding and full invariant validation before any mutation;
- atomic replacement through a flushed temporary file.

An unsupported version is rejected before any ambiguous parsing. Corruption,
truncation at any boundary, trailing garbage, header damage, payload damage and
digest mismatch are all rejected with specific outcomes. Any incompatible layout
change increments the format version.

Dynamic state — live sockets, process sessions, in-flight backend handles,
unconfirmed model/tool requests, heartbeats, process-local readiness, temporary
buffers and live cancellation handles — is never persisted and never becomes current
automatically. Loading or opening a state file places the runtime in `RECOVERING`
with every dynamic binding marked as requiring revalidation.

### Exactly-once phrasing

Physical model or tool execution may occur more than once under a retry policy; that
cannot generally be guaranteed otherwise across arbitrary providers and processes.
The guarantee this library provides is **exactly-once authoritative agent-progress
commit for a given action generation, when external side-effect semantics permit it**.
Non-repeatable ambiguous side effects are reported as ambiguous, never as safe.

## Distributed reference architecture

```
Agent Runtime Coordinator        (real process, hosts one AgentRuntime)
        |  framed TCP over 127.0.0.1
Agent Worker                     (real process, one agent/runtime incarnation)
        |  framed TCP over 127.0.0.1
Tool Worker / Model Worker       (real independent processes)
```

This is a real multiprocess topology on one physical host. It is not physical
multi-node agent execution and makes no claim to be one.

The framed protocol carries magic, protocol version, message type, flags,
correlation id, payload length, header CRC-32C and payload CRC-32C. Malformed
frames, unknown message types, unsupported versions, inconsistent declared lengths,
oversized payloads and integrity failures are rejected. Writes are serialized, and
no canonical runtime lock is ever held across socket or filesystem I/O.

The core library is fully usable in-process without the reference transport.

## REAL / SYNTHETIC / UNSUPPORTED

**REAL** — independent OS processes; loopback TCP; real process death; real
coordinator restart; real durable persistence; real installed-package consumption;
real local CUDA action; real bounded filesystem side-effect tool inside an explicit
scratch root.

**SYNTHETIC** — large agent-run and action populations; deterministic reference model
responses; large tool populations; logical multi-node scenarios; failure schedules;
budget and policy scenarios; long action graphs.

**UNSUPPORTED** — physical multi-node agent execution; remote GPU; MIG; NVLink;
NVSwitch; RDMA; GPUDirect; DPU; real external LLM-provider execution.

Synthetic evidence is never presented as physical proof.

## Optional CUDA reference action

Agent Runtime is not an accelerator scheduler, and CUDA is never part of the core.
An optional component demonstrates that runtime authority can govern a real
accelerator action: it enumerates the local device, allocates real device memory,
performs host-to-device transfer, launches a kernel, synchronizes, copies back,
verifies against a CPU reference and frees device memory. The action is dispatched
through the same admission, authorization, attempt and commit path as any other tool
call, and stale action or attempt authority is rejected before any device work
happens. The standalone core builds without it.

## Build

Requirements: CMake 3.20 or newer and a C++20 compiler. On Windows, Visual Studio
2022 (or Build Tools) with the x64 toolset.

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

Options:

| Option | Default | Purpose |
| --- | --- | --- |
| `AGENT_RUNTIME_BUILD_TESTS` | ON (top level) | build the test suite |
| `AGENT_RUNTIME_BUILD_EXAMPLES` | ON (top level) | build runnable examples |
| `AGENT_RUNTIME_BUILD_TOOLS` | ON (top level) | build `agent_runtime_inspect` |
| `AGENT_RUNTIME_BUILD_BENCHMARKS` | ON (top level) | build the benchmark binary |
| `AGENT_RUNTIME_BUILD_REFERENCE` | ON (top level) | build the multiprocess reference architecture |
| `AGENT_RUNTIME_BUILD_CUDA_PROOF` | ON | build the optional CUDA proof when CUDA is present |
| `AGENT_RUNTIME_ENABLE_ASAN` | OFF | build with AddressSanitizer |
| `AGENT_RUNTIME_ENABLE_SHARED` | OFF | build a shared library instead of a static one |

First-party code is compiled with `/W4 /WX` under MSVC and with
`-Wall -Wextra -Wpedantic -Werror` plus conversion and shadow diagnostics
elsewhere. Warnings are fixed, never suppressed.

## Install

```powershell
cmake --install build --prefix C:/agent_runtime
```

The installed package exports the target `agent_runtime::agent_runtime`, the
optional `agent_runtime::cuda_reference`, the public headers, the inspection tool
and the reference workers. The installed package does not depend on the source tree.

## find_package

```cmake
cmake_minimum_required(VERSION 3.20)
project(consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(agent_runtime 1.0.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE agent_runtime::agent_runtime)
```

```powershell
cmake -S consumer -B consumer/build -DCMAKE_PREFIX_PATH=C:/agent_runtime
cmake --build consumer/build
```

## Tools

`agent_runtime_inspect` inspects a durable state file:

```
agent_runtime_inspect --state <file> [options]
  --validate       decode and validate the file without printing state
  --summary        print the runtime summary
  --run            print current run and step identity
  --actions        print the action history
  --attempts       print the attempt history
  --bindings       print model, tool and memory bindings
  --checkpoints    print checkpoint bindings
  --fenced         print fenced runtime and agent boots
  --json           render machine-readable JSON
  --version        print the version and exit
  --help           print help and exit
```

Opening a state file never makes stored authority current: the tool reports the
state as recovered and requires revalidation, and it exits non-zero when invariants
are violated.

## Examples

| Example | Demonstrates |
| --- | --- |
| `agent_runtime_example_embedded` | create run, model action, tool action, commit progress |
| `agent_runtime_example_safe_retry` | idempotent action fails, fresh attempt succeeds, one commit |
| `agent_runtime_example_ambiguous` | non-repeatable outcome becomes uncertain, blind retry refused |
| `agent_runtime_example_suspend_resume` | checkpoint, suspend, fresh authority, revalidate, resume |
| `agent_runtime_example_distributed` | coordinator process + agent worker process + tool worker process |

## Tests

The suite is deterministic and covers:

- lifecycle, action, attempt and commit authority, including every stale-authority
  rejection path;
- persistence format, integrity, corruption, truncation, trailing garbage,
  unsupported versions, atomic replacement and deterministic round trips;
- reference backend behaviour, operation-key deduplication, scratch-root confinement
  and independent backend incarnations;
- lifetime regressions for views, references across container mutation, callbacks
  and repeated construction;
- randomized property sequences with fixed seeds, invariant checks after bounded
  batches and reproduction history on failure;
- adversarial inputs: duplicate identities, conflicting duplicates, cross-action
  injection, oversized payloads, absurd declared lengths, awkward paths, malformed
  metadata, resource exhaustion and randomized completion corruption;
- deterministic races released with latches: cancel versus admission, dispatch,
  completion and commit; policy and budget change versus dispatch; memory change
  versus commit; backend incarnation change versus completion; suspend and drain
  versus new actions; concurrent completions; concurrent readers;
- real multiprocess proofs: agent process death and reincarnation, ambiguous
  side-effect preservation across tool worker death, and coordinator restart with
  old-epoch rejection;
- framed protocol and loopback transport, including malformed frames, blocked
  receive interruption, bounded queue overflow and session destruction.

Tests run naturally. No test relies on a timeout, a sleep or a watchdog.

## Benchmarks

`agent_runtime_benchmark` measures completed runtime operations only, never enqueue
latency. Action lifecycle measurements include real invocation of the deterministic
reference backends and are **not** model inference throughput. Persistence
measurements include the normal durability work of a production save. Every
benchmark asserts a correctness guard on the result.

Measured on the reference workstation (AMD Ryzen 7 9800X3D, Windows 11, MSVC 19.44 x64,
Release, `/O2`). Values are per completed operation and vary with hardware.

**Run creation** — runtime construction, initialization, run and one open step:

| Runs | Total | Per run |
| --- | --- | --- |
| 100 | 1.10 ms | 10.95 us |
| 1,000 | 7.03 ms | 7.03 us |
| 10,000 | 64.93 ms | 6.49 us |

**Tool action lifecycle** — declaration, admission, authorization, dispatch and
completion through the reference tool backend, then completion validation and
durable progress commit:

| Scale | Declare | Admit | Authorize | Dispatch+complete | Validate+commit |
| --- | --- | --- | --- | --- | --- |
| 1,000 | 2.28 us | 0.27 us | 0.83 us | 2.95 us | 1.13 us |
| 10,000 | 2.77 us | 0.39 us | 0.79 us | 2.93 us | 1.09 us |
| 100,000 | 3.37 us | 0.39 us | 0.70 us | 2.68 us | 0.95 us |

**Model action lifecycle** — same path through the deterministic reference model
backend:

| Scale | Declare | Admit | Authorize | Dispatch+complete | Validate+commit |
| --- | --- | --- | --- | --- | --- |
| 1,000 | 2.78 us | 0.24 us | 0.71 us | 3.72 us | 0.92 us |
| 10,000 | 2.94 us | 0.39 us | 0.80 us | 4.13 us | 1.00 us |
| 100,000 | 4.27 us | 0.54 us | 1.14 us | 5.58 us | 1.46 us |

**Snapshot and invariant checking** over the resulting history:

| Scale | Snapshot | Full invariant check |
| --- | --- | --- |
| 10,000 | 6.04 us | 5.56 us |
| 100,000 | 5.86 us | 59.03 us |

**Retry classification and commit** — each action fails transiently, is retried
under a fresh attempt generation and commits exactly once:

| Scale | Total | Per action |
| --- | --- | --- |
| 1,000 | 11.38 ms | 11.38 us |
| 10,000 | 131.33 ms | 13.13 us |
| 100,000 | 1.58 s | 15.80 us |

**Durable persistence** — a production save (temporary file, flush, atomic
replacement, semantic digest) and a full open, decode, invariant validation and
digest verification:

| Scale | Save | Open+decode+validate | Durable bytes |
| --- | --- | --- | --- |
| 1,000 | 18.35 ms | 16.04 ms | 669,796 |
| 10,000 | 173.93 ms | 182.21 ms | 6,717,604 |
| 100,000 | 1.83 s | 7.55 s | 67,378,800 |

**Conservative recovery** — durable state restored, dynamic authority marked for
revalidation: 12.3 us at 1,000 actions, 114.2 us at 10,000, 1.78 ms at 100,000.

Every measurement above is guarded by a correctness assertion: committed action
counts must match the accepted commits, invariants must hold, and the persistence
round trip must preserve the committed count.

## Limits

All externally influenced growth is bounded: runtime instances, runs, steps,
actions, active attempts, retry history, model and tool calls, memory and checkpoint
bindings, policy and budget records, retained history, connections, session threads,
send queues, outstanding correlations, frame and payload bytes, strings, collections,
persistence bytes, explanation factors and temporary files. Limits are checked with
checked arithmetic before allocation, and exceeding one is an explicit
`REJECT_LIMIT` outcome rather than silent truncation. Concurrency bounds are
configurable and default to one active action.

## Limitations

- No claim of physical exactly-once execution across arbitrary model or tool
  providers; the guarantee is exactly-once authoritative progress commit for a given
  action generation where side-effect semantics permit it.
- No physical multi-node execution, remote GPU, MIG, NVLink, NVSwitch, RDMA,
  GPUDirect or DPU support.
- The reference model backend is deterministic and offline. No external model
  provider integration ships with this release.
- The reference distributed architecture targets one physical host.
- Cancellation of an external model or tool operation is reported as requested
  unless the backend proves physical cancellation.
- Recovery revalidates dynamic authority; it never makes persisted dynamic evidence
  current on its own.

## Project layout

```
include/agent_runtime/          public headers
include/agent_runtime/distributed/  framed protocol and transport headers
include/agent_runtime/cuda/     optional CUDA reference action header
src/                            library implementation
src/distributed/                protocol and transport implementation
cuda/                           optional CUDA reference action and kernel
reference/                      multiprocess reference architecture
tools/                          agent_runtime_inspect
examples/                       runnable examples
tests/                          deterministic test suite
benchmarks/                     completed-operation benchmarks
cmake/                          package configuration
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
