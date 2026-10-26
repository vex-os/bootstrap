# Syscall plan for a complete POSIX mlibc

Status: agreed, ready to implement. Networking is out of scope.

The work is sorted from the cheapest item to the most expensive one. Do the items in this order, and leave the last ones for later. Cost is
a rough size: **S** is a few lines, **M** is one new function or one small subsystem piece, **L** is a new subsystem or a change in several
subsystems. The futex item is first although it is not the cheapest, because it blocks every thread feature.

## Assumptions

- There is no userspace except mlibc and the init program. The ABI can change freely.
- The kernel and mlibc are submodules. An ABI change is one kernel commit, one mlibc commit and one bootstrap commit that moves both pointers.
- "Kernel" paths are under `kernel/kernel/`. "mlibc" paths are under `mlibc/sysdeps/vex/`.
- The plan comes from reading the code. Section "Not verified" lists what the author did not read.
- A stub in mlibc is allowed only when it is honest. A stub for something the kernel will own later goes into the kernel, with a `TODO` or
  `UNDONE`, so that the ABI is final from the start.

## Verified facts that shape the plan

| Subject | What the code does | Effect |
| --- | --- | --- |
| Dispatch | `syscall_dispatch` indexes a table by number. The size is the highest number plus one. An empty slot gives `ENOSYS`. | Sparse and larger numbers are safe. 320 entries of 24 bytes is one `kmalloc`. |
| Numbers in other code | `ktest` and mlibc use the `__k_SYS_*` macros. `syscall.S` has no syscall numbers. | Renumbering touches `usr/vex/syscall.h` only. |
| Futex | `check_flags` in `sys_futex.c` returns `ENOSYS` for `FUTEX_SHARED`. mlibc always passes that flag. The key of a waiter is (vmspace, address). `futex_wait` pins the page only to read the word, and unpins it before it sleeps. | Every mutex, condition variable and join in mlibc fails now. `ktest` passes 0, so it does not see this. |
| Futex timeout | mlibc passes a relative timeout to `FutexWait` and converts absolute deadlines itself (`threads.cpp`). | The kernel needs no absolute timeout. |
| `mmap` flags | The kernel accepts `SHARED`, `PRIVATE`, `FIXED`, `ANONYMOUS`. Other bits give `EINVAL`. mlibc `vm-flags.h` defines only those four. `place_mapping` uses `vms_map_replace` for `MAP_FIXED` and `vms_map` for a hint. | The other flags do not exist in either header. `MAP_FIXED_NOREPLACE` is a one-line variation of the `MAP_FIXED` path. |
| Memory | No swap code in the kernel. | `mlock` can return success honestly, with a `TODO` for swap. |
| `kill` | `kill_group` and `process_group_for_each` exist. Only `pid == -1` returns `EINVAL`. | Process groups need no work. |
| File offsets | `vnode_ops.read` and `.write` take an explicit offset. Only `file_read` and `file_write` move `file->offset`. | `pread` and `pwrite` need no new vnode operation. |
| File close | `vnode_ops.open` and `.close` exist, with `file->ctxdata`. `close` runs when the last reference goes. | End counters for a pipe are easy. |
| Reading | `read_file` in `sys_file.c` loops after a full 4096 byte chunk. It stops early only for `VNODE_CHAR_DEV`. | A pipe read of exactly 4096 bytes blocks on the second round. Add `VNODE_FIFO` to the condition. |
| `fs` pointer | `vnode.c` (chmod, chown, utimes, `set_attr`) and `sys_mmap` use `node->fs` without a check. | A pipe vnode needs a real `vfs_t`. devfs is the model. |
| Page cache | `fcache` fills pages from the block device (`bmap`, `node->fs->volume`). It never evicts a dirty page. | A memory file system needs a mode of `fcache` with no backing store. |
| Events | `event_await_unlock` takes an array of events but only one lock. A trigger without a listener is lost, unless `linger_pending` is set. tty triggers do not set it. | A wait on several files loses wakeups (see `ppoll`). |
| Signals | `queue_signal` drops a signal at once if a thread could take it and the action ignores it. A blocked signal stays pending. | A wakeup in `sigsuspend` always means a real signal. |
| Signal frame | `arch_signal_setup` needs a valid `sa_restorer`. It passes `(sig, siginfo*, ucontext*)` in `rdi`, `rsi`, `rdx`. The old mask goes to `uc_sigmask`. | mlibc must always set `SA_RESTORER` and supply a trampoline. |
| Syscall restart | A handler return of `-ERESTART` rewinds the syscall. `sys_sleep` turns `-ERESTART` into `-EINTR` and writes the time left. | `sigsuspend` must not return `-ERESTART`. A job control stop would show as `EINTR` in `sleep` unless that changes. |
| Return to user | `exit_to_user` loops on `pending_work` bits (`THREAD_WORK_EXIT`, `THREAD_WORK_SIGNAL`), and `sched_kick` makes a thread on another hart reach it. | A new work bit for "stop" fits here. |
| `process_t` size | `process.c` has `static_assert(sizeof(process_t) <= 256)`. A hand count of the fields gives about 240 bytes. The flags byte is "only ever set", so a toggling state cannot live there. The slab has block sizes of 8 bytes times a power of two, so 256 and 512 are two size classes. | Anything above 256 bytes costs a 512 byte block: half as many processes per page. Up to 512 bytes it costs nothing more. See the budget decision below. |
| Job control | `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU` and `SIGCONT` are only rows in `default_actions`. `ignored_locked` treats them as ignored. | Nothing stops. `WUNTRACED` cannot return anything. |
| tty | The tty keeps a foreground group and sends `SIGINT` and `SIGTSTP` to it. Nothing sends `SIGTTIN` or `SIGTTOU`. | Background read and write control is missing. |
| ext2 | `create` accepts only file, directory and symlink. The type mapping says "no vnode types for devices, FIFOs and sockets yet". | `mknodat` on ext2 needs inode type work. |
| Random numbers | No RDRAND, no RDSEED, no `/dev/urandom`. | `getrandom` starts from zero. |
| `uname` | `nodename` is the literal `"IMPLEMENT_ME"`. `struct __k_utsname` has no `domainname`. mlibc fills `domainname` with a fixed string. | `sethostname` fixes the first. See the open question about the second. |
| Capabilities | `CAP_SYS_ADMIN` exists. `CAP_IPC_LOCK` does not. | Use the first for `sethostname`. `mlock` gets a capability when limits arrive. |
| auxv | `AT_PAGESZ` and `AT_CLKTCK` exist. | `sysconf` gets the page size and the tick rate without a syscall. |
| Thread create | Arguments are: entry, stack top, TLS, thread argument, `clear_tid` address. On exit the kernel clears `clear_tid` and wakes a private futex key. | `pthread_join` works once the futex item is done. |
| mlibc sysdeps | The tags `Clone`, `PrepareStack`, `ThreadExit`, `Sigaction`, `Sigprocmask`, `Sigpending`, `Sigaltstack`, `Sigsuspend`, `Sigtimedwait`, `Pipe`, `Poll`, `Pselect`, `Pread`, `Pwrite`, `Readv`, `Writev`, `Sysconf`, `Madvise`, `ClockGetres`, `SetHostname`, `GetEntropy` exist in `sysdep-tags.hpp`. | The names below are real. |

