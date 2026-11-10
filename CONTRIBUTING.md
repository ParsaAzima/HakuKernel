# Contributing to HakuKernel

Thank you for considering a contribution. HakuKernel is an experimental,
freestanding 32-bit x86 operating system. Contributions should preserve the
project's small, testable design and accurately document current limitations.

## Before You Start

- Read [docs/README.md](docs/README.md) for the architecture, setup, and known
  limitations.
- Check [ROADMAP.md](ROADMAP.md) to see whether the work fits an existing
  milestone. For substantial design changes, open an issue before investing in
  a large implementation.
- For security-sensitive bugs, follow [SECURITY.md](SECURITY.md) rather than
  opening a public issue.

## Development Environment

The supported development workflow is Windows with Visual Studio 2022's
32-bit MSVC compiler, NASM, Python 3, and QEMU. See
[docs/building.md](docs/building.md) for details.

```powershell
python build.py
python build.py headless
```

The headless command builds the kernel and boots it in QEMU, then checks serial
markers from the integration/self-tests. Run it for kernel, boot, syscall,
scheduler, storage, and build changes. Documentation-only changes do not need
a kernel build.

## Implementation Guidelines

- Keep kernel code freestanding: do not introduce dependencies on the C
  runtime, hosted operating-system APIs, or unconfigured compiler features.
- Follow the existing C and NASM conventions. Keep changes focused and ensure
  symbols link correctly across MSVC and NASM.
- Validate syscall arguments, user memory ranges, arithmetic bounds, and disk
  structures before using them. Return explicit errors rather than silently
  ignoring failures.
- Preserve existing behavior unless the change intentionally updates it; add
  or extend self-tests for behavioral changes.
- Update documentation for user-visible behavior, build requirements,
  architectural decisions, and limitations.
- Avoid unrelated formatting or generated-file changes.

## Pull Requests

1. Create a focused branch from `main`.
2. Explain the problem, the chosen approach, and any compatibility or behavior
   changes in the pull request description.
3. Include tests or clear manual verification steps.
4. Run `python build.py headless` when applicable and report the result.
5. Keep the change limited to the stated scope and respond to review feedback.

The project is licensed under GNU GPL version 3. Contributions are submitted
for inclusion under the project's licensing terms; include notices for any
third-party code and verify its license is compatible before adding it.

