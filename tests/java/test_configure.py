"""ugly.configure for Java, from a plain python3 process (TAP): the class path
and JVM options are set before the JVM starts. UGLY_JAVA_CLASSPATH is empty
here, so the fixture jar is found only through the "classpath" option."""

import os
import sys

count = 0
failures = 0


def ok(cond, what):
    global count, failures
    count += 1
    if not cond:
        failures += 1
    print(("ok" if cond else "not ok"), count, "-", what, flush=True)


import ugly

ugly.configure("java", classpath=os.environ["UGLY_TEST_JAR"], options=["-Dugly.configured=yes", "-Xmx64m"])

System = ugly.module("java", "java.lang.System")
ok(System.getProperty("ugly.configured") == "yes", "options: -Dugly.configured=yes reaches the JVM")
heap = ugly.module("java", "java.lang.Runtime").getRuntime().maxMemory()
ok(heap <= 64 * 1024 * 1024, f"options: -Xmx64m limits the heap ({heap} bytes)")
conformance = ugly.module("java", "conformance")
ok(conformance.echo("x") == "x", "classpath: a class from the configured jar")
try:
    ugly.configure("java", options="-Xmx1g")
    ok(False, "configure once the JVM runs raises ugly.Error")
except ugly.Error as e:
    ok("already started" in e.message, "configure once the JVM runs raises ugly.Error")

print(f"1..{count}")
sys.exit(1 if failures else 0)