## Numbering

Use blocks of 32 numbers (`0x20`) for every subsystem. Number 0 stays invalid. The old layout has 3 free numbers in the fd block and 1 in the
path block, so it cannot take the new calls.

| Block | Range | Now | New |
| --- | --- | --- | --- |
| Process | `0x20`-`0x3F` | EXIT, FORK, EXEC, WAITPID, GETPID, GETPPID, SETSID, SETPGID, GETPGID, GETSID | CLOCK_NANOSLEEP (replaces SLEEP), WAITID, GETPRIORITY, SETPRIORITY, SCHED_GETSCHEDULER, SCHED_SETSCHEDULER, SCHED_GETPARAM, SCHED_SETPARAM, SCHED_GETPRIORITY_RANGE, SCHED_RR_GETINTERVAL, SCHED_GETAFFINITY, SCHED_SETAFFINITY |
| Thread, futex | `0x40`-`0x5F` | THREAD_CREATE, THREAD_EXIT, GETTID, TCB_SET, YIELD, FUTEX_WAIT, FUTEX_WAKE | none |
| Signal | `0x60`-`0x7F` | KILL, TGKILL, SIGACTION, SIGPROCMASK, SIGPENDING, SIGRETURN, SIGALTSTACK | SIGSUSPEND, SIGTIMEDWAIT, SIGQUEUE, SETITIMER |
| Credentials | `0x80`-`0x9F` | GETRESUID ... CAPSET | none |
| Memory | `0xA0`-`0xBF` | MMAP, MUNMAP, MPROTECT, MSYNC | MADVISE, MLOCK, MUNLOCK, MLOCKALL, MUNLOCKALL, MREMAP |
| Descriptor | `0xC0`-`0xDF` | OPENAT ... FCNTL | PIPE2, PREAD, PWRITE, PPOLL, READV, WRITEV, FLOCK, FALLOCATE |
| Path | `0xE0`-`0xFF` | FSTATAT ... FACCESSAT | MKNODAT, CHROOT |
| xattr, vfs | `0x100`-`0x11F` | GETXATTRAT ... REMOVEXATTRAT, MOUNT, UMOUNT, SYNC, SYNCFS, STATVFS | none |
| System | `0x120`-`0x13F` | UNAME, GETTIME, SETTIME | CLOCK_GETRES, SETHOSTNAME, GETRLIMIT, SETRLIMIT, GETRANDOM, SYSINFO |

Keep the order inside a block. Change the comment in `usr/vex/syscall.h` from 16 to 32.

## Work items, cheapest first

### Tier 0: unblock and repair

1. **Futex with shared keys (M).** Decision: do it properly now.
   - A wait or wake with `FUTEX_SHARED` takes the physical address of the word as the key. A private wait keeps (vmspace, address).
   - A shared waiter keeps its page pinned for as long as it is queued. Today the page is unpinned before the sleep. If the page cache
     evicts or moves the page, the key would change under the waiter.
   - `futex_wake` pins the page to get the key, wakes the matching waiters, and unpins it.
   - The wake at thread exit (`clear_tid`) wakes the private key and the shared key.
   - A shared key is only correct for a `MAP_SHARED` mapping. A `MAP_PRIVATE` page can move on a copy-on-write fault after `fork`, and the
     waiter would never wake. Document this in `usr/vex/futex.h`.
   - Tests: a `ktest` with `fork` and a `MAP_SHARED` file mapping now. A shared anonymous mapping comes with item 24.
   - Not needed by mlibc, and not planned: requeue, wake-op, priority inheritance. Robust mutexes (a robust list walked at thread exit) are
     not checked. Look at what mlibc expects before you decide.
