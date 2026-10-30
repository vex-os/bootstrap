# Syscall plan for a complete POSIX mlibc

Status: agreed, in progress. Networking is out of scope. Struck-through items are *done* and in the kernel; "Decisions made while implementing" lists
what changed against the text below.

The work is sorted from the cheapest item to the most expensive one. Do the items in this order, and leave the last ones for later. Cost is
a rough size: **S** is a few lines, **M** is one new function or one small subsystem piece, **L** is a new subsystem or a change in several
subsystems. The futex item is first although it is not the cheapest, because it blocks every thread feature.

## Assumptions

- There is no userspace except mlibc and the init program. The ABI can change freely.
- The kernel and mlibc are submodules. An ABI change is one kernel commit, one mlibc commit and one bootstrap commit that moves both pointers.
- "Kernel" paths are under `kernel/kernel/`. "mlibc" paths are under `mlibc/sysdeps/vex/`.
- The plan comes from reading the code. Section "Not verified" lists what the author did not read. The table of verified facts shows the
  code as it was when the plan was written: the items marked *done* changed some rows.
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
| Page cache | `fcache` fills pages from the block device (`bmap`, `node->fs->volume`). It never evicts a dirty page. | A memory file system needs a mode of `fcache` with no backing store (done: `fcache_set_memory`). |
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
| Process | `0x20`-`0x3F` | EXIT, FORK, EXEC, WAITPID, GETPID, GETPPID, SETSID, SETPGID, GETPGID, GETSID (SLEEP is gone, slot `0x2A` stays empty) | WAITID, GETPRIORITY (done, `0x2B`), SETPRIORITY (done), GETSCHEDULER (done), SETSCHEDULER (done), GETSCHEDPARAM (done), SETSCHEDPARAM (done), GETSCHEDRANGE (done), GETSCHEDINTERVAL (done), GETAFFINITY (done), SETAFFINITY (done, `0x34`) |
| Thread, futex | `0x40`-`0x5F` | THREAD_CREATE, THREAD_EXIT, GETTID, TCB_SET, YIELD, FUTEX_WAIT, FUTEX_WAKE | none |
| Signal | `0x60`-`0x7F` | KILL, TGKILL, SIGACTION, SIGPROCMASK, SIGPENDING, SIGRETURN, SIGALTSTACK | SIGSUSPEND, SIGTIMEDWAIT, SIGQUEUE, SETITIMER |
| Credentials | `0x80`-`0x9F` | GETRESUID ... CAPSET | none |
| Memory | `0xA0`-`0xBF` | MMAP, MUNMAP, MPROTECT, MSYNC | MADVISE (done, `0xA4`), MLOCK (done, `0xA5`), MUNLOCK (done, `0xA6`), MLOCKALL (done, `0xA7`), MUNLOCKALL (done, `0xA8`), MREMAP |
| Descriptor | `0xC0`-`0xDF` | OPENAT ... FCNTL, PREAD (done), PWRITE (done) | PIPE (done, `0xD3`), PPOLL, READV (done, `0xCF`), WRITEV (done, `0xD0`), PREADV (done, `0xD1`), PWRITEV (done, `0xD2`), FLOCK, FALLOCATE |
| Path | `0xE0`-`0xFF` | FSTATAT ... FACCESSAT | MKNODAT, CHROOT |
| xattr, vfs | `0x100`-`0x11F` | GETXATTRAT ... REMOVEXATTRAT, MOUNT, UMOUNT, SYNC, SYNCFS, STATVFS | none |
| System | `0x120`-`0x13F` | UNAME, GETTIME, SETTIME, SETHOSTNAME (done), NANOSLEEP (done, `0x124`; replaces SLEEP) | GETTIMERES (done, `0x125`), GETRLIMIT, SETRLIMIT, GETRANDOM, SYSINFO |

The calls that take a clock id have no `CLOCK_` in their names (`GETTIME`, `NANOSLEEP`, `GETTIMERES`), so that a `__k_SYS_*` number cannot be
mistaken for a clock id (`__k_CLOCK_*`). mlibc still calls them `clock_*`.

Keep the order inside a block. A new call takes the next free number of its block, in the order that the calls are added and not in the order
of the table. The comment in `usr/vex/syscall.h` says 32 (done).

## Work items, cheapest first

### Tier 0: unblock and repair

1. ~~**Futex with shared keys (M).**~~ *Done.* Decision: do it properly now.
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
2. ~~**`mmap` flags (S to M).**~~ *Done.* Add the flags to `usr/vex/mman.h` and mlibc `vm-flags.h`. The values are the Linux ones, as for `FIXED` and
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

7. ~~**Renumber (S).**~~ *Done.* See Numbering. Do this before any new syscall.
8. ~~**`PREAD`, `PWRITE` (S).**~~ *Done.* Arguments: `fd, buf, len, offset`.
   - `file_read` and `file_write` take a pointer to the position and move it. `read` passes `&file->offset`. `pread` passes a private copy
     and leaves the offset of the file alone. One chunk loop in `sys_file.c` serves all four calls. `file_read` takes no credentials,
     because access is checked in `open`. `file_write` keeps them for `vnode_remove_privs`.
   - `file_t` has a `mode` field. `file_open_node`, the only place that makes a `file_t`, sets `FILE_SEEKABLE` once, from a probe
     `vnode_seek(node, file, 0, SEEK_CUR)`. A result of `-ESPIPE` means a stream. `pread` and `pwrite` check the bit once per call
     (`ESPIPE`, before `EBADF`, as Linux does). They return `EINVAL` for a negative offset or a range that wraps.
   - Contract: a node that cannot seek returns `-ESPIPE` from `seek`. A node with no `seek` operation (regular files, directories) leaves
     the arithmetic to `file_seek`, which still takes `-ENOSYS` to mean that. The `seek` call returns the new offset.
   - Char devices: no `seek` operation means a stream (tty, serial, kplog, hwtree). The framebuffer has a `seek` of its own, because its
     memory is addressed by byte. It keeps the offset in `[0, info.size]`.
   - `pwrite` writes at the given offset also with `O_APPEND`, as POSIX has it (Linux appends). `sys_write` moves to the end of the file
     itself, before each chunk.
   - `UNDONE:` `file_seek` does not use `FILE_SEEKABLE` yet. It asks the node on every call and gets the same answer.
   - Still to do: the `Pread` and `Pwrite` sysdeps in mlibc.
