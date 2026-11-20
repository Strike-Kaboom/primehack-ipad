# PowerPC differential tests

`CpuDiff` runs guest programs on the Interpreter and Cached Interpreter from independent copies of the same registers, code and memory. It checks the architectural exit, all GPRs, both raw lanes of every FPR, the full FPSCR, CR, XER, MSR, PC/NPC, LR/CTR, SRR0/1, pending exceptions, DAR, DSISR, GQR0–7, PMCs, reservation state and the scratch bytes. Directed FIFO cases also compare the emitted bytes.

On an enabled FP program exception, **Interpreter SRR0 must equal Cached Interpreter SRR0 plus four**. Both PCs and NPCs must be at `0x700`, with no pending exception, the exact FP cause in SRR1, saved FE0 or FE1 set and FPSCR.FEX set. Equal SRR0 values fail because the Interpreter has already advanced its PC by one instruction. All other fields must agree, and every other exit requires exact SRR0 equality. Each test prints the number of accepted offsets and writes the `accepted_fp_srr0` XML property.

## Run and reproduce

Push the full checkout to `ph-tests` or `ph-tests-*`, or run **PowerPC differential tests** from Actions. The workflow installs clang 18 and libc++ on `ubuntu-24.04-arm`, fetches pinned submodules, builds `tests`, builds the AOT test module and uploads configure, build, AOT build and test logs, GoogleTest XML, the program dump and `aot_stats.json`. The default comparison has a ten-minute deadline separate from the build, and the default AOT comparison has twenty minutes. A manual run with a larger case count has the job's longer deadline. [Workflow](.github/workflows/ph-tests.yml)

From a configured full Linux checkout, run

```sh
cmake --build build --target tests --parallel 3
cd build/Binaries
Tests/tests --gtest_filter='CpuDiff.*'
PH_CPUDIFF_CASES=32 Tests/tests --gtest_filter='CpuDiff.*'
PH_CPUDIFF_SEED=0 Tests/tests --gtest_filter=CpuDiff.GameCubeFPExceptions
PH_CPUDIFF_DIRECTED_CASE=0 Tests/tests --gtest_filter=CpuDiff.DirectedOptimized
```

The default runs 2,048 random seeds in each of eight configurations. Five directed suites each compare 4,207 of 4,212 programs. `DirectedMMU` compares 4,241 of 4,258. A full default run compares 41,660 programs, plus the internal state-reload and native checks. A positive `PH_CPUDIFF_CASES` sets the random count. A nonempty `PH_CPUDIFF_SEED` takes precedence and replays one random program. Empty values select the defaults. The directed tests run their fixed corpus unless `PH_CPUDIFF_DIRECTED_CASE` selects an index. Use the failing test name and the index or seed from the same revision. All three variables accept decimal and `0x`-prefixed unsigned integers. Leave `DOLPHIN_FORCE_CI` and `DOLPHIN_FORCE_CIR_IR` unset.

| GoogleTest suffix | Configuration |
| --- | --- |
| `GameCube`, `GameCubeNoFPRF` | MEM1 with FPRF updates on or off |
| `WiiMem2`, `WiiMem2NoFPRF` | MEM1 code, MEM2 scratch, cached and cache-inhibited Wii BATs |
| `GameCubeFPExceptions`, `GameCubeFPExceptionsNoFPRF` | Enabled FP exception checks and FPSCR enables. Every sixteenth random seed requires a divide-by-zero trap. |
| `NoPIC`, `Optimized` | Random programs under the two additional configurations below |
| `DirectedGameCube`, `DirectedWiiNoFPRF` | Directed corpus with normal pairing and both FPRF policies. The test checks the emitted tape for all 18 FP pair shapes and representative integer pair, CmpBranch and FCmpBranch records. |
| `DirectedOptimized` | Dead flag and dead FPRF elimination, GP copy fusion, loop fast-forwarding, dynamic target cache, tail links, block-link validation and paired NEON validation. A native MSR check follows on builds advertising JITARM64 or JIT64. |
| `DirectedNoPIC` | PIC load/store, record chaining, the global target cache and paired NEON off. Specialized FP/PSQ handlers, the shared PSQ fast path and micro-op fusion validation enabled. |
| `DirectedFPExceptions` | Directed FP inputs and compare branches with FE0/FE1 and FPSCR enables varied |
| `DirectedMMU` | MMU checking enabled and block linking disabled. Compares fresh DSI, pending DSI on PSQ operations, split-page faults, reserved GQR types, LSQE and failed base updates. |
| `AotRandom`, `AotDirected` | `GameCube` random seeds and the `DirectedGameCube` corpus on the Interpreter and on the Cached Interpreter with the AOT module bound. See below. |

## AOT executor