2. **`mmap` flags (S to M).** Add the flags to `usr/vex/mman.h` and mlibc `vm-flags.h`. The values are the Linux ones, as for `FIXED` and
   `ANONYMOUS`.

   | Flag | Value | Meaning | Behaviour |
   | --- | --- | --- | --- |
   | `MAP_NORESERVE` | `0x4000` | Do not count the mapping against the commit limit or reserve swap. | Accept. The kernel has no commit accounting, so doing nothing is correct. The flag starts to matter when overcommit limits exist. |
   | `MAP_STACK` | `0x20000` | The mapping is a thread stack. | Accept. On Linux it does nothing. |
   | `MAP_POPULATE` | `0x8000` | Map all pages now, so that the first touch does not fault. | Implement as best effort: after the mapping exists, fault the pages in (zero pages for anonymous, read for a file). An error is not reported. Needs a helper in `vms`. |
   | `MAP_FIXED_NOREPLACE` | `0x100000` | As `MAP_FIXED`, but fail if the range is used. | Implement: use `vms_map` at the address and return `EEXIST` if it fails because the range is busy. |

   `MAP_GROWSDOWN` (a mapping that grows down on a fault below it) is not planned. mlibc does not need it, and it changes the fault handler.

### Tier 1: mlibc only, no kernel change

3. **Signals (M).** Implement `Sigaction`, `Sigprocmask`, `Sigpending`, `Sigaltstack`.
   - Convert `struct sigaction` to `__k_sigaction`. Always set `SA_RESTORER`.
   - Write the restorer in `x86_64/` assembly: it calls `SYS_SIGRETURN`. Read `sys_sigreturn` to see where it expects the stack pointer.
   - Add a `static_assert` that `sizeof(sigset_t)` is 8 bytes. The kernel set is `uint64_t`.
   - Argument orders, checked: `sigaction(sig, act, oact)`, `sigprocmask(how, set, oset)`.
4. **Threads (M).** Implement `Clone`, `PrepareStack`, `ThreadExit` and what `pthread_create` needs. Follow another sysdeps tree, for example
   `linux`. Use `SYS_THREAD_CREATE` with the argument order in the table above. The tid word that `pthread_join` waits on is the `clear_tid`
   address. Needs item 1.
5. **`Sysconf` (S).** Page size from `AT_PAGESZ`. Clock ticks from `AT_CLKTCK`. The number of CPUs and the size of memory return `ENOSYS`
   until `SYSINFO` (item 20) exists.
6. **Remaining honest stubs (S).** `getrlimit` returns `RLIM_INFINITY`, with a comment, and `setrlimit` returns `ENOSYS`, until item 22.
   The `sched_*` calls are not stubbed in mlibc: they are kernel stubs (item 13).

### Tier 2: small kernel changes

7. **Renumber (S).** See Numbering. Do this before any new syscall.
8. **`PREAD`, `PWRITE` (S).** Arguments: `fd, buf, len, offset`. Copy the `FILE_IO_CHUNK` loop of `read_file` and `write_file`, but call
   `vnode_read` and `vnode_write` with the given offset and leave `file->offset` alone. Return `ESPIPE` when the file cannot seek. Whether
   it can seek is the answer of its `seek` operation, not its vnode type: a probe `vnode_seek(node, file, 0, SEEK_CUR)` that returns
   `-ESPIPE` means no. A new contract: a file that cannot seek returns `-ESPIPE` from `seek`. The tty and the pipe get such an operation.
   Today the tty has no `seek` operation, so `lseek` on a tty succeeds, which POSIX forbids (`ESPIPE`), and this fixes it too. Check
   `kplog`, `hwtree` and the framebuffer, which may or may not seek. Then add the `Pread` and `Pwrite` sysdeps.
9. **`CLOCK_NANOSLEEP` replaces `SLEEP` (S).** Arguments: `clock, flags, request, remaining`. `TIMER_ABSTIME` is `1`. The timeout is
   `request` for a relative call, and `max(0, request - now(clock))` for an absolute one. Keep the body of `sys_sleep`. Write `remaining`
   only for a relative call. `Sysdeps<Sleep>` becomes a relative `CLOCK_MONOTONIC` call.
10. **`SETHOSTNAME` (S).** Keep the name in a buffer in `sys_uname.c` under a lock, with a real default instead of `"IMPLEMENT_ME"`.
    Require `CAP_SYS_ADMIN`. Add the `SetHostname` sysdep. `setdomainname` is not planned, see the open questions.
