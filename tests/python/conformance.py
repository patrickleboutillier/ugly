"""The conformance fixtures (doc/contract.md section 9), in Python."""

import builtins
import gc
import itertools

import ugly

_finalized = 0


class list(builtins.list):
    """A counter's items: a list whose finalization is counted."""

    def __del__(self):
        global _finalized
        _finalized += 1


class counter:
    def __init__(self):
        self.count = 0
        self.items = list()

    def inc(self):
        self.count += 1
        self.items.append(self.count)
        return self.count

    def __del__(self):
        global _finalized
        _finalized += 1

    def __str__(self):
        return f"counter({self.count})"


class ConformanceError(Exception):
    def __init__(self, message):
        super().__init__(message)
        self.message = message


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm2(self):
        return self.x * self.x + self.y * self.y


def echo(x):
    return x


def kind_of(x):
    if x is None:
        return "null"
    if isinstance(x, bool):
        return "bool"
    if isinstance(x, int):
        return "int"
    if isinstance(x, float):
        return "double"
    if isinstance(x, str):
        return "string"
    if isinstance(x, (bytes, bytearray)):
        return "bytes"
    if isinstance(x, ugly.Proxy):
        return "proxy"
    return type(x).__name__


def make_counter():
    return counter()


# Benchmarks (tests/bench.c): f(i) n times, and our own echo n times.
def bench(f, n):
    for i in range(n):
        f(i)
    return n


def bench_self(n):
    for i in range(n):
        echo(i)
    return n


def apply(f, x):
    return f(x)


def call_twice(f, x):
    return f(f(x))


def raise_error(msg):
    raise ConformanceError(msg)


def catch_and_return(f):
    """Returns the exception object: ours as is, a foreign one through its payload."""
    try:
        return f()
    except ugly.Error as e:
        return e.payload
    except BaseException as e:
        return e


def rethrow(e):
    ugly.rethrow(e)


def finalized():
    gc.collect()
    return _finalized


# Call options (contract §11): keyword arguments.
def kw(a, b=0, c=0):
    return f"{a} {b} {c}"


class KwBox:
    def __init__(self, value=0):
        self.value = value

    def add(self, n=1):
        return self.value + n


# Iteration (contract §2): generators, run only as far as asked.
_produced = 0


def count_to(n):
    global _produced
    _produced = 0
    for i in range(n):
        _produced += 1
        yield i


def naturals():
    global _finalized
    try:
        yield from itertools.count()
    finally:     # closed when released early
        _finalized += 1


def fail_after(n):
    yield from range(n)
    raise ConformanceError(f"fail_after({n})")


def produced():
    return _produced


def sum_iter(xs):
    total = 0
    for x in xs:
        total += x
    return total


def take_iter(xs, n):
    return builtins.list(itertools.islice(xs, n))


def stringify(x):
    return str(x)
