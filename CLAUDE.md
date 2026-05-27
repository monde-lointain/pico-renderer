## Orthodox C++

When writing C++, adhere to **Orthodox C++** (a.k.a. C+): a minimal, C-like subset that rejects most Modern C++. Goal: code readable by a C programmer, fast to compile, portable, no surprises. Reference: https://bkaradzic.github.io/posts/orthodoxc++/.

Heuristic: a feature from C++_year_ is acceptable around year (_year_ + 5). Today: cautious C++17, very selective C++20.

### Global

- Use C headers (`<stdio.h>`, `<string.h>`, `<math.h>`), not C++ wrappers (`<cstdio>`, `<cstring>`, `<cmath>`).
- Use `printf`/`fprintf`, not `<iostream>`/`<sstream>`.
- Return error codes, not exceptions. No RTTI.
- Avoid STL allocating containers unless allocation is irrelevant. Prefer fixed buffers, arenas, custom allocators.
- Avoid heavy template metaprogramming; templates only when they materially simplify.
- Avoid modules, concepts, coroutines, and other bleeding-edge features.

### Rule examples

Each pair: GOOD on the left intent, BAD on the right.

#### struct, not class

```cpp
// GOOD
struct Point { int x; int y; };

// BAD
class Point { int x; int y; };
```

#### POD only — no ctors/dtors/inheritance/virtuals/access specifiers

```cpp
// GOOD
struct Buffer { char *data; size_t size; };
void buffer_init(struct Buffer *b, size_t n);
void buffer_free(struct Buffer *b);

// BAD
struct Buffer {
    Buffer(size_t n);          // constructor
    ~Buffer();                 // destructor
    virtual void resize(...);  // virtual
private:                       // access specifier
    char *data;
};
struct Sub : Buffer {};        // inheritance
```

#### Pointers, not references

```cpp
// GOOD
void scale(Point *p, int k);
int  read_only(const Point *p);

// BAD
void scale(Point &p, int k);
int  read_only(const Point &p);
void take(Point &&p);             // rvalue ref
```

> Note: this rule is about pointers-vs-references (semantics), not `*` whitespace.
> Pointer alignment is whatever `.clang-format` produces — this project uses plain
> `BasedOnStyle: Google` (left-aligned, e.g. `Point* p`). Don't hand-fight it; both
> `Point *p` and `Point* p` are acceptable, and `make format` is the source of truth.

#### C-style casts, not named casts

```cpp
// GOOD
B *b = (B *)a;

// BAD
B *b = static_cast<B *>(a);
B *b = reinterpret_cast<B *>(a);
B *b = dynamic_cast<B *>(a);     // also: no RTTI
A *a = const_cast<A *>(ca);
```

#### Explicit types, not `auto`

```cpp
// GOOD
int      y = x;
Point  *pp = make_point();
int  fn(int x) { return x; }

// BAD
auto y = x;
auto pp = make_point();
auto fn(int x) { return x; }
auto fn(int x) -> int { return x; }
```

#### `malloc`/`free`, not `new`/`delete`

```cpp
// GOOD
int *x = (int *)malloc(100 * sizeof *x);
free(x);

// BAD
int *x = new int[100];
delete[] x;
```

#### Index loop, not range-based for

```cpp
// GOOD
for (size_t i = 0; i < n; ++i) putchar(s[i]);

// BAD
for (char c : s) putchar(c);
```

#### No lambdas

```cpp
// GOOD
static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}
qsort(arr, n, sizeof *arr, cmp_int);

// BAD
std::sort(arr, arr + n, [](int a, int b) { return a < b; });
```

#### No exceptions

```cpp
// GOOD
int parse(const char *s, int *out);  // returns 0 on success, errno-like on failure
if (parse(s, &v) != 0) { /* handle */ }

// BAD
int parse(const char *s);            // throws on failure
try { v = parse(s); } catch (...) { /* handle */ }
```

#### No function/operator overloading, no default arguments

```cpp
// GOOD
void draw_point(Point p);
void draw_line(Point a, Point b);
int  add_i(int a, int b);
float add_f(float a, float b);
int  scale(int x, int k);            // caller passes k explicitly

// BAD
void draw(Point);
void draw(Point, Point);             // function overload
int  operator+(MyT, MyT);            // operator overload
int  scale(int x, int k = 1);        // default argument
```

#### No user-defined conversions / literals

