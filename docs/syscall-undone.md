# Open UNDONEs of the syscall plan

The kernel work items of `syscall-plan.md` are all struck through. These are the `UNDONE:` notes they left behind,
ordered as the plan numbers them. Line numbers point at `syscall-plan.md`. Finish these before the mlibc patching
session, or decide to leave them.

1. **`file_seek` ignores `FILE_SEEKABLE`** (item 8, line 133). It asks the node on every call and gets the same
   answer. Cache the answer from the flag that `file_open_node` sets once.
2. **`MADV_WILLNEED` is a no-op** (item 12, line 175). Add read ahead. Low value before real workloads; can wait.
3. **Scheduler does not preempt on wakeup** (item 13, line 188). A thread that wakes with a better nice value waits
   for the running one to finish its timeslice. The run queues are one FIFO per hart.
4. **Affinity with every allowed hart busy** (item 13, line 206). The thread waits for a hart to free up instead of
   being queued on one that may run it. The same gap as the general queue-balancing `TODO`.
5. **`RLIMIT_MEMLOCK` and `CAP_IPC_LOCK`** (items 12 and 22, lines 179 and 352). `mlock` has no capability and no
   limit check. Item 22 did the rlimit store and `CAP_SYS_RESOURCE`; this limit and capability are still missing.
6. **tmpfs symlink targets are not counted** (item 23, line 374). A target is a slab object or a `pslab` page, and
   neither the `nodes=`/`size=` mount limits nor the commit limit see it.
7. **No `tmpfile` vnode operation** (item 24, line 381). `shmem` makes a hidden file with create and unlink. A
   dedicated operation would save the round trip. Cosmetic; can wait.
8. **No `ptrace` stops, no `CLD_TRAPPED`** (item 25, line 417). Job control is complete for signals; tracer stops
   belong to a future `ptrace` design.
9. **Device nodes cannot find their interface by `rdev`** (item 29, line 483). A node outside devfs opens with
   `ENXIO`. Needs an `rdev` lookup and a `nodev` mount option. Nothing needs it yet.
10. **No commit reserve for `CAP_VM_NORESERVE`** (item 31, lines 541 and 564). The capability exists (number 50) and
    `MAP_NORESERVE` works. Missing: a small reserved part of the commit limit that only holders of the capability
    may use.

Not listed: the "Still to do" lines of the plan are all mlibc-side sysdeps, which the mlibc session covers.