9. ~~**`NANOSLEEP` replaces `SLEEP` (S to M).**~~ *Done.* Arguments: `clock, flags, request, remaining`. `clock` is `CLOCK_REALTIME` or
   `CLOCK_MONOTONIC`. `flags` is `0` for a duration or `TIMER_ABSTIME` (`1`) for a point on the clock. `remaining` is written only for a
   relative call that a signal ends.
   - The call moves to the system block (`SYS_NANOSLEEP`, `0x124`), with the handler in `sys_time.c`. Slot `0x2A` in the process block stays empty.
   - Order: first one helper in `vex/time.h` that turns a `timespec` into nanoseconds, and a deadline from now, both with saturation.
     `deadline_after` in `sys_futex.c`, `sleep_nanoseconds` and the new call use it. This is a commit of its own.
   - A relative call is the body of `sys_sleep`. An absolute call waits for `max(0, request - now(clock))`.
   - An absolute wait on `CLOCK_REALTIME` follows `settime`: `wallclock_set` triggers an event under a lock (`wallclock_wait_until`). The
     sleeper computes its timeout under the same lock, and `event_await_unlock` releases it only after the sleeper listens, so no
     generation counter is needed. When the event fires, it computes the timeout again. A clock set forward past the point ends the wait at once. A clock set back makes it longer. The
     other kinds of wait pay nothing. Read `wallclock.c` first.
   - Other clocks give `EINVAL` until items 14 and 28.
   - A zero timeout, or an absolute point in the past, yields and returns 0. A signal gives `EINTR`; `remaining` is written for a relative call only.
   - `ktest`: `test.sleep.c` covers both clocks and absolute points. A signal and `settime` during a wait are not tested.
   - Still to do: `Sysdeps<Sleep>` in mlibc as a relative `CLOCK_MONOTONIC` call of `SYS_NANOSLEEP`.
10. ~~**`SETHOSTNAME` (S).**~~ *Done.* Number `0x123`. Arguments: `name, length`. The name is not terminated, as in Linux, and has at most `__k_UNAME_SIZE - 1` (255)
    bytes. The default is `CONFIG_DEFAULT_HOSTNAME`, `vex` unless the build changes it. The name lives in `core/hostname.c` behind `vex/hostname.h` (`hostname_get`, `hostname_set`), so that a user in the kernel, such as a
    network stack, can read it. A spinlock guards it, so any context can read. There is a real default instead of `"IMPLEMENT_ME"`.
    Require `CAP_SYS_ADMIN`. Add the `SetHostname` sysdep. `setdomainname` is not planned, see the open questions.
11. ~~**`READV`, `WRITEV` (M).**~~ *Done*, with `PREADV` and `PWRITEV` (no flags argument). Arguments: `fd, iov, count` and, for the
    positioned calls, `offset` last.
    - The point of the call is that the data of a `writev` of up to a page reaches the file as one `vnode_write`, and so as one write
      of a pipe, which `pipe` makes atomic up to `PIPE_BUF` (a pipe has to keep `PIPE_BUF <= FILE_IO_CHUNK`). There is no lock for
      the whole call: the file layer has none, and a plain `write` of more than a page is not atomic either.
    - `struct __k_iovec` is in `usr/vex/uio.h`, `IOV_MAX` is 1024. A longer array, or lengths that add up to more than `INT64_MAX`, give
      `EINVAL` before any data moves. Empty buffers are skipped.
    - `sys_file.c` has one `io_cursor_t` that walks one buffer or an iovec array, read one element at a time, and `read_chunks` and
      `write_chunks` take it. The writer gathers the buffers into one chunk before it calls `file_write`. A read goes on over the buffers
      until a short result, the end of the file, or a device that hands out what it has (`hands_out_at_once`: add `VNODE_FIFO` there with
      item 16).
    - A fault after some data moved reports that data. The first fault reports `EFAULT`.
    - Still to do: the `Readv` and `Writev` sysdeps in mlibc.
12. ~~**`MADVISE`, `MLOCK`, `MUNLOCK`, `MLOCKALL`, `MUNLOCKALL` (S).**~~ *Done.* Real syscalls whose body checks the arguments and does nothing else.
    - Done as planned, with these details. The values are the Linux ones: `MADV_NORMAL` to `MADV_DONTNEED` (0 to 4, any other value is
      `EINVAL`) and `MCL_CURRENT`, `MCL_FUTURE`, `MCL_ONFAULT`. A zero length succeeds. `mlock` and `munlock` round the range out to whole
      pages, as Linux does, while `madvise` wants an aligned start. A range that leaves user space, or has a hole, gives `ENOMEM`
      (`vms_check_mapped`). `mlockall` needs `MCL_CURRENT` or `MCL_FUTURE`, and `MCL_ONFAULT` alone is `EINVAL`. There is no capability
      check yet.
    - `MADV_DONTNEED` is real: `vms_discard` drops the pages of the range and keeps the regions. The next access faults a fresh page in:
      zeros for anonymous memory, the file for a private file mapping (the copies of earlier writes are gone), and the same data for a
      shared one. Device memory gives `EINVAL`, as in Linux. `MADV_WILLNEED` is still a no-op (`UNDONE`: read ahead).
    - Still to do: the `Madvise` sysdep and the `mlock` family in mlibc.
    - The plan as it was: `madvise`: page-aligned start, a known advice value, and `ENOMEM` if part of the range is not mapped. No effect yet. Later,
      `MADV_DONTNEED` frees the pages of an anonymous mapping.
    - `mlock` family: the same range checks. `UNDONE:` the lock matters when swap exists. Add a capability and a limit check when item 22
      exists.
