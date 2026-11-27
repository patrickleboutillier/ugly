// The conformance fixtures (doc/contract.md section 9), in JavaScript.
//
// Type names the suite checks (describe) are constructor names: counter,
// list. A module namespace is "module". Exceptions are ConformanceError.

import * as ugly from "ugly";

// Objects whose finalization the suite counts: counters and their item lists.
// QuickJS queues FinalizationRegistry callbacks only when its GC runs, so
// finalized() collects first (ugly.gc() also runs the queued callbacks).
let finalized_count = 0;
const registry = new FinalizationRegistry(() => { finalized_count++; });

export class list extends Array {}

export class counter {
  constructor() {
    this.count = 0;
    this.items = new list();
    registry.register(this, null);
    registry.register(this.items, null);
  }
  inc() {
    this.count++;
    this.items.push(this.count);
    return this.count;
  }
  toString() { return `counter(${this.count})`; }
}

export class ConformanceError extends Error {
  constructor(message) {
    super(message);
    this.name = "ConformanceError";
  }
}

export class Point {
  constructor(x, y) { this.x = x; this.y = y; }
  norm2() { return this.x * this.x + this.y * this.y; }
}

export function echo(x) { return x; }

export function kind_of(x) {
  if (x === null || x === undefined) return "null";
  if (typeof x === "boolean") return "bool";
  if (typeof x === "number") return Number.isInteger(x) ? "int" : "double";
  if (typeof x === "bigint") return "int";
  if (typeof x === "string") return "string";
  if (x instanceof ArrayBuffer) return "bytes";
  if (ugly.isProxy(x)) return "proxy";
  return x.constructor?.name ?? typeof x;
}

export function make_counter() { return new counter(); }
// Benchmarks (tests/bench.c): f(i) n times, and our own echo n times.
export function bench(f, n) { for (let i = 0; i < n; i++) f(i); return n; }
export function bench_self(n) { for (let i = 0; i < n; i++) echo(i); return n; }
export function apply(f, x) { return f(x); }
export function call_twice(f, x) { return f(f(x)); }
export function raise_error(msg) { throw new ConformanceError(msg); }

// Returns the exception object: ours as is, a foreign one through its payload.
export function catch_and_return(f) {
  try {
    return f();
  } catch (e) {
    return ugly.isError(e) ? e.payload : e;
  }
}

export function rethrow(e) { throw e; }

export function finalized() {
  ugly.gc();
  return finalized_count;
}

// Iteration (contract §2): generators, run only as far as asked.
let produced_count = 0;

export function count_to(n) {
  produced_count = 0;
  return (function* () {
    for (let i = 0; i < n; i++) {
      produced_count++;
      yield i;
    }
  })();
}

export function* naturals() {
  try {
    for (let i = 0; ; i++) yield i;
  } finally {     // closed when released early: return()
    finalized_count++;
  }
}

export function* fail_after(n) {
  for (let i = 0; i < n; i++) yield i;
  throw new ConformanceError(`fail_after(${n})`);
}

export function produced() { return produced_count; }

export function sum_iter(xs) {
  let total = 0;
  for (const x of xs) total += x;
  return total;
}

export function take_iter(xs, n) {
  const out = [];
  if (n <= 0) return out;
  for (const x of xs) {
    out.push(x);
    if (out.length >= n) break;
  }
  return out;
}

export function stringify(x) { return String(x); }
