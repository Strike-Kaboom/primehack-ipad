# PowerPC differential tests

`CpuDiff` runs synthetic Gekko programs through Dolphin's reference Interpreter and the iCube Cached Interpreter. A pass means both executors reached an allowed exit with identical observable state for every selected program. The comparison is exact for those inputs, including NaN payloads and signed zero. Random sampling cannot establish equivalence for every possible program.

## Run and replay

Push the full fork to branch `ph-tests`, or select **PowerPC differential tests** under Actions and run it manually. The manual inputs accept a case count or one replay seed. The workflow builds the native Linux `tests` target on `ubuntu-24.04-arm`. Its artifact includes the available configure, build and test logs, plus the GoogleTest XML when produced. The expected binary path in this fork is `build/Binaries/Tests/tests`. [CI configuration](.github/workflows/ph-tests.yml)

On a Linux build host, use the package installation, pinned submodule list and CMake invocation in that workflow. From the repository root after configuration, run

```sh
cmake --build build --target tests --parallel 3
cd build/Binaries
Tests/tests --gtest_filter='CpuDiff*'
PH_CPUDIFF_CASES=32 Tests/tests --gtest_filter='CpuDiff*'
PH_CPUDIFF_SEED=0x101 Tests/tests --gtest_filter=CpuDiff.GameCube
```

The default is 20,000 distinct seeds, numbered 0 through 19,999. Each of the three guest setups has a separate test for each FPRF mode. A full green default run makes 120,000 comparisons. Each test stops at its first failure and GoogleTest continues with the other modes. Set `PH_CPUDIFF_SEED` to run just that seed in each selected test, even when a count is also set. Both inputs accept unsigned 32-bit decimal or `0x` hex. Counts must be positive. Replay against the same revision because generator changes can change the program for a seed.

Leave the fork's `DOLPHIN_FORCE_CI` and `DOLPHIN_FORCE_CIR_IR` overrides unset. The harness checks the selected core's name before comparing results.

## Cases and operands

The names below enable FPRF updates. Append `NoFPRF` for the corresponding disabled test.

| GoogleTest | Guest setup |
| --- | --- |
| `CpuDiff.GameCube` | MEM1 code and scratch. FPSCR exception enables stay clear, including across guest FPSCR writes. |
| `CpuDiff.GameCubeFPExceptions` | MEM1 with MSR.FE0 and FE1 set, randomized nonempty FPSCR enable masks, and both floating-point exception checks enabled in the core configuration. Every sixteenth seed starts with an enabled divide by zero that must reach the program vector. |
| `CpuDiff.WiiMem2` | MEM1 code and MEM2 scratch with Broadway's extended BATs and HID4.SBE. FPSCR exception enables stay clear. |

The harness declares the CPU thread, uses a temporary Dolphin profile, initializes configuration, memory, CoreTiming and the PowerPC manager, and selects the real core. The generator writes 1–24 instructions at `0x80004000`, followed by a relative branch to `0x80005000`. MSR.FP, IR, DR and RI are set. HID2.PSE and LSQE enable paired singles and quantized loads. The BATs reproduce `CBoot::SetupBAT`, including the cache-inhibited mirror. Dolphin's MMU setting and accurate data-cache emulation are off. The cached MEM1 and MEM2 address ranges have direct BAT page mappings on the arm64 runner.

The generator samples the following families.

- Integer carry and extended arithmetic, negate, subtract, multiply high/low, signed and unsigned divide, logical immediates, rotates, shifts, count-leading-zero and sign extension. It varies OE and Rc where the opcode defines them.
- Signed and unsigned compares, CR Boolean operations, field masks, and reads/writes of CR, LR, CTR and XER.
- Byte, halfword and word loads/stores, signed halfword loads, D/X and update forms, byte reversals, multiple-word and string operations, and word reservation/conditional store.
- Scalar FP loads and stores including indexed, update and `stfiwx`. Arithmetic covers the Gekko single/double tables, fused multiply-add variants, reciprocal estimates, select, round/convert, compares, move/sign and FPSCR operations.
- Paired arithmetic, estimates, sums, lane multiplies and multiply-adds, select, merges, moves, sign operations and compares on both lanes. All eight PSQ load/store forms vary W and the GQR index. Load and store GQRs cover float, u8, u16, s8 and s16 with independent six-bit scales.
- Short forward conditionals, a backward CR branch that clears its own condition, bounded `bdnz[l]` loops, and a local `bl`/`blr` sequence. Directed prefixes also exercise dependent operations, address aliases, reservation success and finite paired-single arithmetic.

