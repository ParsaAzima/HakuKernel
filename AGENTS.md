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