13. ~~**`GETPRIORITY`, `SETPRIORITY`, and the `SCHED_*` calls.**~~ *Done*, with a real effect of the nice value. Numbers `0x2B` to `0x34`
    in the order of the table above.
    - The nice value is `int8_t nice` in `process_t`, in the padding after `exit_signal`: `sizeof(process_t)` stays 248, so the budget
      stays 256 and the `static_assert` is a tripwire as before. A child inherits it. `exec` keeps it. The scheduler reads it without a lock.
    - Effect: the timeslice of a thread is `SCHED_TIMESLICE_NS` scaled by nice, a fifth less per step up and a quarter more per step down,
      cut to 100 us and to four times the base. A table of 40 values, built in `sched_init`. A thread with no process gets the base.
      `thread_t.timeslice` is gone: `timeslice_of` computes it. The run queues are still one FIFO per hart, so this is a share of the
      time and not a priority. `UNDONE:` a wakeup of a thread with a higher priority does not run before the others.
    - `getpriority` returns `NICE_BIAS - nice` (1 to 40), so that a result is not taken for an error. `setpriority` cuts the value to
      the range. `PRIO_PROCESS`, `PRIO_PGRP` and `PRIO_USER` all work (`process_for_each`, a new walk of the process table that holds a
      reference to each process and may sleep). `who` of zero is the caller, its group, or its real uid.
    - `CAP_SYS_NICE` (23, as in Linux): needed to lower the nice value and to change the process of another user. A process of the same
      user, by effective uid, may raise its nice value. Errors as in Linux: `EPERM` for a process that is not ours, `EACCES` for the lowering.
      `RLIMIT_NICE` lets a process lower it that far (item 22).
    - The only policy is `SCHED_OTHER` with priority 0. Anything else is `EINVAL`. `GETSCHEDRANGE(policy, range*)` writes `{0, 0}`.
      `GETSCHEDINTERVAL` gives the timeslice that the process has by its nice value. The `pid` of these calls is a process, and a
      negative one is `EINVAL`.
    - `GETAFFINITY(pid, size, mask*)` writes the online harts as whole 64 bit words and returns the size of the mask, as Linux does.
      `SETAFFINITY` takes a mask with at least one online hart and does nothing else. `UNDONE:` the scheduler keeps the thread off the
      other harts. Today `bound_hart` is one hart for a kernel thread. A mask means the choice in `choose_hart`, the stealing in `rq_steal`
      and the wakeup in `kick_hart` look at it.
    - Still to do: the sysdeps in mlibc, and the conversion of `getpriority`.

14. ~~**`GETTIMERES` and more clock ids (S).**~~ *Done.* `GETTIMERES` is `0x125`. The clock ids have the Linux values. `canonical_clock` in
    `sys_time.c` maps the aliases, and `gettime`, `gettimeres` and `nanosleep` use it. `settime` takes `CLOCK_REALTIME` only. The resolution is
    `clock_source_resolution_ns` (`mult / 2^shift`, rounded up, at least 1 ns). A null pointer only checks that the clock exists. The
    table below is the plan as it was. Answer: the clocks that programs use, besides `CLOCK_REALTIME` and `CLOCK_MONOTONIC`, are:

    | Clock | Meaning | Plan |
    | --- | --- | --- |
    | `CLOCK_MONOTONIC_RAW` | Monotonic time with no slewing. | Alias of `CLOCK_MONOTONIC`. The kernel only steps the realtime clock, so they are equal. |
    | `CLOCK_BOOTTIME` | Monotonic time that counts suspend. | `EINVAL`, with a `TODO` in `sys_gettime`: suspend will exist later, and an alias would then be wrong. |
    | `CLOCK_REALTIME_COARSE`, `CLOCK_MONOTONIC_COARSE` | Faster, less exact reads. | Aliases of the fine clocks. |
    | `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID` | CPU time of the process or the thread. POSIX requires them, and `clock()` is likely to use the first. | Item 28. Until then `EINVAL`. |
    | `CLOCK_TAI` | International atomic time. | Not planned. |

    `GETTIMERES(clock, timespec*)` returns the step of the active clock source: roughly one tick, from the `mult` and `shift` of
    `clock_source_t`. Check how the active source stores its frequency before you write it. Then `Sysdeps<ClockGetres>` is a plain call.

### Tier 3: one new piece of kernel (M)

15. ~~**`SIGSUSPEND`**~~ *Done* (`0x067`, the mask by pointer; `pause` calls it with the current mask in mlibc).
    - Add `saved_sigmask` and a flag to `thread_t`.
    - The call saves the mask, sets the new one, and waits with no timeout, as `sys_sleep` does.
    - Never return `-ERESTART`: the syscall would run again and overwrite the saved mask. Return `-EINTR` after the wait ends.
    - In `signal_deliver`, pass the saved mask instead of the current one as `old_mask`, then clear the flag. `sigreturn` then restores the
      original mask on its own.
    - A wakeup always means a deliverable signal (see the `queue_signal` fact), so no loop is needed.
16. ~~**`PIPE`, with a pipe service.**~~ *Done.* `SYS_PIPE(int fds[2], flags)` is `0xD3`. It takes `O_CLOEXEC | O_NONBLOCK` and has no
    older form without flags, so it has no `2` in its name.
    - The code is the service `ipc/pipe.c` with the header `vex/pipe.h` (`pipe_create`). The header names no filesystem. `ipc` is a new
      top directory with the log subsystem `KP_IPC`: local sockets will go there too.
    - The pipes live in an internal filesystem, `pipefs`, which only the service knows. A driver with the new `internal` flag serves
      `vfs_create` only, and `vfs_mount` gives `ENODEV` for it, so `mount(2)` cannot make a second one. The filesystem is there for
      `node->fs`, `st_dev` and the `ino` of the node. A pipe node is not in the `vcache`: it lives as long as its two files.
    - New `VNODE_FIFO` type. `stat` reports `S_IFIFO`, `getdents` reports `DT_FIFO`. `seek` gives `ESPIPE`, so `pread` and `pwrite` too.
    - The pipe object is a queue of up to 16 pages, not a ring: a page goes back as soon as it is read empty, so an empty pipe holds none.
      Each page counts against the commit limit (item 31) when the write takes it, and a write that cannot get one returns what it wrote,
      or `ENOMEM`. One spinlock, the counts of readers and writers (kept by `open` and `close`), and two events.
    - `read`: empty and no writer returns 0. Empty and `O_NONBLOCK` returns `EAGAIN`. A read hands out one page at most, and
      `read_chunks` stops after it (`hands_out_at_once` has `VNODE_FIFO`).
    - `write`: no reader sends `SIGPIPE` to the writing thread and returns `EPIPE`. A piece of up to `PIPE_ATOMIC_SIZE` (one page, which
      is `FILE_IO_CHUNK`) goes in all or nothing, and a longer write goes in as there is room.
    - `FIONREAD` works on both ends, like in Linux.
    - Tests: `ktest/test.pipe.c`.
    - Still to do: the `Pipe` sysdep in mlibc, and the waiter list of item 26.
