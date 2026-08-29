================================================================
2kOS - 64-bit Operating System
================================================================

2kOS is a 64-bit operating system kernel for x86_64 processors.
The project is built from scratch with a focus on clean design,
hardware support, and practical usability.

Design Goals
------------

- Full 64-bit x86_64 support
- Preemptive multitasking
- Modular driver architecture
- Hardware abstraction through ACPI
- User-space program support
- POSIX-like system call interface

Development Status
------------------

The kernel is currently in active development. Core subsystems are 
functional and tested on both emulated and real hardware.

Building
--------

Build requirements:
- gcc compiler
- NASM assembler
- GNU Make
- xorriso

Build the kernel:
    make

Run in QEMU:
    make run

Source Tree
-----------

Kernel/Include/     - Public headers
Kernel/Source/      - Kernel implementation
Kernel/Source/Boot/ - Bootstrap and early initialization
Kernel/Source/Drivers/ - Hardware drivers
Kernel/Source/Mem/  - Memory management
Kernel/Source/Time/ - Timers and system clock

Documentation
-------------

The code is the documentation. Each subsystem has clear separation
and consistent naming conventions. See the headers for public APIs.

License
-------

2kOS is open source. See LICENSE file.

Project Repository
------------------

Main repository: https://github.com/[username]/2kos
Issue tracker:   https://github.com/[username]/2kos/issues

----------------------------------------------------------------
2kOS - Simple. Clean. From scratch.
================================================================