11. **`READV`, `WRITEV` (M).** Vectored I/O: one call reads into, or writes from, an array of buffers (`struct iovec`). It is used by
    `stdio`, by C++ streams and by loggers, which write a message and a newline, or a header and a body, in one call. The point is
    atomicity and fewer calls. Two writers that use `write` twice can interleave their pieces, and a pipe guarantees that a write of up to
    `PIPE_BUF` bytes is not split only when it is one call. If mlibc loops over the buffers in user space, that guarantee is lost.
    Implement in the kernel: loop over the `iovec` array on top of the helper of item 8, with one lock for the whole call.
12. **`MADVISE`, `MLOCK`, `MUNLOCK`, `MLOCKALL`, `MUNLOCKALL` (S).** Real syscalls whose body checks the arguments and does nothing else.
    - `madvise`: page-aligned start, a known advice value, and `ENOMEM` if part of the range is not mapped. No effect yet. Later,
      `MADV_DONTNEED` frees the pages of an anonymous mapping.
    - `mlock` family: the same range checks. `UNDONE:` the lock matters when swap exists. Add a capability and a limit check when item 22
      exists.
13. **`GETPRIORITY`, `SETPRIORITY`, and the `SCHED_*` calls (S).** Decision: stubs in the kernel, not in mlibc.
    - Keep the nice value (one byte) in `process_t`, and report it back. It has no effect on the scheduler until nice levels are
      planned. The process budget 512 has room for it. `UNDONE:` scheduler effect.
    - `SCHED_GETSCHEDULER`, `SCHED_SETSCHEDULER`, `SCHED_GETPARAM`, `SCHED_SETPARAM`: the only policy is `SCHED_OTHER` with priority 0.
      Setting any other policy or priority returns `EINVAL`. `SCHED_GETPRIORITY_RANGE(policy)` returns 0 and 0 for `SCHED_OTHER`.
      `SCHED_RR_GETINTERVAL` returns the time slice of the scheduler.
    - `SCHED_GETAFFINITY` returns the mask of the online CPUs. `SCHED_SETAFFINITY` accepts a mask that has at least one online CPU and
      does nothing else. `UNDONE:` affinity in the scheduler.
14. **`CLOCK_GETRES` and more clock ids (S).** Answer: the clocks that programs use, besides `CLOCK_REALTIME` and `CLOCK_MONOTONIC`, are:

    | Clock | Meaning | Plan |
    | --- | --- | --- |
    | `CLOCK_MONOTONIC_RAW` | Monotonic time with no slewing. | Alias of `CLOCK_MONOTONIC`. The kernel only steps the realtime clock, so they are equal. |
    | `CLOCK_BOOTTIME` | Monotonic time that counts suspend. | `EINVAL`, with a `TODO` in `sys_gettime`: suspend will exist later, and an alias would then be wrong. |
    | `CLOCK_REALTIME_COARSE`, `CLOCK_MONOTONIC_COARSE` | Faster, less exact reads. | Aliases of the fine clocks. |
    | `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID` | CPU time of the process or the thread. POSIX requires them, and `clock()` is likely to use the first. | Item 28. Until then `EINVAL`. |
    | `CLOCK_TAI` | International atomic time. | Not planned. |

    `CLOCK_GETRES(clock, timespec*)` returns the step of the active clock source: roughly one tick, from the `mult` and `shift` of
    `clock_source_t`. Check how the active source stores its frequency before you write it. Then `Sysdeps<ClockGetres>` is a plain call.

### Tier 3: one new piece of kernel (M)

15. **`SIGSUSPEND`** (and `pause`, which calls it with the current mask).
    - Add `saved_sigmask` and a flag to `thread_t`.
    - The call saves the mask, sets the new one, and waits with no timeout, as `sys_sleep` does.
    - Never return `-ERESTART`: the syscall would run again and overwrite the saved mask. Return `-EINTR` after the wait ends.
    - In `signal_deliver`, pass the saved mask instead of the current one as `old_mask`, then clear the flag. `sigreturn` then restores the
      original mask on its own.
    - A wakeup always means a deliverable signal (see the `queue_signal` fact), so no loop is needed.
16. **`PIPE2`, with a pipe file system.**
    - New `pipefs`, modelled on devfs: a `vfs_t`, `node->fs` set, `vcache_insert_pinned`.
    - New `VNODE_FIFO` type. `stat` reports `S_IFIFO`. `getdents` reports `DT_FIFO`.
    - Pipe object: 64 KiB ring of pages, one lock, reader and writer counts, and two events ("has data", "has room"). `open` and `close`
      keep the counts.
    - `read`: empty and no writer returns 0. Empty and `O_NONBLOCK` returns `EAGAIN`. Otherwise wait with `event_await_unlock`, as
      `read_input` in the tty does.
    - `write`: no reader sends `SIGPIPE` and returns `EPIPE`. A write up to `PIPE_BUF` (4096) is atomic: wait until all of it fits.
    - `SYS_PIPE2(int fds[2], flags)` accepts `O_CLOEXEC | O_NONBLOCK`. Make two `file_t` on one vnode. Undo the first on an error.
      Copy the array out last.
    - Add `VNODE_FIFO` to the early-exit condition in `read_file`.
    - Implement `FIONREAD`. The constant exists. Add the `Pipe` sysdep.
17. **`SIGTIMEDWAIT`** (for `sigwait` and `sigtimedwait`). Take a signal from the pending set without a handler. Reuse the dequeue code of
    `signal_deliver`. The timeout works as in `sigsuspend`.