All generated data addresses fall in a 1 KiB scratch region. Two base registers point into its middle through the cached and uncached aliases. An index moves addresses by eight bytes in either direction. Generated updates can move a base at most 192 bytes, and the multiple/string register ranges preserve all address registers. The initial GPRs mix random words with carry, overflow, divide and shift boundary values. FPR lanes contain raw binary64 patterns including zeros, subnormals, infinities, quiet/signaling NaNs, normal limits, exactly represented binary32 limits, finite values near one and random bits. The seed selects all four rounding modes with NI both clear and set over eight consecutive seeds.

## Execution and failure output

The same seed regenerates the entire initial register file, GQRs, code and scratch bytes for both runs. Host FP mode is reapplied from the guest FPSCR before execution. The reference uses scalar paired arithmetic and generic PSQ. The candidate enables NEON paired math, the PSQ float fast path, BAT load/store handling, micro-op fusion and fused pairs. The handlers for NEON and PSQ are shared by the cores, so the harness configures them separately. Each core uses the same FPRF policy and the full FPSCR participates in the comparison in both modes.

Each executor runs through `PowerPCManager::SingleStep`. One cached step can compile a block or execute several guest instructions. The common comparison point is the stop address or, in the exception group, entry to the floating-point program-exception vector at `0x700`. The harness captures the delivered exception before executing any vector code. An out-of-range or unaligned PC, a pending exception, or exhausting 128 dispatches inside the program fails the test even if both cores agree. Guest clock values and dispatch counts are outside the comparison. The generator issues no time-base or decrementer reads.

The listing shows guest PC, instruction word and Dolphin disassembly. The two hex columns are the Interpreter result and Cached Interpreter result. `fN.PS0` and `fN.PS1` are 64-bit encodings. `RAM[...]` is a physical address, so stores through either BAT alias appear in the same column. Every differing GPR, FP lane, CR, XER, FPSCR, LR, CTR, MSR, PC, SRR0/1, pending-exception word, reservation field and scratch byte is printed. Identical exit PCs inside the program indicate a shared failure to finish. Use the printed seed and failing GoogleTest name for replay.

The executor interface takes a `Program` with its initial state and returns a `State`. A future AOT executor can implement that interface and pass its result through the same comparison against the reference batch. [Test and generator](Source/UnitTests/Core/PowerPC/CpuDiffTest.cpp)

## Build status and assumptions

- This Windows scratch copy lacks `Externals/` and `Data/Sys/`, and has no Linux arm64 build environment. The workflow assumes checkout of the full fork with its tracked wrapper CMake files, data directory and fetchable submodule revisions. Actual package resolution, linking, runtime, resource limits and execution counts await CI.
- There is a source-level Linux build blocker. `MemoryManager::Init` references `MemArena::DarwinMode`, `GetDarwinMode`, `SetDarwinMode` and `DarwinModeName`. `InitFastmemArena` also calls `UsesPlainViews`. These declarations live only in the Apple branch of `MemArena.h`. Disabling the fastmem arena at runtime still compiles those calls. The configure/build logs are uploaded even when this prevents GoogleTest from starting. [Memory initialization](Source/Core/Core/HW/Memmap.cpp) · [Platform declarations](Source/Core/Common/MemArena.h)
- The paired-single NEON implementation is guarded by `_M_ARM_64 || __aarch64__`. Actual NEON execution also depends on round-to-nearest and operand/result guards. The finite paired prefix targets those guards. The test checks architectural results and the cached BAT mapping. Per-op hit counts would require profiling. [Paired-single implementation](Source/Core/Core/PowerPC/Interpreter/Interpreter_Paired.cpp)
- The harness leaves HID0.ICE at reset, invalidates cached translations for the entire program window after each rewrite, and disables unconditional branch following so the stop instruction remains observable at a dispatch boundary. It exercises the generated block and its internal branches. Instruction-cache coherency, interrupts, MMIO, page faults, inter-block scheduling and game boot are outside this corpus.
- An early runtime difference to investigate is SRR0 on an enabled FP trap. In this revision `Interpreter::SingleStepInner` advances PC before the outer step checks remaining exceptions. The cached exception callback explicitly restores the faulting instruction's PC before delivery. A difference there will be reported, including on the directed divide test. Which value is correct on hardware is a separate question from whether these cores agree. [Interpreter stepping](Source/Core/Core/PowerPC/Interpreter/Interpreter.cpp) · [Cached exception delivery](Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreter.cpp)