```cpp
// BAD
struct Money { operator double() const; };          // conversion overload
size_t operator""_kb(unsigned long long n);         // user-defined literal
```

#### Explicit `this->` and `Class::` qualifiers

```cpp
// GOOD (when methods are unavoidable)
int Widget::value() const { return this->x; }
int n = Widget::kCount;

// BAD
int Widget::value() const { return x; }              // implicit this
int n = kCount;                                      // implicit static qualifier
```

#### Plain `enum`, not `enum class`

```cpp
// GOOD
enum Color { COLOR_RED, COLOR_GREEN };

// BAD
enum class Color { Red, Green };
```

#### Avoid namespaces (keep shallow if used at all)

```cpp
// GOOD
int audio_buffer_size(void);

// BAD
namespace audio { namespace buffer { int size(); } }
```

#### No `mutable`

```cpp
// BAD
struct Cache { mutable int hits; };
```

### Pragmatism

When interfacing with an API that forces non-Orthodox features (e.g. Clang/LLVM, Qt), match the API's idioms locally; keep the violation as narrow as possible. Match surrounding style in any existing file.

### Suppression

In codebases enforced by an Orthodox C++ tool, suppress a single line with a trailing comment naming the rule, e.g.:

```cpp
static_cast<int>(x);  /* HERESY(static-cast) */
```

# Refactoring (Orthodox C++)

When refactoring (e.g. acting on code-smell findings), pursue the
*intent* of good design — single responsibility, low coupling, encapsulation,
no duplication — realized with C idioms, never with classes, inheritance,
templates, or value-objects-with-methods. Testability is the design signal:
hard-to-test code signals a design problem, not a need for more mocking.

- **The module is the unit of design, not the class.** A module = a POD struct
  plus `module_verb(Receiver* r, ...)` functions over it, split header
  (interface) / `.cc` (implementation). This is where cohesion lives; "extract
  a class" means *extract a module*.
- **Encapsulate state, don't expose it.** Single-instance state: hide as
  file-scope `static` in the `.cc`, reach it only through module functions —
  that is encapsulation, not a global hazard. Multiple instances that must hide
  layout: forward-declare the struct in the header and define it in the `.cc`
  (opaque pointer / ADT).
- **Group data that travels together** (co-occurring parameters, primitives
  standing in for a domain concept like a coordinate) into a named POD struct.
  It stays POD — behavior goes in free `module_verb` functions, never ctors,
  operators, or conversions.
- **Invert dependencies onto interfaces, not details.** Keep high-level/game
  logic free of platform and hardware calls (the `App`/`plat_*` split already
  does this). Substitute implementations at link time, or — when runtime
  selection is needed — through an explicit function-pointer interface.
- **Replace type-switching with a function-pointer interface.** Repeated
  `switch` on a type tag (shotgun surgery when behavior changes) becomes one
  interface of function pointers; callers dispatch through it. "Inheritance" is
  struct composition: embed the base struct as the first member and cast.
- **Move a function to the data it envies.** If a function works mostly on
  another module's data, move it into that module and rename to fit. Say each
  thing once; kill duplication at the root.