18. **`kill(-1)`.** Walk all processes with the existing `may_signal` check. Skip init and the sender. This needs a process list iterator.
    Check whether one exists.
19. **`SETITIMER`** (and `alarm`). One `ITIMER_REAL` timer per process on top of `clockevt`. At expiry send `SIGALRM`, and restart if the
    interval is not zero. Remove the timer on `exec` and `exit`. The state is about 48 bytes, owned by one process, so it goes inline in
    `process_t` once the budget is 512 bytes (see "Process size budget").
20. **`SYSINFO`.** One structure: online CPUs, total and free pages, uptime. Take the data from `hart` and `pframe`. Then `Sysconf` can use it.
21. **Random numbers.** Decision: a Linux/FreeBSD-style split into noise, a pool, and generators.
    - **Noise sources** (a registry, one entry per source, with a name and a trust flag) only *contribute* to an entropy pool. They are not
      read by users. There are two kinds:
      - Event sources push small samples from interrupt context: IRQ timing, input and disk completion timing. The call must be cheap and
        take no global lock (a small per-hart buffer, folded into the pool later, as Linux does).
      - Polled sources are read when the pool needs noise: Limine entropy, RDSEED, RDRAND, cycle-counter jitter, and later drivers.
        - virtio-rng (planned). Under QEMU it is a strong source on any host, which matters because RDSEED may be missing under WHPX.
          The kernel has no virtio driver now.
        - A floppy controller with no disk in the drive (exotic, low priority, to be tried). An empty drive may give noisy reads or
          jittery seek timing. Give it a very low credit, and print a `kprintf` notice that a floppy controller was found on a 64-bit
          machine.
      - Each source has a credit: how many bits of entropy one sample is worth. Keep the credits low. They are constants in the code. A source
        the kernel cannot check (RDRAND, Limine) is mixed in with little or no credit, unless the user marks it trusted.
    - **Pool.** One pool at first, built on a hash. The kernel has no hash except CRC32 (`crypto/`), so this item adds two. Decision:
      BLAKE2s for the pool, as Linux does: it is small, and it is built from the same kind of operations as ChaCha20. SHA-256 as well,
      for 32 pools in the style of Fortuna and for other uses that will need it. The hash is a kernel detail: no user program chooses it.
      The pool counts the credited bits. The generator is *ready* when the credit passes 256 bits.
    - **Generators** (a registry, so that more can be added later):
      - `chacha20`: the default for `/dev/urandom`, `/dev/random`, `GETRANDOM` and every kernel user that needs unpredictable bytes
        (ASLR, stack canaries, hash seeds, `AT_RANDOM`). Fast key erasure after each request.
      - `mt19937-64`: the kernel's own "duty" generator for things that need only a cheap spread of values and no secrecy: scheduler
        tie breaks, back-off jitter, the test programs. One instance per hart, so there is no lock. Each instance is seeded from `chacha20`
        at hart start. User programs never get it: their state can be recovered from 2496 bytes of output, and user programs have their
        own generators built on `GETRANDOM`.
      - `rdrand` and `rdseed`: the instruction output as a stream, with a retry loop. Only the kernel can select them, with `random=`.
        User programs can run the unprivileged instruction themselves.
    - **Choosing the generator.** User programs do not know which generator the kernel uses, and nothing in the ABI names one. The
      default is a constant in the code (`chacha20`), and the command line option `random=` overrides it at boot, so a test run needs no
      rebuild and a user who wants another generator sets it. There is no `Kconfig` choice (the generators are small and always built),
      and no runtime switch. `GRND_INSECURE` is not about generators: it only allows a read before the first seed.
    - **Syscall** `GETRANDOM(buf, len, flags)`. `flags` follow Linux: `GRND_NONBLOCK`, `GRND_RANDOM`, `GRND_INSECURE`. It always reads the
      default generator. There is no `source` argument: with MT hidden, a user program would have nothing to choose from, and the ABI can
      take a fourth argument later, when a second generator is meant for users. mlibc `GetEntropy` calls the syscall. The devices
      `/dev/urandom` and `/dev/random` stay, because shell scripts (`dd if=/dev/urandom`) have no syscall, and a script must stay as
      portable as C code. Both read the default generator.
    - **Reseed.** The rule is FreeBSD's, not a byte count (see "How Linux and FreeBSD reseed"): the read path reseeds `chacha20` from the
      pool when the pool has `RANDOM_MIN_POOL_BYTES` of credited noise and `RANDOM_MIN_RESEED_MS` have passed since the last reseed.
      Before the first seed, reseed at every chance. Safety does not depend on any thread (see below).
    - **The kernel thread** (the tty already has one: `thread_spawn_kernel`) polls the polled sources and folds the event buffers into the
      pool when there is time. An event wakes it when a buffer is full or the pool is below the reseed threshold. A timer does not. The
      plan assumes no period: userspace reads as often as the kernel and the hardware allow.
    - **Early boot.** The first seed is taken at init from what exists. Until the generator is ready, `GETRANDOM` without `GRND_INSECURE`
      blocks, or returns `EAGAIN` with `GRND_NONBLOCK`, and the device reads block in the same way. A hang of init is acceptable. With
      the Limine seed it should not happen.
    - **Limine entropy.** Read from the protocol text and from the Limine source (`common/protos/limine.c`, `common/lib/rand.s2.c`). The
      Limine protocol has `limine_entropy_request`: the kernel sets `value_count`, and the response is an array of that many `uint64_t`.
      The kernel has no such request now. It sends only these requests: command line, paging mode, MP, RSDP, date at boot, framebuffer,
      memory map, HHDM.
      - Limine caps the count at 4096 values. Ask for 8 (512 bits) and use the count that comes back.
      - Limine fills the array from RDSEED, then RDRAND, on x86-64 when CPUID says they exist. After that it asks the UEFI RNG protocol
        (a firmware service that the kernel cannot call after boot services end, so this is the only way to get it). Whatever is still
        missing comes from PCG32, seeded from the same sources and the cycle counter. PCG32 is predictable.
      - The response does not say which source made which value. So give the whole array a low credit. On a machine with RDSEED or
        UEFI RNG it is good in fact, and the pool also gets the polled sources of the kernel.
      - The array is in bootloader reclaimable memory. Mix the values in, then zero the array before the memory is reused.
    - The state of MT19937-64 is 312 words (about 2.5 KiB), more than `SLAB_MAX_SIZE`, so allocate it with `kmalloc` at hart start.
