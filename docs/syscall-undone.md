# Open UNDONEs of the syscall plan

The kernel work items of `syscall-plan.md` are all struck through. These are the `UNDONE:` notes they left behind,
ordered as the plan numbers them. Line numbers point at `syscall-plan.md`. Finish these before the mlibc patching
session, or decide to leave them.

1. **`MADV_WILLNEED` is a no-op** (item 12, line 175). Add read ahead. Low value before real workloads; can wait.
2. **Scheduler does not preempt on wakeup** (item 13, line 188). A thread that wakes with a better nice value waits
   for the running one to finish its timeslice. The run queues are one FIFO per hart.
3. **Affinity with every allowed hart busy** (item 13, line 206). The thread waits for a hart to free up instead of
   being queued on one that may run it. The same gap as the general queue-balancing `TODO`.
4. **No `ptrace` stops, no `CLD_TRAPPED`** (item 25, line 417). Job control is complete for signals; tracer stops
   belong to a future `ptrace` design.
5. **Device nodes cannot find their interface by `rdev`** (item 29, line 483). A node outside devfs opens with
   `ENXIO`. Needs an `rdev` lookup and a `nodev` mount option. Nothing needs it yet.

Not listed: the "Still to do" lines of the plan are all mlibc-side sysdeps, which the mlibc session covers.