17. ~~**`SIGTIMEDWAIT`**~~ *Done* (`0x068`, `signal_take` in `proc/signal.c`; `ktest/test.sigtimedwait.c`).
    - Arguments: the set by pointer, the `siginfo` by pointer or null, the `timespec` timeout by pointer. A null timeout waits as long as it
      takes, and a zero one only looks. It returns the signal number, `EAGAIN` on timeout and `EINTR` for a handler of another signal.
    - The set is unblocked for the time of the wait (`signal_replace_mask`), so the arrival of a signal wakes the thread. A signal of the
      set is taken first, even when it has a handler. Nothing returns `-ERESTART`.
18. ~~**`kill(-1)`.**~~ *Done.* Every process but init and the sender; 0 if one was signalled, `EPERM` if none may be, `ESRCH` if there are none.
19. ~~**`SETITIMER`**~~ *Done* (`0x06A`; `0x069` stays for `SIGQUEUE`). `proc/itimer.c`, `ktest/test.itimer.c`.
    - `SETITIMER(which, itimerspec*, itimerspec*)` has one timer, `ITIMER_REAL`, in nanoseconds. A null new value only reads, so
      `getitimer` is the same call. `alarm` is `setitimer` in mlibc.
    - The timers sit in a list by deadline, and the scheduler tick (`itimer_run_expired`) sends `SIGALRM` with `signal_raise`, which
      allocates nothing. An interval timer skips the periods it missed.
    - The state is 24 bytes in `process_t`. A fork does not inherit it. **Differs from the first plan:** an `exec` keeps the timer, as
      POSIX and Linux have it. `process_finish_exit` removes it, and `itimer_set` refuses to arm it after that.
    - `UNDONE:` `ITIMER_VIRTUAL` and `ITIMER_PROF` need the CPU time of item 28.
20. ~~**`SYSINFO`.**~~ *Done* (`0x126`, `usr/vex/sysinfo.h`; `total_pages` is `pslab` only). One structure: online CPUs, total and free pages, uptime. Take the data from `hart` and `pframe`. Then `Sysconf` can use it.
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
22. ~~**`GETRLIMIT`, `SETRLIMIT`.**~~ *Done* for the store, `RLIMIT_NOFILE` and `RLIMIT_STACK` (`0x127`, `0x128`).
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
    - Done: the limits are one refcounted object in `process_t` (`proc/rlimit.c`); a change copies it under `process->lock`. The
      numbers are the Linux ones, and the hard limit goes up only with the new `CAP_SYS_RESOURCE` (24). `RLIMIT_NOFILE` caps the
      descriptor table (`EMFILE`; `dup` past it gives `EBADF`, `F_DUPFD` past it `EINVAL`; a limit under the descriptors in use only
      turns away new ones), and its hard limit may not pass `CONFIG_MAX_FDS`. `RLIMIT_STACK` sizes the stack that exec maps: 64 KiB
      at least, 1 GiB at most, and `RLIM_INFINITY` gives the default. The other limits are stored only.
    - Done: `RLIMIT_NICE` lets a process without `CAP_SYS_NICE` lower its nice value down to `20 - rlim_cur`.
    - `UNDONE:` `RLIMIT_MEMLOCK` with `CAP_IPC_LOCK` for `mlock`.
    - Done: the stack grows. Exec maps 128 KiB, and a fault under the stack grows it down to `RLIMIT_STACK`, with a guard gap of 256 pages
      under it, as in Linux. The growth takes its pages from the commit limit when it happens; a fault that gets none ends in `SIGSEGV`.

### Tier 4: expensive (L)

The order inside this tier follows what is wanted first, not only cost.

23. ~~**`tmpfs`** (a memory file system), planned from the start.~~ *Done.*
    - A `vfs_driver` named `tmpfs`, mounted for `/tmp` and `/run`. It has files, directories and symlinks with all data in memory.
      It needs `lookup`, `iterate` (a cookie that survives `unlink`), `create`, `unlink`, `rename`, `link`, `symlink`, `read`, `write`,
      `set_attr`, `stat`, `fault`.
    - Pages: a memory mode of `fcache` (`fcache_set_memory`). With no `bmap`, a missing page is filled with zeros, allocation does nothing,
      writeback does nothing, and reclaim never takes the page, because the page is the only copy. A read of a hole takes no page.
    - Unlinked-but-open files must keep their pages (`release` frees them).
    - Commit limit (item 31): a file takes `ceil(size / PAGE_SIZE)` pages from the limit when `ftruncate` or a write makes it longer, holes
      included, and gives them back when it shrinks or goes. A call that does not fit returns `ENOSPC`, so a fault never fails for the limit.
      The mount options `size=` and `nodes=` limit one mount; without them `/tmp` and private memory share the one limit.
    - Vnode types for FIFOs and devices are only a field here, so `mknodat` works on `tmpfs` before it works on ext2.
    - Done: mount and its options (`size=`, `nodes=`, `mode=`, `uid=`, `gid=`), `statvfs`, files, directories, `lookup`, `iterate`,
      `create`, `unlink`, `read`, `write`, `stat`, `set_attr`, `fault`, and `ktest/test.tmpfs.c`.
    - Done later: `rename`, `link`, `symlink` and `readlink`. A symlink target is a slab object up to `SLAB_MAX_SIZE` bytes and a
      `pslab` page above that, up to `PATH_MAX - 1` bytes. `UNDONE:` the targets are not counted in the limits of the mount.
24. ~~**Shared anonymous `mmap` and POSIX shared memory.**~~ *Done* (`shm_open` needs no kernel work).
    - `MAP_SHARED | MAP_ANONYMOUS` maps a file that no name leads to. `kernel/mm/shmem.c` (`vex/shmem.h`) keeps an internal tmpfs
      that `vfs_create` makes, and makes the file with a name that it removes at once. The file takes its whole size from the commit
      limit when the mapping appears (`ENOMEM` if it does not fit) and gives it back with the last reference. A part `munmap` returns
      nothing. `MAP_NORESERVE` has no effect on it.
    - The service names the `tmpfs` driver with a string and uses only `vfs_*`, `vnode_*` and `file_*`. A filesystem has no public
      header (architecture.md). `UNDONE:` a `tmpfile` vnode operation would save the create and unlink.
    - `shm_open` is a file in `/run/shm`, a `tmpfs` mount. The mount itself belongs to init and mlibc.
    - Tests: `ktest/test.shmem.c`.
