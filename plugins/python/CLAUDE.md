# The Python plugin

Notes for Claude Code, loaded when working in this directory. Shared rules: `plugins/CLAUDE.md` and the root `CLAUDE.md`.

**`python/`** is CPython 3.10+.

- Python is initialized once at plugin load with `Py_InitializeEx(0)`, the GIL is released, and Python is never finalized. So there's no RESTARTABLE and no MULTI_RUNTIME, and a runtime is a globals dict.
- Every entry does `PyGILState_Ensure`/`Release`. `OUT()` releases the GIL around its whole argument: no Python API call inside it.
- The embedded `ugly` module holds `ugly.Proxy` (cached per handle via weakrefs), `ugly.Error` (`.type`, `.message`, `.language`, `.payload`, and an `_err` capsule that's restored unchanged when it escapes) and `ugly.rethrow`. It is the seed of the Python binding.
- Conversions are Python's strict ones.
- A foreign sequence as a list (`seq_*`): negative indexes and slices take its length first; `del p[i]`, slice assignment and deletion are one `ugly_splice` (`seq_splice`); `append`, `extend`, `insert`, `pop`, `clear` are `list_method`, given by `proxy_getattro` only when the value has no member of that name. `del p.k` and `del p["k"]` are `ugly_delete_member`.
- The Python we started is never finalized, so `flush_output` flushes `sys.stdout` and `sys.stderr` in `py_rt_delete` and at exit (`atexit`). `py_failure` leaves the traceback on the exception, and `raise_ugly` raises one that comes home with it (`PyErr_Restore`). Error messages are passed whole, not through a buffer.
- `proxy.m`: a callable member is a bound method (`ugly_invoke`); one that is INSTANTIABLE too is a proxy of the class that remembers its owner (`member_proxy`, not the cached one), called through `ugly_invoke` on the owner. `nb_bool`: false only when there is no element 0 (`ugly_get_index`, never the length: a sequence may be infinite). `proxy_traits` caches only a describe that worked.
- Fork: `pthread_atfork` handlers (`at_fork_*`, registered once by `fork_safety` in `py_init`) bracket every C-level fork like `os.fork()`: GIL + `PyOS_BeforeFork`, then `PyOS_AfterFork_Parent/Child` + release. An `os.register_at_fork` callback sets the thread-local `python_forking`, so `os.fork()` isn't bracketed twice. Test `python-fork` (a busy Python thread while forking; children report through a pipe and end with SIGKILL, for valgrind).
