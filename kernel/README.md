# Lab5 Kernel Module Layout

- `boot.c`: startup sequence and interrupt setup.
- `trap.c`: trap dispatch and exception diagnostics.
- `mm.c`: startup allocator, buddy allocator, and pool allocator.
- `sched.c`: timer queue, deferred task queue, timeout scheduling, and PLIC helpers.
- `shell.c`: command loop, loader path, and user-program execution entry.
- `util.c`: low-level helpers (string/list/FDT/SBI/time primitives).

This split keeps runtime behavior the same while reducing coupling in `main.c`.