25. ~~**Job control.**~~ *Done* in the kernel. Decision: implement it. The existing pieces are `exit_to_user`, `pending_work`, `sched_kick`, `child_exited` and the
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
    - Done: one inline `job_control_t job` in `process_t` (`proc/job.c`). A default stop sets `THREAD_WORK_STOP` on every thread, and a
      thread parks in `exit_to_user` on `job.resume`, which no signal ends (SIGKILL and exec wake it through `job_wake_locked`). `SIGCONT`
      acts at send time (`job_effects` in `signal.c`) and drops the pending stop signals, a stop signal drops a pending `SIGCONT`.
      `waitpid` takes `WUNTRACED` and `WCONTINUED` (`0x7f | sig << 8`, `0xffff`), a report is taken once with an atomic bit, and the
      parent gets `SIGCHLD` unless `SA_NOCLDSTOP`.
    - Done: a wait that returns `-ERESTART` and does not restart itself (`nanosleep`, `sigsuspend`) calls `job_resume_wait`. It delivers
      the signals that run no handler, parks for a stop, and goes on. A stop does not end `sigsuspend`.
    - Done: a background group gets `SIGTTIN` on a read, and `SIGTTOU` on a write with `TOSTOP` and on `TCSETS*`, with `-ERESTART`; a
      refused `SIGTTIN` or an orphaned group gives `EIO`, a refused `SIGTTOU` lets the access through (`tty_interface.c`).
    - Done: a group is orphaned when no member has a parent outside it in the same session (`process_group_orphaned_locked`; exited
      members and children of init do not count). When an exit cuts the last tie and a member is stopped, the group gets `SIGHUP` and
      `SIGCONT`. The exiting thread does it in `sched_terminate` (`process_check_orphans`), not in `begin_exit`, which can run under
      the lock of the groups. The signals of a terminal do not stop a process of an orphaned group.
    - Done: a `setpgid` or `setsid` that cuts the last tie of a group hangs it up like an exit (Linux does not do this, POSIX does), and
      `tcsetpgrp` from the background sends `SIGTTOU` (`TIOCSPGRP`; a shell ignores `SIGTTOU` around it).
    - `UNDONE:` `ptrace` stops, and `CLD_TRAPPED`, do not exist.
    - Still to do in mlibc: the `WUNTRACED`/`WCONTINUED` options and the status macros, `SA_NOCLDSTOP`, and a shell test.
    - Check `SIGTSTP`, which the tty already sends on `^Z`.
26. ~~**`PPOLL`**~~ *Done* in the kernel (`0xD4`, `usr/vex/poll.h`). `poll`, `select` and `pselect` go on top of it in mlibc.
    - Arguments: `fds, count, timeout, mask`. The timeout is relative, `nullptr` waits for as long as it takes, and the call does not
      write back what is left. The mask is a pointer to a `sigset_t`, with no size argument, as in `SIGSUSPEND`.
    - A signal gives `EINTR`, never a restart. `count` above `RLIMIT_NOFILE` gives `EINVAL`.
    - The design below changed: see "Decisions made while implementing".
    - Tests: `ktest/test.poll.c`.
    - A loop of "check all, then wait on an event array" loses wakeups: `event_await_unlock` holds one lock, the files have different locks,
      and tty triggers keep no pending wakeup.
    - Fix: each poll puts its own `event_t` on the stack and registers it in a waiter list on every file before the check. A new vnode
      operation `poll(file, waiter)` registers the waiter (if not `nullptr`) and returns the ready bits. Pipe and tty triggers also wake the list.
    - Files without the operation are always ready. An invalid fd gives `POLLNVAL`. A closed pipe writer gives `POLLHUP`.
    - The signal mask swap is the same code as in item 15.
    - Cheaper but weaker option: triggers with `linger_pending`. A pending wakeup can then go to a thread that waits for something else.
27. ~~**`FLOCK` and `fcntl` locks.**~~ *Done* (`fs/file_lock.c`, `ktest/test.flock.c`). A lock list on the vnode. `F_SETLKW` waits on an event.
    - One kind of range lock for all three interfaces. The owner is the process for POSIX locks (`F_GETLK`, `F_SETLK`, `F_SETLKW` = 6 to 8)
      and the open file for OFD locks (`F_OFD_GETLK`, `F_OFD_SETLK`, `F_OFD_SETLKW` = 9 to 11) and `SYS_FLOCK` (`0xD5`). `flock` is a lock of
      the open file over `[0, end)`, so it conflicts with `fcntl` locks, as in FreeBSD and not as in Linux.
    - POSIX locks go when the process closes any descriptor of the file, as POSIX requires. OFD and `flock` locks go with the last close of
      the open file. `F_GETLK` reports pid -1 for a lock of an open file, and the `F_OFD_*` calls take `l_pid` zero.
    - `flock` drops the old lock before it changes its type, as Linux and BSD do, so that two owners that upgrade at once do not wait on
      each other forever. `fcntl` changes the type in one step.
    - `EDEADLK` for cycles of processes only, up to a chain of 10, as in Linux. An open file can be shared between processes, so a cycle of
      open files is not always a deadlock.
    - Advisory only, on any kind of file.
