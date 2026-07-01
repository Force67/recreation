# minicoro (vendored)

Single-header stackful coroutine library, used as the cross-platform backend for
`engine/script/papyrus/fiber.cc` (the Papyrus latent-Wait scheduler). It switches
stacks on the *same* OS thread (asm backend, or Win32 fibers / ucontext fallback),
which is required here: the Papyrus VM is thread-confined to its guest thread, so
a fiber must not run its activation on another thread.

- Upstream: https://github.com/edubart/minicoro
- Source: `minicoro.h` fetched from the `master` branch on 2026-07-01.
- License: Public Domain (unlicense) OR MIT-No-Attribution — see the end of
  `minicoro.h`.

Update by re-fetching `minicoro.h` from a pinned upstream tag/commit. No build
step; the implementation is compiled once via `MINICORO_IMPL` in
`engine/script/papyrus/minicoro_impl.cc`.
