# EdenMaliG610

**A native-code execution engine for Nintendo Switch titles on the Orange Pi 5 Pro
(RK3588 / Mali-G610).**

The Switch's CPU is an ARMv8 Cortex-A57. The Orange Pi 5 Pro's is an ARMv8
Cortex-A76. The guest's ARM64 code therefore runs **natively** — no JIT, no
interpreter, no translation of guest instructions. What the engine translates is
only what the hardware genuinely cannot do: the Maxwell GPU command stream and
the Maxwell shaders.

> **This project is written with AI assistance and reviewed by a human.**
> Every change is committed with the measurement that justifies it; the
> reasoning, the dead ends and the failed hypotheses are kept in
> [`PROGRESSO.md`](PROGRESSO.md) (Portuguese). A measurement is the unit of
> truth here: a claim without a number behind it is not accepted.

---

## What it is

* Loads a Switch title's program (NSO, LZ4-compressed segments, 64-bit NPDM).
* Patches `SVC` / `WFI` into traps at load time, so the guest's system calls
  land in the engine.
* Runs the guest's ARM64 code **natively** on the A76, in the guest's own
  39-bit address space.
* Reimplements the Horizon OS services the title needs (IPC, kernel objects,
  `sm:`, `am`, `apm`, `pctl`, …) as a C++ system layer.
* Extracts the Maxwell command stream and translates it to Vulkan, and the
  Maxwell shaders to GLSL → SPIR-V.
* Carries **telemetry in every cost point**: per frame, per SVC, per service,
  per GPU batch — the engine's actual product is the measurement.

It is not a ready-to-use emulator with settings. It is the execution engine,
written so that every guest instruction, every system call and every GPU
command is measured in place.

## Native vs. translated

| Component | Classification |
|---|---|
| The title's ARM64 code | **Native** — runs as-is on the A76 |
| `SVC`, `WFI`, traps | **Wrapper** — static patch at load time |
| Horizon services (IPC, kernel objects, `sm:`/`am`/`apm`/`pctl`) | **Wrapper** — C++ |
| Maxwell GPU commands | **Translation required** — the GPU is not an NVIDIA one |
| Maxwell shaders | **Translation required** — GLSL → SPIR-V |

The guest's code is never translated. The GPU is unavoidably different:
removing that translation would require a Mali driver speaking NVIDIA's
protocol.

## Provenance: what is Eden's and what is ours

This engine is built **on top of** the [Eden](https://github.com/eden-emulator/Ryujinx)
(formerly Ryujinx) Switch emulator, and the project is therefore
**GPL-3.0-or-later**. What was taken and what was written here is recorded
explicitly, because the distinction is the whole point of the project.

### Taken from Eden — the *system* layer, not the emulator

The engine borrows Eden's **operating-system layer**: the Horizon service
framework, the kernel object model, the IPC transport, and the individual
services. These are the parts that describe *how a Switch OS behaves*, and
re-deriving them from scratch would be invention rather than engineering.

* `vendor/eden/` — a pruned copy of Eden's `core/hle` (services, kernel
  objects, IPC, synchronization), with the parts that require an emulator
  removed. `tools/gerar_lista.py` and `tools/podar_vendor.py` derive the
  source list and prune the tree; `tools/aplicar_overrides.py` applies our own
  replacements.
* The `am` (application manager) service, the `apm` and the `pctl`
  (parental control) — vendored and adapted.
* `tools/overrides/` — the files where **we** replace Eden's, with the reason
  written in the file.

### Written here

* **The whole execution engine**: NSO loader, NPDM parsing, the static
  SVC→BRK patcher, the 39-bit guest address space, the trap trampoline, the
  fault handlers, the SVC dispatcher.
* **The GPU layer**: Maxwell ISA decoder, command extraction, the IR, the
  Maxwell→Vulkan translation, the shader cache.
* **The telemetry subsystem**: per-frame and per-service profiling, the
  bottleneck classifier, the disk flush.
* **The bridge** that replaces Eden's `KProcess`/`KThread` (`k_process_bridge.h`)
  — Eden's real scheduler and page-table manager do not exist in a native
  engine, so the guest's process is bridged instead of emulated.