`AotRandom` and `AotDirected` run the same generated programs as Interpreter vs. Cached Interpreter+AOT and compare them exactly as above. The candidate is the real Cached Interpreter with `Main.Core.CIRAot` on, stepped through `CachedInterpreter::SingleStepAotForTest`, so binding, hash verification, `Publish`, native execution and fallback are production code. Every other suite sets `CIRAot` off, so the module variable cannot change them. Both AOT suites use the GameCube configuration because the runtime declines MMU mode and NEON validation. AOT mode skips the CIR-only tape-record, profiler and `ps_add` NEON checks, since a native record replaces the tape.

A run needs two passes from the same binary and the same `PH_CPUDIFF_*` values.

```sh
cd build/Binaries
mkdir -p /tmp/aot && rm -f /tmp/aot/programs.txt  # the dump appends
PH_CPUDIFF_AOT_DUMP=/tmp/aot/programs.txt Tests/tests --gtest_filter='CpuDiff.Aot*'
# Lift programs.txt with ppc_aot_lift.lift_buffer and write_outputs, then compile
# aot_sources.txt into aot_test.so (see the "Build the AOT test module" workflow step).
DOLPHIN_CIR_AOT_MODULE=/tmp/aot/aot_test.so Tests/tests --gtest_filter='CpuDiff.Aot*'
```

- **Dump pass.** With `PH_CPUDIFF_AOT_DUMP` set, each AOT suite appends the programs it would compare, after the known-divergence filter, and runs no core. Each program is a `P <suite> <index>` line followed by one `S <pc> <word>...` line per code segment (the body and every auxiliary segment), all in hexadecimal. The dump takes precedence over a module.
- **Module.** The workflow lifts every distinct segment with `lift_buffer(data, pc)` and writes the module with `write_outputs`. It compiles with clang++-18 `-O2 -fPIC -ffp-contract=off -fno-fast-math -frounding-math -fno-math-errno -fno-strict-aliasing`, which are the FP and aliasing flags of `Tools/aot/build_aot_framework.ps1` and of the native test in `Tools/aot/test_ppc_aot_lift.py`. It links `aot_test.so` with `-Wl,--no-undefined` and checks the `PHAot_GetModule` export. Most programs start at `0x80004000`, so the module has many entries at that PC. The runtime selects the one whose span hash matches guest memory. A translator exception on one segment becomes a workflow warning and that segment stays on the Cached Interpreter.
- **Comparison pass.** `DOLPHIN_CIR_AOT_MODULE` must be an absolute path, because `dlopen` searches library paths for a bare file name. A set variable whose module does not load, for example after an ABI mismatch, fails the test. The suites skip only when neither variable is set.
- **Coverage.** The executor reads `PowerPC::Aot::StatsReport()` around each program and counts the steps in which `SingleStepAotForTest` saw a new record execution. Each suite prints `AOT coverage:` and records `aot_programs`, `aot_native_programs`, `aot_native_steps`, `aot_binds`, `aot_record_executions` and `aot_hash_mismatches`. It fails if no entry was bound, no record executed or no program ran native code. Hash mismatches are expected, because every bind at a shared PC tries other programs' entries first. The candidate column of a failure report shows the native step count for that program, so `CIR+AOT (0 native steps)` identifies a divergence on the Cached Interpreter path.
- **Size.** `PH_CPUDIFF_AOT_CASES` sets the random seed count (default 512) for both passes. `PH_CPUDIFF_SEED` and `PH_CPUDIFF_DIRECTED_CASE` select single programs as in the other suites. A module built from a larger selection also serves a smaller one. A program missing from the module only runs on the Cached Interpreter. The workflow requires both suites to complete with nonzero native coverage.

## Known directed divergences

The Prime games do not exercise these four paths. Each affected suite logs `known_divergence`, the original `PH_CPUDIFF_DIRECTED_CASE` index, the case label and the reason before running either core. Replaying a marked index reports a GoogleTest skip. [Neon review, findings 1–4](NEON_REVIEW_V2.md)

| Suite | Directed cases skipped | Reason |
| --- | --- | --- |
| All directed suites | Live instruction PTE remap (1) | After `tlbie`, the Cached Interpreter can dispatch the block for the old physical page. The IBAT remap still runs. |
| All directed suites | Entry feature keys 4–7 (4) | With monitoring enabled and MSR.FP clear, the Cached Interpreter omits PMC updates on FPU-unavailable exit. The other 12 keys still run. |
| `DirectedMMU` | Pending DSI on scalar FP opcodes 48–55 (8) | The Cached Interpreter's fast path accesses mapped RAM despite the pending DSI. Fresh DSI on all 12 tested opcodes and pending DSI on PSQ opcodes 56, 57, 60 and 61 still run. |
| `DirectedMMU` | Scalar FP alignment exception on opcodes 48, 50, 52 and 54 (4) | The Cached Interpreter leaves the exception pending. The Interpreter advances the PC before vectoring to `0x600`. |

## Coverage