28. ~~**CPU time:**~~ *Done* (`GETRUSAGE` is `0x129`; `proc/cputime.c`, `time/cycles.c`, `ktest/test.cputime.c`). `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID`, `getrusage`, `times`, and the `rusage` result of `waitpid`. A
    nanosecond counter in `thread_t`, updated at each context switch. Sum it per process and for waited children.
    - **Counter.** The time comes from a cycle counter of the hart, not from a clock source. Counters of two harts can differ, so a
      value is only compared with a value of the same hart. A thread starts and ends each run on one hart, so this is sufficient. The
      cycle counter never becomes a `clock_source`.
    - **Arch interface** (`<arch/cycles.h>`): `arch_cycles_read()` (`static __always_inline`), a probe that tells if the counter has a
      constant rate, and the frequency when the arch knows it. amd64 uses `rdtsc`. riscv64 (`rdtime`, `timebase-frequency`) and aarch64
      (`CNTVCT_EL0`, `CNTFRQ_EL0`) know the frequency and need no calibration.
    - **Generic part** (`kernel/time/cycles.c`): mult and shift as in `clocksrc.c`, the conversion to nanoseconds, and the calibration.
    - **Calibration**, when the arch gives no frequency: once on the boot hart, against the active clock source, as `lapic_timer.c` does.
      At each end of the window, take several samples of "counter, clock source, counter" and keep the narrowest. Window 20 to 50 ms,
      3 to 5 rounds, the median wins. On amd64 a frequency from CPUID `0x15`/`0x16` or the hypervisor leaf `0x40000010` is used when
      present, and a difference of more than about 1% against the calibration gives `pr_warning`.
    - **When the TSC is usable.** Accounting needs a constant rate, not a TSC that runs in deep C-states: the TSC stops there only while
      the hart is idle. So the TSC is usable when one of these is true: CPUID says invtsc; the hypervisor bit is set (QEMU, KVM and
      Hyper-V give a virtual TSC with a constant rate; the default `qemu64` CPU of `ktest` has no invtsc); the family and model have a
      constant TSC (Intel from Core 2, AMD from family `0x10`), as Linux sets `CONSTANT_TSC`. Otherwise the counter is
      `clock_source_now_ns()`: correct, but slow (a global lock and an HPET read), which matters only on K8 and early Pentium 4. The
      command line option `cputime=clocksrc` selects that path, to test it.
    - `NOTE:` a TSC that stops in deep C-states makes the idle time of a hart too small. This matters only for idle accounting later.
    - **User and system time** are exact from the start: the counter is read at every entry to the kernel (system call, interrupt,
      exception) and at every exit to user mode, and at each context switch.
    - A thread gives its time to its process when it exits. A process gives its own time and the time of its children to the
      `children` fields of its parent at `wait`.
    - `CLOCK_THREAD_CPUTIME_ID` for the caller: the stored time plus the current run. `CLOCK_PROCESS_CPUTIME_ID`: the time of the
      process plus the threads that live. A thread that runs on another hart lags by its current run, at most one timeslice, as the
      counter of another hart cannot be read.
    - **ABI.** `GETRUSAGE(who, usage)` with `RUSAGE_SELF`, `RUSAGE_CHILDREN`, `RUSAGE_THREAD`. `struct __k_rusage` in
      `usr/vex/resource.h` has the Linux layout. Done also: `ru_maxrss` (the peak of resident pages that rmap counts, kept over an exec; the children give their maximum), `ru_minflt`, `ru_nvcsw`, `ru_nivcsw` and `ru_nsignals`. `ru_majflt` (`VMS_FAULT_MAJOR` from `fcache_fault` when the page was not in the cache), `ru_inblock` (in `block_read`, for the thread that waits) and `ru_oublock` (when a page becomes dirty, for the thread that dirties it, and in `devfs_block_write`), in units of `S_BLKSIZE` (512), which `st_blocks` now uses in every filesystem. A truncation does not take cancelled writes off `ru_oublock`, unlike Linux. A stop or a continue reports the usage of the child so far. `waitpid` and `waitid` get a `rusage`
      argument that can be `nullptr`. This breaks the ABI, and the mlibc stage repairs it. `times` goes in mlibc on top of `getrusage`
      and `CLOCK_MONOTONIC`, with no syscall.
29. ~~**`MKNODAT` and named FIFOs.**~~ *Done* (`SYS_MKNODAT` is `0xF0`, `CAP_MKNOD` = 27; `ktest/test.fifo.c`).
    - A FIFO or a device node of a filesystem gets `special_ops` from `vnode_init_special`: open, close, read, write, seek, ioctl and poll go
      there, and stat, setattr and the rest stay with the filesystem, as `i_fop` in Linux. So `fstat`, the permissions and the locks are the
      ones of the real inode. A filesystem makes such nodes through the `mknod` operation; without it, `mknodat` fails with `EPERM`.
    - The pipe of a FIFO lives in `special_data` from the first open to the last close, and its data goes with the last close. An open for
      reading waits for a writer and one for writing waits for a reader; with `O_NONBLOCK` the reader goes on and the writer fails with
      `ENXIO`; `O_RDWR` never waits. A waiter counts the opens of the other end, so one that came and went is not missed.
    - `tmpfs` keeps FIFOs and device nodes with `rdev`; ext2 keeps FIFOs. A type of zero or `S_IFREG` makes a regular file, as in Linux.
    - Device nodes outside devfs only show their type and `rdev`; their open fails with `ENXIO`. `UNDONE:` finding the interface by `rdev`,
      with a `nodev` mount option, when something needs it. ext2 makes no device nodes: `dev_t` takes 64 bits, and an inode of ext2 has 32.
    - On the way, `mount` and `umount` resolve the mount point as any other path: from the working directory and the root of the
      caller, with its permissions. `umount` finds the mount by its filesystem, not by the text of the path.
30. **The rest.** `MREMAP`, `FALLOCATE` (ext2 and `tmpfs`, other file systems return `ENOSYS`), ~~`CHROOT`~~ (*done*, `0xEF`, `CAP_SYS_CHROOT` = 18; `process->root` is inherited by a fork and kept by an exec; the path code gets it through `path_set_root_source`, and `..` and absolute symlinks stop at it; the working directory is not moved, so a process that keeps one outside the new root can leave through it, as in Linux; `ktest/test.chroot.c`),
    ~~`WAITID`~~ (*done*, `0x035`: `P_ALL`, `P_PID`, `P_PGID`, `WEXITED`, `WSTOPPED`, `WCONTINUED`, `WNOHANG`, `WNOWAIT`; shares `wait_child` with `waitpid`; `ktest/test.waitid.c`), `SIGQUEUE` (needs a queue of real-time signals, not a bit mask).

