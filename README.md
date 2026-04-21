# OS-Jackfruit: Lightweight Linux Container Runtime

OS-Jackfruit is a lightweight, supervised container runtime featuring multi-threaded logging and kernel-space resource enforcement.

## Architecture

The system follows a tiered architectural flow to ensure robust isolation and management:

1.  **User CLI**: The operator interacts with the system via the `./engine` binary for lifecycle commands.
2.  **Unix Domain Socket**: Commands are transmitted from the CLI client to the supervisor via `/tmp/mini_runtime.sock`.
3.  **Supervisor**: A long-running process that tracks container metadata, reaps children, and manages concurrent log streams.
4.  **Fork/Namespace Isolation**: The supervisor spawns containers using `clone()` and `unshare()` to create isolated PID, UTS, and Mount namespaces, followed by a `chroot` into the container rootfs.
5.  **Kernel IOCTL**: The supervisor communicates with the `monitor.ko` kernel module to register active PIDs and set resource constraints.
6.  **Resource Enforcement**: The kernel module enforces memory limits via periodic RSS checks, while the Linux scheduler manages CPU contention based on assigned `nice` values.

## Engineering Analysis

### 1. Isolation (Namespaces & Jails)
*   **Implementation**: Utilized `unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWPID)` and `chroot()` to create a isolated execution environment.
*   **Analysis**: Namespace isolation ensures that a containerized process has its own view of the process tree, hostname, and filesystem. This prevents "noisy neighbors" from seeing or interfering with host processes. The `chroot` jail restricts the process to a specific directory subtree, providing a foundational security boundary.

### 2. Lifecycle Management
*   **Implementation**: The supervisor maintains a `container_record` list, handles `SIGCHLD` for process reaping, and manages state transitions (Starting, Running, Stopped).
*   **Analysis**: Robust lifecycle management is critical for system stability. By using a centralized supervisor, the system can ensure that containers are correctly cleaned up, exit codes are captured, and resources (like kernel module registrations) are released even if the container process crashes.

### 3. Inter-Process Communication (IPC)
*   **Implementation**: Employed Unix Domain Sockets for the control plane and Pipes for the data plane (logs). A thread-safe Bounded Buffer (Producer-Consumer) handles log synchronization.
*   **Analysis**: Separating the control plane from the data plane prevents log-heavy workloads from blocking management commands. The bounded buffer, synchronized with mutexes and condition variables, prevents memory exhaustion by back-pressuring fast producers when the logging thread is saturated.

### 4. Memory Enforcement (Kernel-Space)
*   **Implementation**: A custom kernel module (`monitor.ko`) performs periodic RSS checks via a kernel timer and triggers `SIGKILL` for hard-limit violations.
*   **Analysis**: Kernel-space monitoring provides a "source of truth" that is difficult for user-space processes to bypass. By leveraging `get_mm_rss()`, the system can accurately track resident memory usage and enforce strict resource isolation, protecting the host and other containers from OOM conditions.

### 5. Scheduling & CPU Contention
*   **Implementation**: Integrated Linux `nice` values into the container creation flow to adjust process priority.
*   **Analysis**: Experimental analysis using `cpu_hog` workloads confirmed that the Linux Completely Fair Scheduler (CFS) correctly apportions CPU time based on these priorities. Higher-priority containers receive a larger share of the CPU cycles during periods of high contention, demonstrating effective multi-tenant resource sharing.

## CLI Contract

```bash
Usage:
  ./engine supervisor <base-rootfs>
  ./engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
  ./engine run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
  ./engine ps
  ./engine logs <id>
  ./engine stop <id>
```

## Verification & Demos

| Task | Description | Evidence |
| :--- | :--- | :--- |
| **Namespace (UTS)** | Isolated Hostname and UTS namespace proof. | [Hostname Isolation](./boilerplate/images/ss1_supervision.pn) |
| **Namespace (PID)** | Process isolation (PID 1) and metadata check. | [PID Isolation](./boilerplate/images/ss2_metadata.png) |
| **Logging** | Multi-threaded logs captured in `logs/<id>.log`. | [Log Capture](./boilerplate/images/ss3_logging.png) |
| **CLI / IPC** | Supervisor communication via Unix Domain Sockets. | [CLI IPC Proof](./boilerplate/images/ss4_cli_ipc.png) |
| **Memory (Soft)** | Kernel module issues warning at soft limit. | [Soft Limit Warning](./boilerplate/images/ss5_soft_limit.png) |
| **Memory (Hard)** | Kernel module kills process at hard limit (OOM). | [Hard Limit Kill](./boilerplate/images/ss6_hard_limit.png) |
| **Scheduling** | CPU priority enforcement via Nice values. | [Scheduler Contention](./boilerplate/images/ss7_scheduling.png) |
| **Cleanup** | Proper reaping of children and socket closure. | [Engine Cleanup](./boilerplate/images/ss8_cleanup.png) |

## Build Instructions

1.  **Build Engine and Workloads**: `make all`
2.  **Build Kernel Module**: `make module`
3.  **Load Module**: `sudo insmod monitor.ko`
4.  **Start Supervisor**: `sudo ./engine supervisor rootfs-alpha`