- **Refactor behind tests, behavior-preserving.** Copy → compile the new path →
  switch callers → delete the old (don't break a working path mid-refactor).
  Leave each file a little cleaner than you found it.
- **Reconcile generic smell heuristics with this style.** A module's file-scope
  state behind accessors is encapsulation, not "global data." An explicit
  receiver pointer plus a few parameters is not automatically a long-parameter
  smell. Intention-revealing and section comments are fine. Apply the smell's
  intent; reject any fix that reaches for a class, template, or virtual.

# Modern CMake Best Practices

Follow these when writing or editing CMake.

## Core principles
- **Think in targets, not variables or directories.** Attach requirements to targets; let `PRIVATE`/`PUBLIC`/`INTERFACE` propagate them.
- **Always build out-of-source;** never commit generated build files.
- **Never assume you're the top-level project** — it may be embedded via `FetchContent`/`add_subdirectory()`.
- **Never hard-code developer/CI choices** (build type, `-Werror`, flags, linker, sanitizers, architectures); leave them to cache vars, toolchain files, or presets.
- **Require a recent CMake** (`cmake_minimum_required()` first line; prefer ≥3.21, never below 3.1).

## Targets (the heart of it)
- `PRIVATE` = build requirement (this target only); `INTERFACE` = usage requirement (consumers only); `PUBLIC` = both.
- **Start dependencies PRIVATE; promote to PUBLIC only when consumers truly need them.**
- Always use the keyword form of `target_link_libraries()`; **link to targets, not bare names/paths.**
- Use the uniform `target_*()` commands (`target_include_directories`, `target_compile_definitions/options/features`, `target_sources`, `target_link_options`) — not `include_directories()`, `add_definitions()`, `link_libraries()`, or `set_target_properties()`.
- `add_library()`/`add_executable()` without `STATIC`/`SHARED` (let `BUILD_SHARED_LIBS` decide). Prefer static over object libs; interface libs for header-only.
- Project-specific target names (`MyProj_Algo`) + a `MyProj::Algo` ALIAS for every public target; set `OUTPUT_NAME`/`EXPORT_NAME`. Don't pre/suffix names with `lib`.

```cmake
add_library(MyProj_Algo)
add_library(MyProj::Algo ALIAS MyProj_Algo)
target_sources(MyProj_Algo PRIVATE algo.cpp)
target_include_directories(MyProj_Algo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(MyProj_Algo PUBLIC fmt::fmt PRIVATE spdlog::spdlog)
target_compile_features(MyProj_Algo PUBLIC cxx_std_17)
```

## Configuration & flags
- Language standard via `CMAKE_CXX_STANDARD` + `_STANDARD_REQUIRED ON` + `_EXTENSIONS OFF` (set all three) or `target_compile_features(... cxx_std_17)`. Never set `-std=` directly.
- Don't modify `CMAKE_<LANG>_FLAGS` — it belongs to the developer. Use abstractions consistently (`CMAKE_COMPILE_WARNING_AS_ERROR`, `MSVC_RUNTIME_LIBRARY`, `SYSTEM` for dependency headers).
- Use generator expressions for per-config/per-target logic; **`$<CONFIG:Debug>` is the only robust per-config mechanism** (`CMAKE_BUILD_TYPE` breaks on multi-config generators). Use `$<TARGET_FILE:tgt>`, never the `LOCATION` property or hard-coded output paths.

## Paths, variables, structure
- Use `PROJECT_SOURCE_DIR`/`PROJECT_BINARY_DIR` and `CMAKE_CURRENT_LIST_DIR`, never `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR`.
- Prefer functions over macros; pass values as arguments. Append to `CMAKE_MODULE_PATH`, never replace. Don't `FORCE`/`INTERNAL` build-wide cache vars. Give cache options a `UPPERCASE_` project prefix.
- Top-level `CMakeLists.txt` = table of contents (preamble → setup → deps → targets → tests → packaging); only it calls `project()`, with name & `VERSION` set directly. Standard dirs: `cmake src tests doc packaging`.

## Dependencies, install, test
- Prefer `find_package()` in **config mode** (imported targets) over Find-modules/variables; `CMAKE_PREFIX_PATH` to locate. Use `FetchContent` for source builds (git `GIT_TAG` = commit hash); add `FIND_PACKAGE_ARGS` so deps can come from either.
- Make consumable: `install(TARGETS ... EXPORT)` + `install(EXPORT ... NAMESPACE MyProj::)`, `GNUInstallDirs`, relocatable installs, `configure_package_config_file()` + `write_basic_package_version_file()`, `find_dependency()` for deps. Provide identical targets whether found or `add_subdirectory()`'d.
- Tests: `enable_testing()`; `add_test(NAME ... COMMAND ...)`; unique project-prefixed names + `LABELS`; gate on `PROJECT_IS_TOP_LEVEL`; assert in code (not output regex); use a framework and fixtures over `DEPENDS`.

## Performance & anti-patterns
- Correctness before speed. Enable `CMAKE_OPTIMIZE_DEPENDENCIES`, ccache (`CMAKE_<LANG>_COMPILER_LAUNCHER`), and prefer the Ninja generator; treat unity builds/PCH as opt-in optimizations.
- **Avoid:** `file(GLOB)` for sources; `include_directories`/`add_definitions`/`link_libraries`; keyword-less `target_link_libraries`; `CMAKE_SOURCE_DIR` for internal paths; the `LOCATION` property; hard-coded `-std=`/`-Werror`/build type/linker; `set(name ...)` reused as project & target name; replacing `CMAKE_MODULE_PATH`; modifying `CMAKE_<LANG>_FLAGS`.
