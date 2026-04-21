# OS-Jackfruit Final Project Summary

## Architectural Flow
1. **User CLI**: The user executes commands via the `./engine` binary (e.g., `start`, `ps`, `stop`).
2. **Unix Socket**: The CLI client communicates with the long-running supervisor process over a Unix Domain Socket (`/tmp/mini_runtime.sock`).
3. **Supervisor**: 
   - Manages container lifecycle and metadata.
   - Handles multi-threaded logging via a **Bounded Buffer** (Producer-Consumer pattern).
   - Monitors container logs using dedicated pipe-reading threads.
4. **Fork/Namespace**: 
   - The supervisor uses `clone()` and `unshare()` to create isolated environments.
   - **PID Namespace**: Isolates process IDs.
   - **UTS Namespace**: Provides independent hostname.
   - **Mount Namespace**: Enables private mounts and `chroot` jails.
   - **chroot**: Jails the process into a specific root filesystem (e.g., `rootfs-alpha`).
5. **Kernel IOCTL**: 
   - The supervisor communicates with the `monitor.ko` kernel module using `ioctl`.
   - It registers every new container's host PID and memory limits (soft/hard).
6. **Resource Enforcement**: 
   - **CPU**: Enforced via standard Linux priorities (`nice` values) set during container creation.
   - **Memory**: The `monitor.ko` kernel module periodically checks RSS. It logs warnings for soft-limit breaches and issues a `SIGKILL` for hard-limit violations, ensuring robust resource isolation.

## Key OS Concepts Demonstrated
- **Process Isolation**: Using Namespaces and chroot.
- **Inter-Process Communication (IPC)**: Unix Domain Sockets for control, Pipes for logging.
- **Concurrency**: Mutexes, Condition Variables, and Threading in the supervisor.
- **Kernel-User Bridging**: Custom character device and IOCTL interface.
- **Resource Management**: Scheduling priorities and memory limit enforcement.
