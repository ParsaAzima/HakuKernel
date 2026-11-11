# Repository Agent Instructions

These instructions apply to all work in this repository. Read
[docs/README.md](docs/README.md) for the current architecture and build
prerequisites, and [ROADMAP.md](ROADMAP.md) for planned work and known scope
boundaries.

## Project Constraints

- HakuKernel is a freestanding 32-bit x86 operating system, not a hosted
  application. Preserve the boot, interrupt, paging, and user/kernel privilege
  boundaries.
- The kernel is built with 32-bit MSVC `cl.exe` and NASM, linked without the C
  runtime. Avoid hosted-library or compiler-runtime dependencies.
- Keep the implementation compatible with the existing toolchain and patterns.
  NASM objects linked with MSVC need explicit sections and matching symbol
  naming; see [troubleshooting](docs/troubleshooting.md).
- Treat disk images and generated files as user data. Do not overwrite or
  discard them except through the documented build or test flow.
- Keep syscall user-pointer validation and `-errno` behavior consistent. Never
  dereference user-provided pointers before validating and copying them safely.
- Document hardware limitations and test behavior changes in QEMU.

## Build and Validation

On Windows, build and run the full headless integration check with:

```powershell
python build.py headless
```

Use `python build.py` for a build-only check and `python build.py run` to boot
the image in a QEMU window. See [building and running](docs/building.md) for
required tools and troubleshooting.
For documentation-only changes, verify links and spelling; a kernel build is
not required.

## Algorithm and Efficiency Rules

- Use the most efficient algorithm and data structure that fits the problem,
  and keep the complexity honest: no accidental O(n²) work over
  user-controlled sizes, no repeated linear scans where one pass or an index
  will do, and no per-byte or per-field copies when a page- or block-sized
  copy is available.
- Say the complexity out loud when it is not obvious. A deliberate linear
  scan over a fixed, tiny table (8 cache slots, 16 descriptors) is fine, but
  the comment must make that a choice rather than an oversight.
- Prefer caller-owned buffers and existing caches over new allocations, and
  bound every loop by an explicit constant so hostile input cannot turn it
  into unbounded work.
- Validate user pointers once per page and copy in page-sized chunks, as
  `copy_from_user64`/`copy_to_user64` do; a syscall must not call the page
  walker once per byte.
- Efficiency never buys its way out of the safety rules above: validation,
  `-errno` behavior, and the privilege boundaries stay intact. Keep the
  headless and x64 self-tests green and measure before claiming a speedup.

## Change Guidelines

- Make focused changes and follow existing naming, formatting, and error
  handling conventions.
- Update the documentation under `docs/` or the roadmap when architecture,
  commands, behavior, limitations, or project priorities change.
- Add or update headless/self-tests for observable kernel behavior where
  practical.
- Do not claim full POSIX compatibility; describe the implemented subset
  accurately.
- Do not stage unrelated files or generated/user data.

