# Rye

Rye is a procedural, statically-typed programming language designed for simplicity and usability.

## Building

Requirements:

- **g++-14** (or another C++23-capable compiler with `<expected>` and `<print>`)
- **CMake** 3.28+
- **clang** (assembles and links generated `.S` files)

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## Running tests

```bash
python3 run_tests.py
```

Each test file starts with `/// <exit_code>` — the expected process exit code.

## Compiling a program

```bash
./build/rye myprogram.rye -O myprogram
./myprogram
```

The compiler always links `runtime/ryert.rye`, which provides platform entry-point glue.

## Language overview

```rye
proc add(a int, b int) int {
  return a + b;
}

proc main() int {
  var x int = 10;
  if x == 10 {
    return add(x, 32);
  }
  return 0;
}

proc sum_to_n(n int) int {
  var total int = 0;
  for var i int = 1; i <= n; i = i + 1 {
    total = total + i;
  }
  return total;
}
```

Loop forms:

```rye
while condition {
  // body
}

for init_stmt; condition; step_stmt {
  // body
}
```

Supported today:

- Types: `void`, `bool`, `int`, pointers (`*int`, `**int`, …)
- Declarations: `proc`, `const`, `extern proc`, compile-time `when`
- Statements: `var`, `name = expr` assignment, `return`, `if` / `else`, `while`, `for`, `break`, `continue`, blocks `{ ... }`
- Expressions: arithmetic, comparisons, logical operators (`&&`, `||`, `!`), pointers (`&`, `*`), calls

`break` exits the nearest enclosing `while` or `for` loop. `continue` skips to the next iteration — the loop condition for `while`, or the step expression for `for`.

Logical `&&` and `||` short-circuit: the right-hand side is evaluated only when needed. `!` negates a `bool` value.

### Arena allocation

The parser allocates AST nodes from a per-file bump arena (`src/arena.hpp`) instead of scattering `new` across the parse tree. Each source file gets its own arena in `main.cpp`, which keeps allocation fast and localized.

## Runtime and linking

Rye compiles to native assembly for the **host platform** (Linux x86_64, Linux AArch64, or macOS AArch64) and links with `clang` against the system C library.

### Entry points

| Platform | Entry symbol | Notes |
|----------|--------------|-------|
| Linux | `main` | libc `_start` → `__libc_start_main` → your `main` |
| macOS | `_main` | Runtime wrapper in `runtime/ryert.rye` forwards to `main` |

You may define `main` with or without `argc` / `argv`:

```rye
proc main() int { return 0; }
// or
proc main(argc int, argv **int) int { return 0; }
```

### External symbols

Use `extern proc` to declare functions implemented outside Rye (typically provided by libc or other object files):

```rye
extern proc some_c_function(x int) int;
```

The compiler emits `.extern` declarations and relies on the linker to resolve them.

### Modules and imports

Each `.rye` file is a **module** named after its file stem (`foo.rye` → module `foo`). Import other modules with a string path:

```rye
import "lib/math.rye";

proc main() int {
  return math:add(2, 40);
}
```

Paths are resolved relative to the importing file's directory, then along each `-I` search path:

```bash
./build/rye app.rye -I vendor -O app
```

The compiler follows imports transitively, type-checks modules in dependency order, and emits mangled linker symbols (`math_add`, `foo_bar`, …). Entry-point `main` and `runtime/ryert.rye` symbols are not mangled.

Imported procedures and constants are available both qualified and unqualified:

```rye
import "lib/math.rye";

proc main() int {
  return add(1, 2) + math:TEN;  // unqualified proc, qualified const
}
```

Use `module:proc(args)` or `module:CONST` when you need an explicit namespace, or when the same name is exported from multiple imports.

### Multi-file compilation

Pass one or more entry `.rye` files on the command line. Imported modules are discovered automatically; each file compiles to an object under `.rye/`, then everything links together:

```bash
./build/rye myprogram.rye -O myprogram
```

`runtime/ryert.rye` is always included automatically.

### Freestanding / nostdlib

Not supported yet. All programs currently link against libc via `clang` without `-nostdlib`.