22. **`GETRLIMIT`, `SETRLIMIT`.**
    - Limits are per process, and the threads of a process already share them through `process_t`. A table of 16 limits with two 8 byte
      values is 256 bytes. It is needed in every process, but almost every process has the limits of its parent.
    - Two ways. (a) Inline: raise the budget to 512 and put a compact table in `process_t`. There is no lifetime to manage and `fork`
      cannot fail on it. A full table (256 bytes) plus the other new fields (see "Process size budget") is over 512, so this needs a table of
      only the limits that are stored and enforced. (b) Shared and refcounted, like credentials: `process_t` keeps one pointer, `fork` only
      increments the count, and `setrlimit` makes a copy, swaps the pointer under a lock, and drops the old one. This is `process_set_cred`
      again. Decision: (b), because the pointer costs 8 bytes and the pattern exists.
    - Enforce `NOFILE` (the fd table has `max_fds`) and `STACK` first. Store the other limits without enforcing them.
    - Add `static_assert(sizeof(rlimits_t) <= SLAB_MAX_SIZE)`. Check the new size of `process_t` with a build before you add a field.
    - Enforce `MEMLOCK` and add `CAP_IPC_LOCK` for item 12 here.

### Tier 4: expensive (L)

The order inside this tier follows what is wanted first, not only cost.

23. **`ramfs`** (a memory file system), planned from the start.
    - A `vfs_driver` named `ramfs`, mounted for `/tmp`, `/run` and `/dev/shm`. It has files, directories and symlinks with all data in memory.
      It needs `lookup`, `iterate` (a cookie that survives `unlink`), `create`, `unlink`, `rename`, `link`, `symlink`, `read`, `write`,
      `set_attr`, `stat`, `fault`.
    - Pages: add a memory mode to `fcache`. With no `bmap`, a missing page is filled with zeros, allocation does nothing, writeback does
      nothing, and reclaim never takes the page, because the page is the only copy. The other way is a page store inside `ramfs`, which
      repeats the fault and the read code. The first way is cheaper.
    - Unlinked-but-open files must keep their pages (`release` frees them).
    - Vnode types for FIFOs and devices are only a field here, so `mknodat` works on `ramfs` before it works on ext2.
24. **Shared anonymous `mmap` and POSIX shared memory.** `MAP_SHARED | MAP_ANONYMOUS` creates a hidden unlinked `ramfs` file of the
    size of the mapping and maps it. `shm_open` opens a file in `/dev/shm`. This also tests shared futexes (item 1) and process-shared
    mutexes in mlibc.