* The `appletOE` service and the application's focus/message model.

### Deliberately not taken

Eden's **CPU emulation** (the interpreter, the JIT and the NCE backend) is
not used. The guest runs natively on this machine's CPU. Also excluded: the
file-system emulator, the audio and video cores, and the title loader's
content-archive machinery — none of which a native engine needs.

The native execution path is structurally the same as Eden's `nce` backend
(`src/core/arm/nce/`), but reimplemented independently, for a different CPU
generation and a different GPU.

## Status

| Component | State |
|---|---|
| NSO loader (LZ4 segments, NPDM) | done, validated on a real title |
| Static patcher (SVC→BRK, HINT→NOP) | done |
| Native execution engine (traps, faults, 39-bit space) | done, validated |
| Telemetry (frame, SVC, service, GPU, bottleneck classifier) | done |
| Horizon IPC transport and `sm:` | done |
| `am` / `appletOE` (focus, messages, system event) | done |
| `apm`, `pctl` | done |
| Maxwell command extraction | done (opcode table under validation) |
| Maxwell → Vulkan translation | skeleton |
| Maxwell → SPIR-V shader compiler | integration point |
| `aoc` and the remaining services | in progress |

The guest currently reaches the `am`, opens the application's proxy and its
sub-interfaces, and waits on the services that are not implemented yet.

## Building

Target platform: the Orange Pi 5 Pro itself (aarch64, C++20, CMake ≥ 3.20).

Dependencies:

* AArch64 cross- or native toolchain (GCC or Clang)
* Vulkan (PanVK over the Mali-G610) and its loader
* SDL2 (gamepad input; see `include/g610/input.hpp` for why SDL and not evdev)
* `fmt` (used by the vendored Eden code)
* `boost` (header-only subset)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Running

```sh
./build/g610_boot /path/to/exefs
```

Useful environment variables:

| Variable | Effect |
|---|---|
| `IA_IPC_TRACE=1` | verbose IPC trace |
| `IA_DUMP_MODULE=sdk\|main\|todos` | dump a loaded module (NSO + geometry) |
| `IA_DUMP_TEXT=<name>` | dump a text section |
| `IA_FIND=<string>` | search the guest's memory for a string |
| `IA_FIND_TLS=<addr>` | search relative to the thread-local region |
| `IA_FIND_STORE=<addr>` | search the guest's store |
| `IA_BREAK_REGS`, `IA_BREAK_SYMBOLS` | dump registers / symbols at a `Break` |

## Tests

```sh
cd build && ctest
```

Six suites: core (telemetry, SVC dispatch, decoder, patcher), exec, program,
maxwell, translate, input, and a GPU execution test that actually runs on the
Mali-G610.

## Layout

```
src/            the engine (loader, patcher, dispatcher, GPU, telemetry)
include/g610/   the public headers
tests/          the test suites and the probes
vendor/eden/    the pruned copy of Eden's OS layer
tools/          the vendorising, pruning and override tooling
docs/           architecture, measurements, session notes
PROGRESSO.md    the running log: what was tried, measured, worked, failed
```

## Documentation

* [`PROGRESSO.md`](PROGRESSO.md) — the running log (Portuguese). Every bug,
  every measurement, every dead end.
* [`docs/ARQUITETURA.md`](docs/ARQUITETURA.md) — the architecture and the
  measured results on this board.
* The comments in the source: they explain *why*, and they cite the file and
  line of the upstream project each rule comes from.

## License

**GPL-3.0-or-later** — see [`LICENSE`](LICENSE).

The vendored Eden code keeps its own `SPDX` headers (`GPL-2.0-or-later` and
`GPL-3.0-or-later`, from Eden and from yuzu). The files written here are
`SPDX-License-Identifier: GPL-3.0-or-later`.

## Acknowledgements

* The **Eden** emulator project (formerly Ryujinx), for the Horizon OS layer
  this engine builds on.
* **yuzu**, for the parts of that layer Eden inherited.
* **libnx** and **Atmosphère**, as references for the Switch's behaviour.
