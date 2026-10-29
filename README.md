# Bare-Metal RISC-V Operating System Kernel

A bare-metal **64-bit RISC-V operating system kernel** built from scratch in **C and RISC-V assembly**. The project implements essential operating-system mechanisms, from bootstrapping and interrupt handling to process scheduling, virtual memory, and system calls. It is designed around low-level hardware/software interaction, including development on the **OrangePi RV2**.

## Features

### Bootstrapping and Hardware Interfaces
- Low-level RISC-V assembly startup and C/assembly interfaces.
- UART bootloader supporting kernel loading over a serial connection.
- Device-tree parsing for hardware discovery and initramfs integration.
- Supervisor/user privilege transitions and trap handling.
- Timer interrupts, PLIC interrupt handling, and UART interrupt support.

### Memory Management
- Buddy page allocator and dynamic kernel memory allocation.
- **Sv39 virtual memory** with multi-level page tables and separate user address spaces.
- Demand paging and page-fault handling.
- **Copy-on-write (COW)** memory management with page reference tracking.
- User-space memory mapping support.

### Threads and Process Management
- Kernel threads and **preemptive round-robin scheduling**.
- User-mode process execution and context management.
- Process-related system calls, including `fork`, `exec`, and `waitpid`.
- POSIX-style signal handling.
- Timer-based callbacks and deferred task scheduling.

### File Systems and User Interaction
- Virtual file-system support, including **tmpfs** and initramfs-based program loading.
- UART command shell for interacting with the kernel.
- Commands to load or execute programs and exercise memory management, threading, and scheduling.

## Architecture

The implementation separates hardware-facing startup, kernel services, and user-process support into focused modules:

    start.S                   RISC-V assembly startup and low-level control
    uart.c                    UART communication
    kernel/
      boot.c                  Kernel initialization
      mm.c                    Physical and dynamic memory management
      vm.c                    Sv39 virtual memory and page tables
      sched.c                 Timers, interrupts, and deferred tasks
      thread.c                Thread management and scheduling
      trap.c                  Trap and exception handling
      syscall.c               User processes, system calls, page faults, and COW
      shell.c                 Interactive UART shell
      util.c                  Device tree, SBI, and low-level utilities
      video.c                 Display support
    include/kernel/common.h   Shared kernel interfaces and definitions

## Technologies

- **Languages:** C, RISC-V Assembly
- **Architecture:** RISC-V RV64, supervisor/user modes, Sv39
- **Systems concepts:** Bootloaders, interrupts, memory allocators, virtual memory, preemptive scheduling, processes, system calls, signals, and file systems
- **Hardware:** OrangePi RV2

## Author

**Che-An Lin** — [GitHub](https://github.com/johnny19436)
