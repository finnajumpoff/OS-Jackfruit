# OS-Jackfruit: Multi-Container Runtime & Kernel Monitor
**Team Name:** finnajumpoff  
**Team Members:** kanchhit (SRN: PES1UG24CS213) mahit (SRN:PES1UG24CS231)

---

## 1. Project Overview
This project is a lightweight Linux container runtime consisting of a user-space supervisor (`engine`) and a kernel-space memory monitor (`monitor.ko`). It demonstrates process isolation through namespaces, concurrent logging via a bounded buffer, and kernel-level resource enforcement using `ioctl`.

---

## 2. Build, Load, and Run Instructions

### Pre-requisites
Ensure you have installed the necessary headers for kernel module development:
```bash
sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r)

Build Phase
Bash

# Compiles the engine, workload hogs, and the kernel module
make engine module memory_hog cpu_hog io_pulse

Loading the Kernel Monitor
Bash

sudo insmod monitor.ko
# Verify the control device is created
ls -l /dev/container_monitor

Execution Sequence

    Start the Supervisor (Terminal 1):
    Bash

    sudo ./engine supervisor rootfs-base

    Launch a Container (Terminal 2):
    Bash

    # Note: Copy workloads into the rootfs before starting for filesystem isolation
    cp memory_hog rootfs-alpha/
    sudo ./engine start alpha-box rootfs-alpha "./memory_hog 100" --soft-mib 20 --hard-mib 50

    Operations:

        List metadata: sudo ./engine ps

        View persistent logs: cat logs/alpha-box.log

        Graceful Stop: sudo ./engine stop alpha-box

        Teardown: sudo rmmod monitor

3. Engineering Analysis
1. Isolation Mechanisms

Isolation is achieved using Linux Namespaces and chroot.

    PID Namespace: Isolates the process ID view, making the container process PID 1 within its own world.

    UTS Namespace: Provides an independent hostname via sethostname().

    Mount Namespace & chroot: Jails the process into a private root filesystem (e.g., rootfs-alpha), ensuring it cannot access host files unless explicitly copied into the jail.

    Kernel Sharing: While isolated, containers still share the host kernel's system clock and core scheduling logic.

2. Supervisor and Process Lifecycle

The long-running Supervisor acts as the central manager:

    Reaping: It handles SIGCHLD signals to clean up exited children, preventing zombie processes.

    Metadata: It tracks the state (STARTING, RUNNING, KILLED, EXITED) and host PIDs for all active containers.

    Signals: It handles SIGINT/SIGTERM to perform an orderly shutdown of all logging threads and child processes.

3. IPC, Threads, and Synchronization

The architecture utilizes two distinct IPC paths:

    Control Plane (Unix Domain Sockets): Used for bidirectional CLI-to-Supervisor communication.

    Logging Plane (Pipes): Container output (stdout/stderr) is captured via pipes and fed into a Bounded Buffer.

    Synchronization: We used Mutexes and Condition Variables to implement a Producer-Consumer pattern. This prevents log loss and ensures the supervisor does not crash if a container generates output faster than the disk can write it.

4. Memory Management and Enforcement

RSS (Resident Set Size) measures the actual physical RAM allocated to a process.

    Soft Limit: The kernel module logs a warning in dmesg when the process first breaches this threshold.

    Hard Limit: The kernel module issues a SIGKILL once the limit is exceeded.

    Justification: Enforcement resides in kernel space to ensure it cannot be bypassed by user-space processes and to provide more efficient, timer-based monitoring.

5. Scheduling Behavior

Experiments with cpu_hog demonstrated Linux scheduling priorities:

    Nice Values: By assigning different nice values (e.g., 0 vs 10), we observed the scheduler granting significantly more CPU time to the higher-priority task.

    I/O Wait: io_pulse demonstrated how I/O-bound tasks yield the CPU, allowing the scheduler to optimize for system responsiveness.

4. Design Decisions and Tradeoffs

    Spinlock vs Mutex: We used a Spinlock in the kernel module because the timer callback runs in an atomic interrupt context where sleeping (mutex) is prohibited.

    Bounded Buffer Capacity: A buffer size of 10 was chosen to balance memory footprint with logging throughput.

    Unix Sockets: Chosen for the CLI contract to support complex, bidirectional request-response messages more effectively than FIFOs.

5. Final Checklist & Demo Proof

    [x] Multi-container: Supervisor successfully manages multiple IDs (alpha, beta, etc.).

    [x] Log capture: Verified via cat logs/[id].log.

    [x] Hard-limit: Verified via dmesg kill logs.

    [x] CLI Contract: Verified via engine ps and engine stop commands.

    [x] Cleanup: Verified no zombies remain and rmmod succeeds.


### Final Step for You:
1. Copy the code block above.
2. Open your `README.md` file in your editor.
3. Replace the contents with this code.
4. Fill in your **SRN** and **Partner's name** at the top.
5. Push to GitHub!