25. **Job control.** Decision: implement it. The existing pieces are `exit_to_user`, `pending_work`, `sched_kick`, `child_exited` and the
    tty foreground group.
    - State: stopped or running, the stop signal, "stop not yet reported", "continue not yet reported", and an `event_t` for the threads
      that wait to continue. Decision: one inline sub-structure `job_control_t job` in `process_t`, about 32 bytes (see "Process size
      budget"). No refcount and no allocation: one process owns it, and a failed allocation inside signal delivery has no good answer.
    - Stop: delivering `SIGSTOP`, `SIGTSTP`, `SIGTTIN` or `SIGTTOU` with the default action sets the state, sets a new work bit
      `THREAD_WORK_STOP` on every thread, and kicks them. A thread with the bit parks in `exit_to_user` on the event until the process
      continues. A thread in an interruptible wait leaves it with `-ERESTART`, parks, and the syscall runs again after the continue.
      `SIGKILL` and `THREAD_WORK_EXIT` must wake a parked thread.
    - `sleep` returns `EINTR` for a stop today. Make it restart instead, or use `-ERESTART` with the remaining time.
    - Tell the parent: a "stopped" status for `waitpid(WUNTRACED)`, `SIGCHLD` with `CLD_STOPPED` unless `SA_NOCLDSTOP`. `wait_child`
      also looks at the child state, not only at zombies. The wait status is `0x7f | signal << 8` for a stop and `0xffff` for a continue.
    - Continue: sending `SIGCONT` acts at send time. It drops the pending stop signals, wakes the threads, records "continued" for
      `WCONTINUED`, and sends `SIGCHLD` with `CLD_CONTINUED`. A stop signal sent later drops a pending `SIGCONT`.
    - Remove the stop signals from `ignored_locked`. Init keeps ignoring them.
    - tty: a read from a background group sends `SIGTTIN`. A write with `TOSTOP`, and a `tcsetattr` from the background, send `SIGTTOU`.
      An orphaned group, or a blocked or ignored signal, gives `EIO` instead.
    - Orphaned process groups: when a group becomes orphaned with stopped members, send `SIGHUP` and `SIGCONT`. Do it after the rest works.
    - Check `SIGTSTP`, which the tty already sends on `^Z`.
26. **`PPOLL`** (and `poll`, `select`, `pselect` on top of it in mlibc).
    - A loop of "check all, then wait on an event array" loses wakeups: `event_await_unlock` holds one lock, the files have different locks,
      and tty triggers keep no pending wakeup.
    - Fix: each poll puts its own `event_t` on the stack and registers it in a waiter list on every file before the check. A new vnode
      operation `poll(file, waiter)` registers the waiter (if not `nullptr`) and returns the ready bits. Pipe and tty triggers also wake the list.
    - Files without the operation are always ready. An invalid fd gives `POLLNVAL`. A closed pipe writer gives `POLLHUP`.
    - The signal mask swap is the same code as in item 15.
    - Cheaper but weaker option: triggers with `linger_pending`. A pending wakeup can then go to a thread that waits for something else.
27. **`FLOCK` and `fcntl` locks.** A lock list on the vnode. `F_SETLKW` waits on an event. No deadlock detection. Until then mlibc returns
    `ENOSYS`.
28. **CPU time:** `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID`, `getrusage`, `times`, and the `rusage` result of `waitpid`. A
    nanosecond counter in `thread_t`, updated at each context switch. Sum it per process and for waited children.
29. **`MKNODAT` and named FIFOs.** First on `ramfs` (a vnode of type FIFO or device, in memory). Opening a FIFO inode resolves to a pipe object
    that lives with the vnode. An open for read blocks until a writer exists. Then ext2: it must store and read the device and FIFO inode
    types and `rdev`, and `create` must accept the new types.
30. **The rest.** `MREMAP`, `FALLOCATE` (ext2 and `ramfs`, other file systems return `ENOSYS`), `CHROOT` (a root pointer in the process),
    `WAITID` (as `waitpid`, with `siginfo`), `SIGQUEUE` (needs a queue of real-time signals, not a bit mask).

### Later, not scheduled

- **A sysctl-like registry.** Not planned now, and not to be designed before it is needed. The idea so far: values of several types bound
  to a string name (like cvars in Quake). An initcall adds an instance to a list, and the system keeps all of them in a hash map. An
  update can run a trigger. Until then, a kernel setting comes from the command line.

- **pty** (`/dev/ptmx`, a dynamic `/dev/pts`, a master and slave pair on top of `tty_discipline`, the ioctls `TIOCGPTN`, `TIOCSPTLCK`,
  `TIOCSCTTY`, `TIOCNOTTY`). Plan it with the work on a kms console like `kmscon`, which needs more than a pty (input and video
  interfaces). Check first that the discipline does not depend on one driver.

## How Linux and FreeBSD reseed

Read from the source of each (Linux `drivers/char/random.c`, FreeBSD `sys/dev/random/fortuna.c`, current trees). Neither uses a byte count or a
random interval.

| | Linux (ChaCha20) | FreeBSD (Fortuna) |
| --- | --- | --- |
| Reseed trigger | Time. A delayed work reseeds every `CRNG_RESEED_INTERVAL`, which is 60 s. Early in boot the interval is `uptime / 2` seconds, at least 1 s (`CRNG_RESEED_START_INTERVAL`), until uptime reaches 120 s. Extra reseeds: the input pool first becomes ready, resume from suspend, a VM fork, and the `RNDRESEEDCRNG` ioctl. | The read path. `random_fortuna_pre_read` reseeds when pool 0 holds at least the minimum pool size (a tunable, lower bound 16 bytes) and more than 0.1 s have passed since the last reseed. The first seed is exempt from the time limit. |
| Byte count | None. | None for reseed. A request is at most 1 MiB, and a key makes at most 2^16 blocks. |
| Output safety | Fast key erasure: each ChaCha block call replaces the key with the first half of the block and gives out the rest, at most 32 bytes per call. Each CPU has its own key, refreshed from the base key when a generation counter changes. | The generator takes a new key from its own stream after each request, so earlier output stays safe. |
| Pools | One input pool. | 32 pools. Pool *i* joins every 2^*i*-th reseed, so a slow attacker cannot predict the slow pools. |

What this means here:

- The time gate is the common point. Linux gates on time alone and relies on a worker. FreeBSD gates on time and on the amount of noise,
  and does it in the read path. The plan uses FreeBSD's rule, because it needs no assumption about when a thread runs.
- A reseed interval picked at random at init is not in either system. It does not hide anything from an attacker who has the state, so it
  is only a tunable. Use constants in the code.
- Fast key erasure is what makes the interval matter less. It applies to ChaCha20 only. MT19937 cannot have it.
- Linux keeps a generation counter per CPU key. If one lock around the generator becomes a hot spot, copy this idea later.
- Not read: the default minimum pool size of FreeBSD (defined outside `fortuna.c`) and the harvest thread that fills the pools.

## Process size budget

`process_t` is about 240 bytes now, in the 256 byte slab class. Planned additions: nice level (1 byte), job control state (about 32 bytes),
interval timer (about 48 bytes), and the limits (8 bytes as a pointer, or 128 to 256 bytes inline). Together these are over 256.

- Raise the `static_assert` budget to 512. The next slab class holds fewer processes per page, 7 and not 8, because the allocator keeps its
  own metadata at the start of the page. A smaller class wastes less, so do not use the extra room without a reason. The 256 class is
  out of reach anyway: 240 bytes plus even the job state is over it.
- With 512 bytes, the job control state goes inline as one sub-structure (`job_control_t job` in `process_t`), and so do the interval
  timer and the nice level. None of them needs a refcount, because each has one owner.
- The limits are the only large item. Decision: shared and refcounted (item 22, way b).
- Keep the `static_assert` so that the next field fails the build instead of changing the class by accident. Check the real size with
  a build before each field. The count above is by hand.

## Dependencies

- Item 4 needs item 1. Item 5 needs item 20 for the CPU count and the memory size.
- Items 8 to 14, 15, 16 need item 7 (renumbering) when they add a number.
- Item 12 gets its limit check from item 22.
- Item 14 needs item 28 for the CPU time clocks.
- Item 24 needs item 23. Item 29 needs items 16 and 23.
- Item 26 needs item 16 (pipe events) and item 15 (mask swap).
- Item 25 does not depend on pipes. It touches `waitpid`, the signal code, the return to user mode and the tty.

## Priority overrides

The tiers are sorted by cost. These items are done earlier than their tier says:

- Item 25 (job control) goes right after Tier 2, before items 15 (`sigsuspend`) and 16 (`pipe2`). It does not depend on them. It does
  depend on the signal sysdeps (item 3) and on the renumbering (item 7), because a shell test needs both.

## Open questions

- [x] Item 10: no `setdomainname` and no domain name in the kernel. mlibc returns `(none)` (`(null)` is also fine). Decided.
- [x] Item 13: keep the nice value in `process_t` (1 byte, reported back). Decided.
- [x] Item 14: aliases for `MONOTONIC_RAW` and the `COARSE` clocks, for now. `CLOCK_BOOTTIME` is `EINVAL` until suspend exists. Decided.
- [x] Item 6: the `sched_*` stubs live in the kernel (item 13). Decided.
- [x] Item 8: seekability is the contract of the `seek` operation (`-ESPIPE`), not a vnode type or a new flag. Decided (delegated).
- [x] Item 21: `mt19937-64` is not given to user programs. Decided.
- [x] Item 21: add both BLAKE2s (pool) and SHA-256. Decided.
- [x] Item 21: no `source` argument in `GETRANDOM`, and no generator concept in the user ABI. Decided.
- [x] Item 21: the generator has a default in the code and the `random=` command line option overrides it. No `Kconfig` choice and no
  runtime switch. Decided.
- [x] Item 21: no assumption about how often the random thread runs. Safety does not depend on it. Decided.
- [x] Item 21: a blocking `GRND_*` call may hang init. Decided. The Limine seed should prevent it.
- [x] Item 21: ask Limine for 8 values. Limine takes them from RDSEED, RDRAND, the UEFI RNG, and then a predictable PCG32. Low credit.
- [x] Item 21: reseed rule. For ChaCha20, reseed on the read path when the pool has `RANDOM_MIN_POOL_BYTES` (32) of credited noise and
  `RANDOM_MIN_RESEED_MS` (100) have passed. Reseed at every chance before the first seed. No byte count, no random interval. The values are
  constants in the code, and `Kconfig` comes only when someone needs to change them. Decided.
- [x] Process size budget: 512, small fields inline in sub-structures. Decided.
- [x] Item 22: shared refcounted object (b). Decided.
- [x] Item 23: how `ramfs` stores file pages: a memory mode in `fcache` (not a separate page store inside `ramfs`). Decided (delegated).
- [x] Item 25: job control moves before `pipe2` and `sigsuspend`. See "Priority overrides". Decided.

## Not verified

- `kill(-1)`: whether a process list iterator exists (item 18).
- The exact size of `process_t` (about 240 bytes by hand count) and the exact size of its mutex field.
- The number that `RLIMIT_NOFILE` should return. `process_t` has `max_fds`, but the limit constant was not read.
- `sys_sigreturn`: where it expects the stack pointer, for the restorer in item 3.
- How the active clock source stores its frequency, for item 14.
- Whether `mlibc` `clock()` uses `CLOCK_PROCESS_CPUTIME_ID`, and what mlibc expects from robust mutexes (item 1).
- Whether `vms` has a helper to fault a range in, for `MAP_POPULATE`.
- Whether the tty discipline is independent of one driver (pty).
- Nothing in this plan was built or run.
