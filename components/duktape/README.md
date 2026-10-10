# Duktape 2.7.0

Embeddable ECMAScript engine, MIT licensed (see `LICENSE.txt`), from
<https://github.com/svaarala/duktape/releases/download/v2.7.0/duktape-2.7.0.tar.xz>: `src/` is the
dist's `src/` directory.

`src/` carries the changes in `duktape.patch` (`patch -p1` against the dist; its `configure.py`
needs Python 2):

- `DUK_USE_INTERRUPT_COUNTER` + `DUK_USE_EXEC_TIMEOUT_CHECK` → `script_exec_timeout(udata)`, so a
  runaway script is aborted instead of stalling the bridge.
- `DUK_USE_NATIVE_STACK_CHECK()` → `script_native_stack_check()`: deep recursion throws a
  RangeError before the engine task's C stack runs out.
- `duk__comp_recursion_increase()` in `duktape.c` also calls the native stack check: the compiler
  only had a depth limit (2500), far deeper than the task stack allows.
- `DUK_USE_MARK_AND_SWEEP_RECLIMIT` 256 → 32: the GC's mark recursion isn't covered by the stack
  check; past the limit it falls back to a slower, non-recursive pass.
- `DUK_USE_FASTINT`: integer arithmetic without soft-float doubles (the ESP32-S3 FPU is
  single-precision only).
