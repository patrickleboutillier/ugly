# The Java plugin

Notes for Claude Code, loaded when working in this directory. Shared rules: `plugins/CLAUDE.md` and the root `CLAUDE.md`.

**`java/`** is Java through JNI (JDK 21+, found via `javac`; `-ljvm` with an rpath to `lib/server`).

- One JVM per process, created in `init` (or the running one reused), with `-Xrs` and the class path `<plugin dir>/ugly.jar:$UGLY_JAVA_CLASSPATH:$CLASSPATH`. Only `UGLY_CAP_ATTACH`.
- Most of the protocol is Java, in `ugly/Bridge.java`: reflection, bean getters, `Map` keys, arrays and `List`s, and overloads chosen by `score()`. A foreign value passed for an interface becomes a dynamic proxy (`ForeignHandler`).
- `ugly.Foreign` holds a handle, released by a `Cleaner` and cached per handle (the Cleaner's action drops the cache entry too, `forget`); its natives are registered from `java.c`. Errors from elsewhere are `ugly.ForeignException`, which owns the core's error until it leaves Java.
- `coerce()` checks what it narrows itself (fields, array elements and interface results reach it without `score()`). `Bridge.call` takes the hint for a callable that isn't overloaded, and refuses one that isn't its method's signature: a hint left in `HINT` is the next resolution's.
- Refs are global refs; identity is an id from `Bridge.acquireId` (an `IdentityHashMap`), since objects move. Strings are converted from UTF-16 in C, never via modified UTF-8.
- Every op runs in `FRAME_IN`/`FRAME_OUT`, and every JNI call that can throw is followed by `JCHECK` or `drop_exception`. Keep the call path lean (perf-profiled): no `FindClass` per call (it walks the stack), and results are classified by one call to `Bridge.kind` (keep it in step with `to_val`'s `K_` enum). `Bridge` caches overload resolution (`Overloads`, `Sig`).
- JVM output goes to stderr (the `vfprintf` hook). `UGLY_JAVA_CHECK` adds `-Xcheck:jni` and aborts on a JNI warning at `FRAME_OUT`; the tests set it. Its signal checker prints "handler modified" noise at `exit()`, harmlessly, now on stderr.
- Java tests are in suite `jvm`, which the valgrind setup excludes. The fixture is `tests/java/conformance.java`, built into `conformance.jar`.
- The binding (`binding/`, `libugly_jni.so`, no libjvm) is loaded by `ugly.Foreign`'s static initializer unless `-Dugly.embedded=true` (set by the plugin when it creates the JVM). `JNI_OnLoad` registers and attaches on the loading thread; teardown is at JVMTI VMDeath, which runs on `main`'s thread (shutdown hooks run on new threads, which own no ctx). Its test runs `java` with `-Xcheck:jni -XX:+DisplayVMOutputToStderr`, and its last TAP line comes from a Lua finalizer at teardown.
