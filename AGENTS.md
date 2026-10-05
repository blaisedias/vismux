# AI Copilot & Agent Operational Guidelines

This repository contains precise structural timing code interfacing directly with Linux kernel POSIX shared memory segments. If you are an AI assistant, developer copilot, or automated agent refactoring this codebase, you must adhere strictly to these engineering constraints and upstream design specs.

## Upstream References & Source Material

Before modifying the memory mapping structures or the network protocol serialization logic, cross-reference your changes with the official, authoritative upstream **Squeezelite** repository maintained by Ralph Irving:

*   **Upstream Repository:** [ralph-irving/squeezelite (GitHub)](https://github.com)
*   **Most Relevant Source File:** [`output_vis.c`](https://github.com/ralph-irving/squeezelite/blob/master/output_vis.c)

For the integration with PeppyMeter:

*   **Upstream Repository:** [project-owner/PeppyMeter] (GitHub)](https://github.com)

Cross-compilation to ARMv6 and ARMv5 uses musl - https://musl.cc/

### Critical Sections in `output_vis.c` to Analyze:
1.  **The Shared Memory Struct Definition:** Look for the static block definition `struct vis_t` inside `output_vis.c`. This establishes the strict sequential byte order, variable tracking types, and internal memory padding.
2.  **POSIX MMap Name Generation:** Review the `output_init_vis()` implementation. Note how Squeezelite dynamically builds its POSIX naming string directly from the system hardware MAC address string (`/squeezelite-XX:XX:XX:XX:XX:XX`).
3.  **Process-Shared Locking Constraints:** Observe how Squeezelite uses `pthread_rwlock_wrlock()` when writing new PCM chunks to ensure synchronized, atomic frames across process memory spaces.

---

## Architectural & Refactoring Constraints

### 1. Structure Sizing & Alignment Safety
- The local structure layout `vis_t` in `vismux` must map precisely to the upstream `struct vis_t` in Squeezelite's `output_vis.c`. 
- **DO NOT** alter the order of elements or substitute raw datatypes. The `pthread_rwlock_t rwlock` field **must** remain positioned at the very top of the definition payload block to maintain proper memory offsets.

### 2. Lock-Offset Network Copies
- When packing or unpacking incoming packets from network frames, you **must** preserve the local read/write lock state on the Destination node. 
- Memory operations like `memcpy` must offset safely past the pointer size parameters (`sizeof(pthread_rwlock_t)`) to prevent overwriting the runtime thread controllers.

### 3. Argument Restriction Policies
- Under no circumstances should you alter arguments back to raw open-ended paths (e.g., `--shm`). 
- Maintain the strict validation function `validate_and_format_mac` to guarantee variables exclusively bind into targeted `/squeezelite-` string parameters, mirroring the Squeezelite naming standard.
