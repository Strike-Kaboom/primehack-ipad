# PowerPC differential tests

`CpuDiff` runs guest programs on the Interpreter and Cached Interpreter from independent copies of the same registers, code and memory. It checks the architectural exit, all GPRs, both raw lanes of every FPR, the full FPSCR, CR, XER, MSR, PC/NPC, LR/CTR, SRR0/1, pending exceptions, DAR, DSISR, GQR0–7, PMCs, reservation state and the scratch bytes. Directed FIFO cases also compare the emitted bytes.

The **only accepted register difference** is the four-byte SRR0 offset on an enabled FP program exception. Both PCs and NPCs must be at `0x700`, with no pending exception, the exact FP cause in SRR1, saved FE0 or FE1 set and FPSCR.FEX set. Interpreter SRR0 must equal Cached Interpreter SRR0 plus four. All other fields still have to agree. Each test prints the number accepted and writes the `accepted_fp_srr0` XML property. Equal SRR0 values pass without incrementing the count.

## Run and reproduce

Push the full checkout to `ph-tests` or `ph-tests-*`, or run **PowerPC differential tests** from Actions. The workflow installs clang 18 and libc++ on `ubuntu-24.04-arm`, fetches pinned submodules, builds `tests` and uploads configure, build and test logs and GoogleTest XML. The default comparison has a ten-minute deadline separate from the build. A manual run with a larger case count has the job's longer deadline. [Workflow](.github/workflows/ph-tests.yml)

From a configured full Linux checkout, run

```sh
cmake --build build --target tests --parallel 3
cd build/Binaries
Tests/tests --gtest_filter='CpuDiff.*'
PH_CPUDIFF_CASES=32 Tests/tests --gtest_filter='CpuDiff.*'
PH_CPUDIFF_SEED=0 Tests/tests --gtest_filter=CpuDiff.GameCubeFPExceptions
PH_CPUDIFF_DIRECTED_CASE=0 Tests/tests --gtest_filter=CpuDiff.DirectedOptimized
```

The default runs 2,048 random seeds in each of eight configurations. Five directed tests each run 4,212 programs. The checked-MMU test runs 4,258. A full default run compares 41,702 programs, plus the internal state-reload and native checks. A positive `PH_CPUDIFF_CASES` sets the random count. A nonempty `PH_CPUDIFF_SEED` takes precedence and replays one random program. Empty values select the defaults. The directed tests always run their fixed corpus unless `PH_CPUDIFF_DIRECTED_CASE` selects an index. Use the failing test name and the index or seed from the same revision. All three variables accept decimal and `0x`-prefixed unsigned integers. Leave `DOLPHIN_FORCE_CI` and `DOLPHIN_FORCE_CIR_IR` unset.

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
| `DirectedMMU` | MMU checking enabled and block linking disabled. Includes alignment, pending and fresh DSI, split-page faults, reserved GQR types, LSQE and failed base updates. |

## Coverage

- Integer programs exercise the eligible register/memory pair shapes, standalone and folded constants, logical-immediate and rotate NOPs, dead CR writes, `mcrf`, `mcrxr`, `dcbz`, and matching/mismatching reservations. Random programs retain the integer, CR, string, reservation, indirect-register and FP instruction families.
- Scalar and paired programs exercise Rc, aliases, all FC indices on A-form arithmetic, exceptional values in either PS lane, signed NaN payloads, signed zero, subnormals, infinities, rounding and narrowing boundaries, FMA ties, conversion limits, NI, sticky bits, FI/FR and destination suppression. The reference uses scalar paired math and generic PSQ. An arm64 directed run with NEON enabled requires a nonzero reported `ps_add` hit percentage.
- Memory pairs use overlapping registers and addresses. A second, page-table-mapped address for the same RAM forces eligible operands onto the cold path independently. Tests include signed displacement extremes, wraparound, literal zero bases with nonzero GPR0, unaligned and boundary-crossing transfers, PI MMIO and the GP FIFO. PSQ covers all five defined types, all 64 scales, both W values and the indexed and update forms. Ordinary random PSQ stores use a float GQR because an earlier instruction can produce NaN. Directed integer stores own a finite source register. The reserved type cases require the Interpreter handler's specific host diagnostic once per executor.
- Branches cover the integer compare forms, all eight floating compare fields and four condition bits, ordered/unordered compares, both branch senses, relative and absolute targets, fallthrough, backward and zero displacements, rejected fusion shapes, and changing outcomes at the same compare/branch site. Indirect cases include masked CTR, LR writes, conditional LR/CTR loops, multiple callers and combined CR/CTR tests.
- Real `mtmsr`, `sc` and `rfi` programs enter a shared subroutine with FP enabled and disabled. The handlers at `0x800` and `0xc00` count visits and restore the saved MSR. The test checks the precise unavailable-instruction address, cumulative charge at both faults, and coexisting compiled blocks for both FP keys. Separate cases cover all 16 entry keys, PMC cycle/load/FP events, self-invalidating code, warm target replacement, single-block erase and tape reuse, a pending indirect fill, colliding targets, guest IBAT and PTE/TLB remaps, a scheduled short slice and a loop exceeding the link hop bound.
- A CPU-section savestate round trip warms a translation, resets the CPU, reloads the state and replays the program. The same case exercises returning from Interpreter mode and disabling and re-enabling FPRF with a warm PC. The native smoke check covers constant- and register-sourced MSR writes, exception entry and return.

## FIFO and timing

GP stores go to guest address `0xCC008000`. The test initializes the real gather pipe and uses an unlinked processor-interface FIFO in MEM1. Completed 32-byte bursts advance the PI write pointer and write to RAM. The uncommitted suffix lies between the CPU's gather-pipe base and current pointer. Each run resets both and snapshots the committed length and the two byte ranges in stream order. Single-store cases check an independent big-endian byte sequence at each of the 32 initial pipe offsets. Longer sequences cover multiple bursts, float PSQ D/X forms, STFD, update fallback, a split RAM/FIFO mapping and fused byte copies. The optimized profile must record execution of the GP copy handler. [Gather-pipe implementation](Source/Core/Core/HW/GPFifo.cpp)

One Interpreter step executes one instruction. A cached step can compile without advancing the PC, execute a block, or follow links. Every cached program must encounter a compiled block and consume guest cycles. The directed profile also requires a positive executed-block count. The test compares directed cycle totals using the Interpreter's opcode cycle table and the Cached Interpreter's actual slice charge at 1× clock. Raw Interpreter single-step downcount resets to zero on every step. Host SIMD mode is restored after each program.

The failure listing includes absolute PCs and instruction words, disassembly for auxiliary code and vectors, register differences, physical RAM offsets and FIFO byte indices. Directed comparison continues with the next family after a mismatch. A future AOT implementation can implement `Executor::Run(const Program&)` and return the same `State`. The SRR0 allowance is an explicit comparison argument for the cached candidate. [Harness and generator](Source/UnitTests/Core/PowerPC/CpuDiffTest.cpp)

The CPU section uses the current savestate format. A historical savestate fixture and a way to force the private dynamic-cache generation through wrap are unavailable in this checkout. The video loader and XFB cases in the speed report require a graphics harness. [Speed report](CIR_FAST_REPORT.md)