31. ~~**Memory commit accounting.**~~ *Done.* Decision: strict, like the Windows commit charge and Linux `vm.overcommit_memory=2`. A call that
    would take the total over the limit fails with `ENOMEM` at the call, so that no process finds out about it on a page fault. It goes
    after item 13 and before `tmpfs` (see "Priority overrides"), so that shared anonymous memory is counted from the start.
    - The account is a property of the region, not of the calls. `vm_region_t` gets a flag for a counted region, and its size is known from
      its bounds. The global counter changes only where a region appears, goes away or changes the flag: `insert_region`, `free_region`,
      `vms_protect`, `vms_clone`. `split_region` keeps the sum, so `munmap` of a part, `exec` and exit need no code of their own.
    - Counted: private writable memory, anonymous or file (a write may copy a page), and shared anonymous memory once. Not counted:
      `PROT_NONE`, read-only private mappings, and shared file mappings. `mprotect` of a private region to writable counts it, and can
      fail with `ENOMEM`: this is the reserve and commit pair of Windows. `MADV_DONTNEED` does not uncount, as the region stays.
    - The limit is the number of managed pages minus a margin for the kernel (the slab, stacks, page tables are not counted). With swap
      it grows by the size of swap. The page cache is not counted, as it can be reclaimed.
    - `fork` counts the private writable memory of the child too, and returns `ENOMEM` when it does not fit. POSIX allows this. Redis
      (a background save) and Java (`Runtime.exec`) are the known programs that meet it; shared memory is counted once and does not
      double. `UNDONE:` a call that starts a program without a copy of the address space (`vfork` or a spawn call), as strict systems
      have it.
    - `MAP_NORESERVE`: a process that holds a new capability may ask for a mapping that is not counted, and a page that cannot be
      given on a touch sends `SIGBUS`. Any other process passes the flag with no effect and no error, as programs pass it out of habit.
      `UNDONE:` the capability (the next free number after `CAP_DEVICE`) and a small reserve of the limit for processes that hold it.
    - A fault that cannot get a page sends `SIGBUS` (`BUS_ADRERR`), as it does now for every error other than `EFAULT` and `EACCES`.
      An OOM killer is not planned: strict accounting makes it rare. Come back to it when it is a problem.
    - Item 2 (`MAP_NORESERVE`) and item 12 (`mlock`) lie next to this. Item 22 (`RLIMIT_AS`, `RLIMIT_DATA`) is a different limit, per
      process.
    - Done as planned, with these details:
      - `mm/commit.c` has `commit_charge`, `commit_charge_replacing` and `commit_uncharge` on one atomic counter, with no lock. The limit
        is `pslab_numpages` minus `max(pslab_numpages / 8, 4096)` pages, set at `initcall_bsp_pmm`. `pdma` is not counted, because only
        drivers and the slab take pages from it. `pslab_numpages` and `pslab_numfree` are variables now, and the getters are gone.
      - There is no flag for a counted region. `counts_commit` in `vms.c` gives the answer from the fields: not device, not shared, not
        `noreserve`, and `VPROT_WRITE`. `alloc_region` takes the pages and `free_region` gives them back, so `split_region`, `munmap`,
        `exec` and exit need no code. `destroy_region` is the part of `free_region` that takes nothing back: the list of stale regions
        uses it, as the pages went already.
      - `vms_map_any` passes the size to `alloc_region`, as the start is not known yet.
      - `vms_protect` splits first, then counts the regions that start or stop counting. It takes the gain before it changes anything, and
        a failure leaves the regions split, which a program does not see.
      - `vms_map_replace` counts the pages that it replaces, and `commit_charge_replacing` lets the total pass the limit by that many for
        a moment. The old mapping stays when the call fails. Until the replaced pages are given back, every other charge fails.
      - `exec` builds the new image beside the old one, so both count for a while. This is accepted: a `fork` already paid for the old
        image, and the new image is small.
      - `MAP_NORESERVE` works for a process with `CAP_VM_NORESERVE` (number 50). The flag is the `flags` argument of `vms_map*`
        (`VMS_NORESERVE`) and the field `noreserve` of the region. `vm_backing_t` is not the place: it describes only the file. Without
        the capability the flag does nothing. The fault path already sends `SIGBUS` when a page is not available.
      - `UNDONE:` a small part of the limit for the holders of `CAP_VM_NORESERVE`. `UNDONE:` a call that starts a program without a copy
        of the address space.
      - Shared anonymous memory (item 24) and `tmpfs` (item 23) take pages from the same counter, and the charge belongs to the object
        and not to the region, so that a `fork` does not count shared pages twice. The hidden file of item 24 takes its whole size at
        creation, and a named `tmpfs` file takes one page at a time.
      - Tests: `ktest/test.commit.c`.

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

## Next steps

1. `GETRANDOM` (21) and the rest.

## Decisions made while implementing

- `PROT_NONE`: a user page without access clears `PRESENT` in its entry and sets a software bit (`AMD64_PML_PROTNONE`). The entry keeps its
  frame. `pmentry_valid` counts it as mapped, so `rmap`, `fork`, `munmap` and `mprotect` need no change. An access faults and ends in
  `SIGSEGV` with `SEGV_ACCERR`. This is what Linux does, because an x86 page that is present is always readable.
- Shared futex: a shared key is the physical address of the word. A shared waiter keeps the frame pinned while it sleeps. The wake at
  thread exit uses both kinds of key. Only a `MAP_SHARED` mapping gives a stable frame, and `usr/vex/futex.h` says so.
- `MAP_POPULATE` faults the pages in with a read, one page at a time, and reports no error.
- `SETHOSTNAME` takes the lengths up to 255, not 64 as Linux does, because the `nodename` field has 256 bytes.
- Commit accounting: private writable memory counts, and the account follows the region (item 31). A mapping that replaces others
  gets a credit for the pages that it replaces. `MAP_NORESERVE` takes `CAP_VM_NORESERVE` and a `flags` argument of `vms_map*`.
- `tmpfs` pins every node it makes: `create` calls `vcache_insert_pinned` itself, sets the parent of a directory, and the VFS keeps what the
  filesystem gave it. The filesystem owns one reference for as long as a node has a name, and `unmount` drops them.
- `parse_size_iec` in `libk/hrsize.c` reads `16k`, `4M`, `1GiB` and the like, for mount options and the command line.
- A system call handler is a plain function whose parameters are the registers that the call reads (`proc/syscall_internal.h`).
  `define_syscall(fn, number, count)` counts the registers. `define_syscall_fd` hands the handler a `file_t*` for the descriptor, and
  `define_syscall_dirfd` hands it the `int` and the path copied from user space. A new kind of macro is only for what the dispatcher must
  do around the handler.
- Addresses have types (`vex/types.h`, `vex/address.h`). `user_addr_t` is a transparent union, so that a wrapper passes it a register, and a
  handler cannot use it as a number or a pointer. `virt_addr_t` and `phys_addr_t` are structs that take no plain integer. A null physical
  address still means no frame. The user ABI has `__k_uintptr_t`, `__k_intptr_t` and `__k_size_t`; the width of a pointer comes from
  `arch/types.h`.
- CONTRIBUTING has two style rules from this work: no call in the middle of the arguments of another call, and a condition of three
  operands at most.
- Access to a file is checked in `open` only. A descriptor is a right that was granted, so `read` does not look at credentials again. A
  check on every access would belong to a security module, as `security_file_permission` in Linux.