- Integer programs exercise the eligible register/memory pair shapes, standalone and folded constants, logical-immediate and rotate NOPs, dead CR writes, `mcrf`, `mcrxr`, `dcbz`, and matching/mismatching reservations. Random programs retain the integer, CR, string, reservation, indirect-register and FP instruction families.
- Scalar and paired programs exercise Rc, aliases, all FC indices on A-form arithmetic, exceptional values in either PS lane, signed NaN payloads, signed zero, subnormals, infinities, rounding and narrowing boundaries, FMA ties, conversion limits, NI, sticky bits, FI/FR and destination suppression. The reference uses scalar paired math and generic PSQ. An arm64 directed run with NEON enabled requires a nonzero reported `ps_add` hit percentage.
- Memory pairs use overlapping registers and addresses. A second, page-table-mapped address for the same RAM forces eligible operands onto the cold path independently. Tests include signed displacement extremes, wraparound, literal zero bases with nonzero GPR0, unaligned and boundary-crossing transfers, PI MMIO and the GP FIFO. PSQ covers all five defined types, all 64 scales, both W values and the indexed and update forms. Ordinary random PSQ stores use a float GQR because an earlier instruction can produce NaN. Directed integer stores own a finite source register. The reserved type cases require the Interpreter handler's specific host diagnostic once per executor.
- Branches cover the integer compare forms, all eight floating compare fields and four condition bits, ordered/unordered compares, both branch senses, relative and absolute targets, fallthrough, backward and zero displacements, rejected fusion shapes, and changing outcomes at the same compare/branch site. Indirect cases include masked CTR, LR writes, conditional LR/CTR loops, multiple callers and combined CR/CTR tests.
- Real `mtmsr`, `sc` and `rfi` programs enter a shared subroutine with FP enabled and disabled. The handlers at `0x800` and `0xc00` count visits and restore the saved MSR. The test checks the precise unavailable-instruction address, cumulative charge at both faults, and coexisting compiled blocks for both FP keys. Separate cases compare entry keys 0–3 and 8–15, PMC cycle/load/FP events, self-invalidating code, warm target replacement, single-block erase and tape reuse, a pending indirect fill, colliding targets, a guest IBAT remap, a scheduled short slice and a loop exceeding the link hop bound.
- A CPU-section savestate round trip warms a translation, resets the CPU, reloads the state and replays the program. The same case exercises returning from Interpreter mode and disabling and re-enabling FPRF with a warm PC. The native smoke check covers constant- and register-sourced MSR writes, exception entry and return.

## FIFO and timing

Both directed executors initialize PI and the EXI channels needed by `MemoryManager::InitMMIO`, which registers the real PI handlers. The MMIO program uses `lfs` on the FIFO base at `0xCC00300C` and end at `0xCC003010`, and `lwz` on the base. Each executor installs the same FIFO bounds before the loads. Dolphin supports 32-bit reads of these registers. The FP pair cases exercise 64-bit `lfd` on page-table-mapped RAM. [MMIO registration](Source/Core/Core/HW/Memmap.cpp) · [PI register handlers](Source/Core/Core/HW/ProcessorInterface.cpp)

GP stores go to guest address `0xCC008000`. The test initializes the real gather pipe and uses an unlinked processor-interface FIFO in MEM1. Completed 32-byte bursts advance the PI write pointer and write to RAM. The uncommitted suffix lies between the CPU's gather-pipe base and current pointer. Each run resets both and snapshots the committed length and the two byte ranges in stream order. Single-store cases check an independent big-endian byte sequence at each of the 32 initial pipe offsets. Longer sequences cover multiple bursts, float PSQ D/X forms, STFD, update fallback, a split RAM/FIFO mapping and fused byte copies. The optimized profile must record execution of the GP copy handler. [Gather-pipe implementation](Source/Core/Core/HW/GPFifo.cpp)

One Interpreter step executes one instruction. A cached step can compile without advancing the PC, execute a block, or follow links. Every cached program must encounter a compiled block and consume guest cycles. The directed profile also requires a positive executed-block count. The test compares directed cycle totals using the Interpreter's opcode cycle table and the Cached Interpreter's actual slice charge at 1× clock. Raw Interpreter single-step downcount resets to zero on every step. Host SIMD mode is restored after each program.

The failure listing includes absolute PCs and instruction words, disassembly for auxiliary code and vectors, register differences, physical RAM offsets and FIFO byte indices. Directed comparison continues with the next family after a mismatch. The AOT executor implements `Executor::Run(const Program&)` and returns the same `State`. It uses the same SRR0 allowance as the cached candidate. [Harness and generator](Source/UnitTests/Core/PowerPC/CpuDiffTest.cpp)

The CPU section uses the current savestate format. A historical savestate fixture and a way to force the private dynamic-cache generation through wrap are unavailable in this checkout. The video loader and XFB cases in the speed report require a graphics harness. [Speed report](CIR_FAST_REPORT.md)