- `DUP`, `DUP2` and `DUP3` are one call, `SYS_DUP(old, new, flags)` (`0xC6`), with the rules of `dup3`: the same number twice is `EINVAL`.
  The C library makes `dup` with `fcntl(F_DUPFD, 0)`, and `dup2` with `SYS_DUP` and no flags; for the same number it checks the
  descriptor with `fcntl(F_GETFD)` and returns it.
- A subsystem of the kernel that is a service with files of its own gets a header under its name and keeps its filesystem to itself, as
  `shmem` does with `tmpfs`: no filesystem has a public header. The pipe service does so with `pipefs`.

- `PPOLL` has no waiter lists. An event counts its triggers (a generation). A file's `poll` operation names its events with
  `poll_table_add`, which reads the generation before the file reads its state. The wait (`event_watch_*`) registers on one event at a
  time and returns at once if a generation moved. It never holds the locks of two events, so two polls that list the same files in
  another order cannot deadlock, as they could with `event_await_unlock` on several events.
- The `pollfd` entries of `PPOLL` live in chunks: 8 on the stack, then one page from `pslab` for each further chunk. No `kmalloc`, except
  that `thread->events` grows once to the largest number of events that the thread has waited on.

### Follow-up for mlibc (a session of its own)

- The new `MAP_*` flags in `abi-bits/vm-flags.h`.
- `Sysdeps<Seek>` already stores the result of the call, which is the new offset now.
- The `Readv` and `Writev` sysdeps, and `Pread` and `Pwrite`, and `Sleep` as a relative `CLOCK_MONOTONIC` call of `SYS_NANOSLEEP`.
- The `SetHostname` sysdep.
- The `Pipe` sysdep (`SYS_PIPE` takes the flags), and `Dup` and `Dup2` as the item above says.
- `poll`, `ppoll`, `select` and `pselect` on `SYS_PPOLL`, and `abi-bits/poll.h` with the values of `usr/vex/poll.h` (the Linux ones).
- Everything in Tier 1: signals, threads, `Sysconf`.
- `shm_open` and `sem_open` use `/run/shm/` (`options/posix/generic/sys-mman.cpp`), and `/run` is a `tmpfs` mount.
- `SYS_WAITPID` takes a fourth argument and `SYS_WAITID` a fifth: a `struct __k_rusage` pointer or zero. `wait4` and `wait3` go on
  top of it, and every other wait passes zero.
- `getrusage` on `SYS_GETRUSAGE`, `times` on `getrusage` and `CLOCK_MONOTONIC`, and `clock_gettime` with the CPU time clocks.
- `fcntl` with `F_GETLK`, `F_SETLK`, `F_SETLKW` and the `F_OFD_*` commands (our numbers, 6 to 11), `struct __k_flock`, and `flock` on
  `SYS_FLOCK` with `LOCK_*` from `usr/vex/fcntl.h`.
- `mkfifo`, `mknod` and `mknodat` on `SYS_MKNODAT` (`dirfd, path, mode, dev`).
- `struct iovec` has a `__k_uintptr_t` base and a `__k_size_t` length; the layout on amd64 is the same.

## Dependencies

- Item 31 goes after item 13 and before item 23. It needs no other item.

- Item 4 needs item 1. Item 5 needs item 20 for the CPU count and the memory size.
- Items 8 to 14, 15, 16 need item 7 (renumbering) when they add a number.
- Item 12 gets its limit check from item 22.
- Item 14 needs item 28 for the CPU time clocks.
- Item 24 needs item 23. Item 29 needs items 16 and 23.
- Item 26 needs item 16 (pipe events) and item 15 (mask swap).
- Item 25 does not depend on pipes. It touches `waitpid`, the signal code, the return to user mode and the tty.

## Priority overrides

The tiers are sorted by cost. These items are done earlier than their tier says:

- Item 25 (job control) goes right after Tier 2, before items 15 (`sigsuspend`) and 16 (`pipe`). It does not depend on them. It does
  depend on the signal sysdeps (item 3) and on the renumbering (item 7), because a shell test needs both.

- Item 31 goes right after item 13 and before item 23 (`tmpfs`), not at the end of Tier 4.

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
- [x] Item 23: how `tmpfs` stores file pages: a memory mode in `fcache` (not a separate page store inside `tmpfs`). Decided (delegated).
- [x] Item 25: job control moves before `pipe` and `sigsuspend`. See "Priority overrides". Decided.
- [x] Item 8: `pwrite` with `O_APPEND` follows POSIX and writes at the given offset. Decided.
- [x] Item 8: a flag in `file_t` for streams, set at open, and a char device with no `seek` is a stream. Decided.
- [x] Item 9: `CLOCK_NANOSLEEP` goes to the system block, and an absolute `CLOCK_REALTIME` wait follows `settime`. Decided.
- [x] Item 31: strict accounting at the region level. `fork` may return `ENOMEM`. `PROT_NONE` is not counted and `mprotect` counts.
  `MAP_NORESERVE` works only for a process with a capability and is ignored for the others. `SIGBUS` when a fault cannot get a page.
  No OOM killer for now. Decided.
- [x] Item 9: one shared time helper for futex, sleep and the new call. Decided.
- [x] Item 31: the limit is the `pslab` pages minus `max(1/8, 4096 pages)`. Decided.
- [x] Item 31: shared anonymous memory and `tmpfs` take pages from the one counter. The hidden file takes its whole size at creation, and
  a named file takes a page when it allocates one. Decided.
- [x] Item 31: a mapping that replaces others checks the difference first, with a credit. Decided.
- [x] Item 31: the `MAP_NORESERVE` flag goes in a `flags` argument of `vms_map*`, not in `vm_backing_t`. The capability is
  `CAP_VM_NORESERVE`. Decided.

## Not verified

- The exact size of `process_t` (about 240 bytes by hand count) and the exact size of its mutex field.
- The number that `RLIMIT_NOFILE` should return. `process_t` has `max_fds`, but the limit constant was not read.
- `sys_sigreturn`: where it expects the stack pointer, for the restorer in item 3.
- How the active clock source stores its frequency, for item 14.
- Whether `mlibc` `clock()` uses `CLOCK_PROCESS_CPUTIME_ID`, and what mlibc expects from robust mutexes (item 1).
- Whether `vms` has a helper to fault a range in, for `MAP_POPULATE`.
- Whether the tty discipline is independent of one driver (pty).
- Nothing in this plan was built or run.
