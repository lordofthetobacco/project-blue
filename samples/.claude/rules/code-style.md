# C++ coding instructions for AI agents

Adapted from the **Google C++ Style Guide**, by Google:
<https://google.github.io/styleguide/cppguide.html>.
Read and reviewed in full on **2026-10-29**. This is a practical, condensed
adaptation of every topic in that page, not a verbatim copy or a C++ tutorial.
The source remains authoritative for details not captured here. Its language
baseline at review time is **C++20**, excluding C++23 features.

The source guide text is licensed under
[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/); this adapted prose
is provided under that license. Changes: reorganized into agent instructions,
condensed rationale, paraphrased decisions, and added an execution workflow
and review checklist. No Google endorsement is implied. Source code samples
in Google's guide have a separate Apache 2.0 license; the examples here are
new illustrative examples. See the project's
[licensing statement](https://google.github.io/styleguide/#licensing).

## 1. Agent contract and workflow

**MUST / MUST NOT** express requirements or prohibitions. **SHOULD / PREFER**
express defaults that allow a reasoned exception. **MAY** expresses permission.
Do not silently strengthen a preference into a prohibition, or weaken a ban.

The workflow in this section and the final checklist are operational additions
for agents; the language and style rules below are derived from the source.

Before changing code:

1. Read applicable repository instructions, build configuration, formatter
   configuration, and nearby interfaces and tests.
2. Establish the supported C++ version, platform constraints, dependencies,
   error model, and public API compatibility requirements.
3. Follow explicit task and repository requirements. Otherwise apply this
   document. In existing code with a different established style, preserve
   local consistency where needed; do not perform an unrelated style migration.
4. Prefer a simple design understandable by a reader outside the immediate
   team. Make ownership, mutation, lifetime, and surprising costs visible.
5. Use existing project libraries and abstractions before inventing new ones.

While implementing:

- Keep changes focused on the requested behavior.
- Preserve API contracts and invariants; update affected callers and comments.
- Prefer clear values and ordinary control flow over clever abstractions.
- Consider compilation cost, namespace pollution, portability, and maintenance
  by people unfamiliar with the original implementation.
- Justify complexity introduced for performance with evidence.
- Do not invent a waiver. Use an already documented project exception when it
  applies. If a proposed approach requires a new waiver, first seek an approach
  within the rules; leave a concrete unresolved decision for maintainers only
  if necessary.

Before delivering:

1. Review ownership, lifetimes, bounds, integer conversions, error paths,
   concurrency assumptions, and API changes.
2. Run the repository's appropriate formatter, linter, build, and relevant
   tests. Use `cpplint` when configured; it is useful but not a complete checker.
3. Inspect the diff for accidental changes and apply the final checklist.
4. Report what changed, actual validation results, and unresolved limitations.
   Never claim a command passed when it was not run.

## 2. Language baseline and restricted features

- MUST target C++20 under this snapshot. MUST NOT introduce C++23 features.
  Respect a repository's explicitly different supported baseline.
- MUST consider toolchain and platform portability before using newer features.
- MUST NOT use nonstandard language extensions directly, unless a documented
  exception applies. Approved project-wide portability wrappers are allowed.
- MUST NOT use C++20 modules (`module`, `import`, `export` for modules).
- MUST use coroutines only through libraries approved for project-wide use.
  MUST NOT implement custom promise or awaitable types.
- MUST NOT use `<ratio>`, `<cfenv>`, `<fenv.h>`, or `<filesystem>` under this
  policy. These are Google policy restrictions, not claims that the features
  are prohibited by the C++ language.
- MUST NOT introduce or use user-defined literals, including standard-library
  suffixes such as `"text"sv` or duration suffixes. Use explicit constructors.
- MUST NOT use `std::auto_ptr`; use `std::unique_ptr`.
- MUST NOT use C++ exceptions or exception-handling facilities such as
  `std::exception_ptr` or `std::nested_exception`. See the Windows exception
  below for compiler interoperability, which does not permit writing handlers.

## 3. Files, headers, and dependencies

### File structure

- SHOULD pair an implementation `.cc` with a `.h`. Unit tests and small files
  containing only `main()` are common exceptions.
- MUST make ordinary headers self-contained: independently includable, guarded,
  and directly including their prerequisites.
- MUST use `.h` for headers and `.cc` for implementations.
- MAY use `.inc` sparingly for intentionally non-self-contained textual
  fragments. Prefer normal headers.
- MUST use include guards based on the project-relative path:
  `<PROJECT>_<PATH>_<FILE>_H_`. MUST NOT substitute `#pragma once`.

```cpp
#ifndef ORBIT_STORAGE_RECORD_H_
#define ORBIT_STORAGE_RECORD_H_

// Declarations.

#endif  // ORBIT_STORAGE_RECORD_H_
```

### Include what is used

- MUST directly include the intended provider header for each external symbol
  a file uses. Do not depend on transitive includes, including those from its
  related header.
- MUST NOT include headers for unrelated reasons.
- SHOULD include definitions rather than forward-declare.
- MUST NOT forward-declare entities the project does not own, including
  standard-library entities.
- For owned entities, first make the provider header inexpensive. A forward
  declaration is allowed when engineering judgment identifies it as the better
  solution to a compilation slowdown.
- MUST NOT distort an object model into pointers merely to enable declarations.

### Include spelling and order

Use project-source-relative include paths without `.` or `..` components.
Use quotes unless the library requires angle brackets. Standard library and
system headers require angle brackets; some third-party headers do too.

Order groups as follows, alphabetizing within each and separating nonempty
groups by one blank line:

1. The related header implemented or tested by this `.cc` file.
2. C/system and other angle-bracket headers ending in `.h`.
3. C++ standard-library headers without extensions.
4. Other libraries' headers.
5. The project's headers.

Keep platform-specific conditional includes small and localized; they may
follow the other groups. C headers and their C++ counterparts are both
acceptable; follow nearby conventions.

### Definitions in headers

- SHOULD expose a body at its public declaration only when it is short,
  approximately ten lines or fewer.
- SHOULD put longer bodies in `.cc` files unless technical or performance
  requirements require a header definition.
- MUST make header definitions safe under the One Definition Rule. Use
  `inline` when required; templates and in-class definitions may be implicitly
  inline under the language rules.
- MUST make client-instantiated templates and inline-function definitions
  available through the declaring header. Do not require clients to include
  a separate `-inl.h` file.
- MAY keep templates in `.cc` when all instantiations are confined there.
- SHOULD place necessarily long header definitions in a clearly internal area
  rather than obscuring the public interface.

## 4. Namespaces, scope, and free functions

- SHOULD put almost all code in a namespace with a recognizable,
  project-specific, globally unique top-level name.
- MUST NOT use `using namespace ...`, even in implementation files.
- MUST NOT use inline namespaces or declare entities in `std`.
- MUST NOT export namespace aliases from public headers. Header aliases at
  namespace scope belong only in explicitly internal namespaces; function-local
  aliases and implementation-file aliases are allowed.
- SHOULD identify implementation namespaces with `internal` and make their
  names unique to the library/file where collisions could otherwise occur.
  Do not depend on another library's internal API.
- PREFER `namespace project::component { ... }` in new code.
- Wrap declarations and definitions after includes and necessary external
  declarations. Do not indent the namespace body. End multiline namespaces
  with `}  // namespace project::component`.
- SHOULD give implementation-only definitions in `.cc` internal linkage through
  an unnamed namespace or namespace-scope `static` functions/variables.
- MUST NOT introduce unnamed namespaces or namespace-scope internal-linkage
  definitions in headers. This does not ban static class members.
- PREFER free functions in namespaces for operations unrelated to an instance.
  Do not create a class merely to group static functions without a substantive
  reason, such as a trait or policy type.
- SHOULD avoid nonmember functions depending on external mutable variables.
- MUST keep local variables in the narrowest practical scope and initialize
  them at declaration near first use. Prefer declaring loop/control variables
  in their control statements.
- MAY move an expensive reusable object outside a loop when its construction
  cost warrants it and the resulting state management is correct.

## 5. Static, global, and thread-local storage

### Static storage

- MUST NOT use objects with static storage duration and nontrivial destructors.
  This includes global objects, static data members, function-local statics,
  and lifetime-extended temporaries.
- References are not themselves objects, but initialization and the lifetime
  of any referenced temporary still matter.
- PREFER constant initialization, marked `constexpr` for constants or
  `constinit` for mutable static-storage variables.
- SHOULD avoid dynamic initialization of nonlocal statics. It is allowed only
  when program behavior does not depend on its ordering against other
  initializations.
- MAY dynamically initialize function-local statics, subject to the destructor
  restriction.
- PREFER literal-backed string views, character arrays, or character pointers
  for global strings; avoid global `std::string`.
- PREFER fixed arrays of trivial types for static lookup tables. Ordinary
  static `std::map`, `std::set`, and owning smart pointers violate the
  destruction rule.
- If alternatives fail, the source permits a function-local static pointer or
  reference to an intentionally never-deleted heap object. Treat this as a
  deliberate process-lifetime design, not a general memory-management pattern.

### Thread-local storage

- PREFER `thread_local` over alternative thread-local mechanisms.
- MUST enforce constant initialization of class- or namespace-scope
  `thread_local` variables with `constinit` (or, rarely, `constexpr`).
- MAY use dynamic initialization for function-local `thread_local` variables.
- MUST account for thread-exit destruction order. Destructors must not access
  potentially destroyed thread-local state, including through called code.
- PREFER trivial types or destructors that provably run no user code.
- MUST NOT apply the intentional global-leak workaround casually to thread-local
  objects: it leaks resources for every terminated thread.
- Consider memory cost multiplied by thread count.

## 6. Class design, construction, and special members

### Classes and structs

- MUST use `struct` only for passive public data without cross-field invariants.
  Constructors and helpers are allowed if they do not enforce invariants.
- SHOULD use `class` for behavior, invariants, or a broadly exposed type likely
  to evolve. When uncertain, use `class`.
- MAY use `struct` for stateless traits, metafunctions, and some functors.
- PREFER named structs over pairs and tuples when fields have meaningful names.
  Pairs/tuples remain appropriate for generic code or existing APIs.
- MUST keep class data private, except constants. A Google Test fixture defined
  in the `.cc` using it may have protected data; header-defined fixtures may not.

### Construction and conversions

- MUST NOT call virtual functions from constructors.
- SHOULD avoid construction that can fail without a usable error signal.
  Prefer a factory returning the project's error/result type for recoverable
  failure. Avoid a merely half-initialized object requiring an easily forgotten
  `Init()` call. Termination may fit genuinely fatal initialization failures.
- MUST mark conversion operators and constructors callable with one argument
  `explicit`, except copy/move constructors and a sole
  `std::initializer_list` parameter.
- Constructors requiring multiple arguments may omit `explicit`.
- Any additional implicit-conversion exception requires a project waiver.

### Copying and moving

- MUST make copyability/movability clear in the public interface.
- For copyable classes, explicitly declare copy construction and assignment.
- For move-only classes, explicitly declare move construction and assignment.
- For neither-copyable-nor-movable classes, explicitly delete copy construction
  and assignment; this also suppresses implicit moves.
- Declaring/deleting all four operations is allowed, but not required.
- If an assignment operation is provided, MUST provide its corresponding
  constructor; explicitly handling one half makes the other non-obvious.
- MAY omit these declarations when the semantics are already obvious from
  public members or an unambiguously noncopyable/nonmovable base. An interface
  base with merely implicit operations is not sufficient clarification.
- PREFER correct compiler-generated/defaulted operations; review their behavior.
- SHOULD NOT offer copying/moving when semantics are unclear or cost surprising.
- SHOULD add explicit moves to copyable types only when substantially cheaper.
- SHOULD prevent slicing by using abstract or otherwise appropriately restricted
  bases, and avoid deriving from concrete classes.

### Inheritance, friends, and operators

- PREFER composition. MUST use public inheritance; private inheritance should
  be replaced with a member object.
- SHOULD use inheritance for an actual is-a relationship and minimize
  implementation inheritance. Multiple implementation inheritance is strongly
  discouraged, not absolutely banned; see Windows COM exceptions.
- MUST annotate overriding virtual methods/destructors with exactly one of
  `override` or `final`; do not repeat `virtual` on an override.
- MAY make classes `final` when inheritance is unsupported.
- Restrict `protected` to functions genuinely needed by subclasses.
- MAY use friends sparingly, preferably defined in the same file; prefer public
  interfaces for ordinary cooperation.
- MUST overload operators only with conventional, unsurprising meanings and
  on owned types in their associated files/namespaces.
- MUST keep related operators semantically consistent. PREFER nonmember,
  nonmutating binary operators so conversions work symmetrically.
- For equality-comparable values, define nonmember `operator==` and document
  equality. MAY define a consistent `operator<=>` for a natural ordering;
  prefer not to overload the other comparisons separately.
- Do not invent an unnatural ordering solely for a container; supply a comparator.
- MUST NOT overload `&&`, `||`, comma, unary `&`, or literal operators.
- Prefer idiomatic meaningful operators over artificial `Equals()`/`CopyFrom()`
  substitutes, while respecting streaming and conversion restrictions.

### Declaration order

Use `public`, then `protected`, then `private`, omitting empty sections.
Within each section, prefer:

1. Types, aliases, nested classes/enums, and friend types.
2. Optionally, nonstatic fields for structs only.
3. Static constants.
4. Factory functions.
5. Constructors and assignment operators.
6. Destructor.
7. Other member/static/friend functions.
8. Other data members.

## 7. Function interfaces

- PREFER returned values to output parameters, including a named result struct
  for meaningful multiple outputs.
- PREFER return by value, then by reference. Avoid returning a raw pointer unless
  null is meaningful. Any returned reference/view must have a valid lifetime.
- Use the following defaults:

| Purpose | Default representation |
| --- | --- |
| Required input | Value or `const T&` |
| Optional input ordinarily passed by value | `std::optional<T>` |
| Optional input ordinarily passed by reference | `const T*` |
| Required output or input/output | `T&` |
| Optional output or input/output | `T*` |
| Ownership transfer | Appropriate owning type, usually `std::unique_ptr<T>` |

- SHOULD avoid retaining reference parameters beyond a call. Prefer copying
  retained data, or accept a pointer and document lifetime/non-null requirements.
  A reference can bind to a temporary; retaining it can dangle.
- SHOULD place input-only parameters before output parameters. Existing related
  APIs, in/out parameters, and variadic signatures can justify exceptions.
- SHOULD write small, focused functions. Around 40 lines is a prompt to consider
  decomposition, not a mandatory maximum.
- MAY overload only when overloads have the same meaning and a call can be
  understood without resolving the exact overload. Prefer one umbrella comment
  explaining the whole overload set.
- MAY use default arguments on nonvirtual functions only if the default always
  evaluates to the same value and improves readability enough to justify it.
- MUST NOT put defaults on virtual functions or use changing defaults such as
  counters. Prefer overloads when unsure.
- PREFER leading return types. Use trailing return types only when required or
  materially clearer, typically for lambdas or unusual dependent return types.

## 8. Ownership, references, and lifetime

- PREFER values and a single fixed owner. Use dynamic allocation only when needed.
- PREFER ownership remaining with the allocating code; pass copies or borrowing
  pointers/references when transfer is unnecessary.
- PREFER `std::unique_ptr` for ownership transfer. Keep nonowning pointer/reference
  contracts explicit; smart pointers are not universal replacements for borrows.
- SHOULD NOT introduce shared ownership without a strong reason. Significant
  savings from avoiding expensive immutable copies can justify
  `std::shared_ptr<const T>`; prefer `std::shared_ptr` when sharing is needed.
- Check cycles, aliasing, deletion timing, and synchronization when sharing.
- MUST restrict rvalue references and `&&` method qualifiers to:
  move construction/assignment; methods that consume `*this`; forwarding
  references with `std::forward`; or justified `T&&`/`const T&` overload pairs.
- PREFER by-value ordinary parameters for consumed values. Performance-driven
  overload pairs need evidence that they improve the relevant case.
- Use `std::move` to express the intended transfer, checking later uses of the
  moved-from object and the actual operation selected.

## 9. Errors, noexcept, RTTI, and casts

### Errors and noexcept

- Use the established project error-return mechanism in exception-free code.
  Do not add an exceptions subsystem or introduce exception-dependent APIs.
- MAY use `noexcept` when useful and semantically correct: an escaping exception
  would represent a fatal error.
- `noexcept` on move constructors has an accepted performance benefit. Other
  performance-motivated uses should follow project-lead review policy.
- PREFER unconditional `noexcept` when exceptions are fully disabled. Otherwise
  keep conditional specifications simple and tied to actual throwing operations.
- Do not add `noexcept` mechanically: callers may depend on this lasting promise.

### RTTI

- SHOULD avoid production `typeid`/`dynamic_cast`, especially type-based dispatch
  chains. Prefer virtual dispatch or, where appropriate, a visitor.
- MAY freely use RTTI in unit tests and for downcasts whose dynamic type is
  guaranteed by program logic.
- PREFER an available project downcast helper where suitable.
- MUST NOT hand-build a type-tag system merely to circumvent RTTI guidance.

### Cast selection

Do not use C-style casts except casts to `void`. Function-style casts are
allowed only for class types. Choose the first applicable safe conversion:

1. Braced arithmetic conversion when narrowing must be rejected.
2. Function-style construction for a class destination.
3. An available `absl::implicit_cast` for an explicitly expressed safe implicit
   conversion, such as an upcast or added const qualification.
4. `static_cast` for an appropriate explicit value conversion or justified
   hierarchy conversion; downcasts require a proven dynamic type.
5. `absl::down_cast`, when available, for a known-safe hierarchy downcast.
6. `const_cast` only with justified const-correctness reasoning; it does not
   make modifying an actually const object valid.
7. `reinterpret_cast` only with a clear low-level need and understood aliasing,
   alignment, and representation constraints.
8. `std::bit_cast` for same-size value representation conversion; consider it
   instead of pointer reinterpretation when it fits.

These helper names do not authorize adding Abseil to an unrelated repository.
Use available equivalents consistent with the project's dependency policy.

## 10. Constness, arithmetic, and portability

- SHOULD use meaningful `const` in APIs: nonmutating pointer/reference inputs,
  logically nonmutating methods, and appropriate nonlocal variables.
- Do not put top-level `const` on by-value parameters in declarations merely
  for callers; it does not constrain their objects.
- Local `const` is neither required nor discouraged by the source.
- MUST make concurrent calls to a class's const operations safe, or clearly
  document that the class is thread-unsafe.
- PREFER `const T*` spelling; `T const*` is permitted for local consistency.
- Use `constexpr` for true constants and supporting constant-evaluable functions;
  `constinit` for constant initialization of mutable static-storage variables;
  `consteval` when runtime invocation must be impossible.
- Do not contort implementations to achieve constant evaluation, or use
  `constexpr`/`consteval` merely to force inlining.
- Use `int` for ordinary suitably bounded integers. For required widths use
  `int16_t`, `int32_t`, `int64_t`, etc., rather than `short`, `long`, or
  `long long`. Appropriate `size_t` and `ptrdiff_t` are permitted.
- Use a 64-bit type if a value can reach `2^31`; also check intermediate results.
  When uncertain, select a sufficiently large type.
- SHOULD avoid unsigned types except bit patterns, modular arithmetic, or a
  justified interoperability need. Nonnegativity alone is not justification.
- Minimize signed/unsigned mixing, narrowing, and overflow hazards. Prefer
  iterators and containers to pointer/size manipulation.
- PREFER omitting `std::` on standard integer aliases.
- MUST use only `float`/`double` for built-in floating-point types, not
  `long double`. This guide permits assuming binary32/binary64 respectively.
- MUST write architecture-portable code. Store integer representations of
  addresses in `uintptr_t`, not an assumed-width integer.
- Use serialization libraries for data exchanged across process boundaries,
  rather than copying native object representations.
- PREFER type-safe formatting instead of `printf`-family formatting.
- Construct wide constants and shift operands with adequate width before shifting.

## 11. Deduction, templates, concepts, and aliases

### Type deduction

- MUST use deduction to improve clarity or safety, not merely to save typing.
  Judge clarity for a reader who does not know the project.
- Function template argument deduction is normally appropriate.
- MAY use local `auto` for obvious construction results, iterators, or
  uninformative type boilerplate. Spell out meaningful non-obvious types.
- Prefer an explicitly typed local extracted from a complex iterator when the
  value type matters to understanding the code.
- MUST NOT use `decltype(auto)` if a simpler option works.
- Use deduced return types only for very short, narrowly scoped functions or
  lambdas. Public header functions should almost never deduce their return type.
- Avoid generic parameters when a concrete type suffices.
- MUST NOT use abbreviated `auto` parameters on non-lambda functions; use named
  template parameters if a function genuinely needs to be generic.
- In a lambda needing to name a parameter's type, prefer an explicit template
  parameter over deriving it repeatedly with `decltype`.
- MAY use structured bindings to give meaningful element names, especially for
  map entries and API-returned tuples. Check copies and reference qualifiers.
  If binding names differ from struct fields, consider `/*field_name=*/` labels.
- MUST use class template argument deduction only when maintainers explicitly
  support it with at least one explicit deduction guide. Standard-library
  templates are presumed to support it. General clarity rules still apply.

### Templates and concepts

- SHOULD avoid complicated template metaprogramming. Confine necessary complexity
  to widely reused low-level implementation details, document its use and
  generated behavior, and make compiler diagnostics understandable.
- Use concepts sparingly, in cases where pre-C++20 template constraints would
  already have been warranted. Do not make ordinary code generic to use concepts.
- PREFER equivalent standard concepts to type traits and modern constraints to
  `enable_if`/SFINAE.
- MUST use `requires(Concept<T>)`-style constraints rather than
  `template <Concept T>` syntax under this policy.
- MUST NOT reimplement existing concepts or traits.
- New concepts should be rare and internal to a library, not public API surface.
- MUST NOT define concepts with primarily semantic, compiler-unenforced
  requirements. Use comments, assertions, and tests for those contracts.
- Avoid redundant constraints that merely repeat an obvious function body.

### Aliases and library selection

- PREFER `using` to `typedef` in new C++.
- MUST expose a public alias only for clients' benefit and document whether it
  promises exact type identity or only a narrower compatibility contract.
- Keep convenience aliases in `.cc`, functions, private class sections, or
  explicitly internal namespaces.
- MUST NOT expose namespace aliases in public API.
- First use the library already established for the problem in the codebase.
  Otherwise the source's approximate preference is Abseil, permitted standard
  library facilities, existing first-party libraries, then other third parties.
- MUST follow repository third-party policies; consider maintenance, licensing,
  security updates, and upstream compatibility before adding dependencies.

## 12. Lambdas, control flow, and expressions

### Lambdas

- MAY use lambdas when they improve readability; avoid long nested bodies.
- PREFER explicit captures when a lambda may escape its creating scope.
  Explicit capture documents lifetime dependencies but does not fix them.
- MUST use `[&]` only when the lambda's lifetime is clearly shorter than all
  potential captures.
- Use `[=]` only for a short lambda with a few obvious captures and no implicit
  `this` capture. Capture `this` explicitly when needed.
- MUST understand that copying a pointer does not copy or extend its pointee.
- MUST use init-captures to capture enclosing variables, such as moving an
  existing owner. Do not use them to invent unrelated local names or substantially
  change the meaning of an existing name; declare such variables normally first.

### Switches and ordinary expressions

- MUST provide a `default` for switches not on an enum. For enum switches,
  omission is allowed so the compiler can diagnose unhandled enumerators.
- MUST treat a logically unreachable default as an error.
- MUST mark intentional fallthrough with `[[fallthrough]];` at the execution
  point. Consecutive labels without intervening code need no annotation.
- PREFER prefix `++i`/`--i` unless the previous value is needed.
- MUST use `nullptr` for null pointers and `'\0'` for a null character.
- PREFER `sizeof(variable)` when measuring a particular variable;
  `sizeof(Type)` is appropriate when no particular object is involved.
- MUST use symbolic operators (`&&`, `!`, etc.), not word alternatives
  (`and`, `not`, etc.).
- Do not wrap return expressions in unnecessary parentheses.
- MAY initialize with `=`, `()`, or `{}`. Check that braces do not accidentally
  select an `initializer_list` constructor instead of the intended operation.
- MUST use only C++20-compatible designated initializers in field-declaration
  order. Do not use C/compiler-extension forms with reordered fields.

## 13. Macros, streams, and text encoding

### Macros and extensions

- SHOULD avoid macros, especially exported header macros. Prefer functions,
  constants, enums, references, or other language facilities.
- MUST NOT generate pieces of a C++ API with macros without an applicable waiver.
- When a macro is necessary, prefer defining it immediately before use and
  undefining it immediately afterward. Never replace another macro by simply
  undefining it and reusing its name.
- MUST give exported macros globally unique uppercase project-prefixed names.
- Avoid unbalanced expansions and token-pasting-generated entity names.
- Minimize conditional compilation; preserve include guards and necessary
  localized platform handling.
- MUST NOT use direct compiler intrinsics, variable-length arrays, `alloca`,
  inline assembly, statement expressions, or other nonstandard extensions
  outside an allowed exception or designated portability wrapper.

### Streams

- Use streams only where they fit: typically local, ad-hoc, developer-facing
  human-readable output.
- PREFER established logging facilities for diagnostics and string libraries for
  string building rather than defaulting to `cerr` or `stringstream`.
- SHOULD avoid streams for end-user or untrusted-data interfaces; use suitable
  libraries with localization and security handling.
- Avoid persistent formatting state, manipulators, locale changes, callback
  registration, and custom stream state. Prefer explicit formatting functions.
- MUST overload streaming `<<` only for value types and print the human-visible
  value. Use a named function such as `DebugString()` for implementation internals.

### Encoding and literals

- Non-ASCII source content should be rare and MUST be UTF-8. Use escapes for
  invisible characters or when they improve readability.
- Do not hard-code localizable user-facing text; use the project's mechanism.
  Tests and data-format delimiters are reasonable literal-text cases.
- SHOULD avoid `u8` prefixes because their type semantics change across versions.
- SHOULD NOT use `char16_t`/`char32_t`; avoid `wchar_t` except Windows API interop.
- Floating-point literals MUST have digits on both sides of a radix point,
  including exponential notation: `0.5`, `1.0f`, `12.0e6`.
  Integer literals may initialize floating variables when exactly representable.

## 14. Naming

Choose names that explain purpose to readers outside the team. Scale detail to
scope; avoid both obscure abbreviations and redundant context. Do not abbreviate
by deleting internal letters. Treat acronyms as words: `ParseHttp`, not
`ParseHTTP`. Familiar short forms such as loop index `i` are fine.

| Entity | Convention | Example |
| --- | --- | --- |
| Source/header file | Lowercase, prefer underscores | `record_store.cc`, `record_store.h` |
| Test file | `_test.cc` | `record_store_test.cc` |
| Class/struct/enum/type alias | PascalCase | `RecordStore`, `RecordId` |
| Concept/type template parameter | PascalCase | `Serializable`, `Element` |
| Variable/parameter | snake_case | `record_count` |
| Class data member | snake_case plus trailing underscore | `record_count_` |
| Struct data member | snake_case, no trailing underscore | `record_count` |
| Program-fixed constant | `k` plus PascalCase | `kMaxRecordCount` |
| Enumerator, scoped or unscoped | Constant style | `kMissingRecord` |
| Function | PascalCase | `LoadRecord()` |
| Accessor/mutator | May use snake_case | `size()`, `set_size()` |
| Namespace | snake_case | `orbit_storage` |
| Necessary macro | UPPER_SNAKE_CASE, project prefix | `ORBIT_STORAGE_EXPORT` |

- Follow existing lowercase dash conventions for filenames when the project
  uses them; otherwise prefer underscores. Avoid system-header filename
  collisions and vague names. `_unittest`/`_regtest` are deprecated naming forms.
- Constant naming applies to `const`/`constexpr` values fixed for the program,
  including static constant class members. A merely const value computed
  differently on each call does not qualify for `k` naming.
- `k` naming is optional for qualifying constants without static storage duration.
- Non-type template parameters follow variable or constant naming as appropriate.
- Name aliases according to their entity category in their new context.
- Function-like public objects intended to be called like functions may use
  function naming.
- Avoid well-known top-level namespace names such as `std`/`absl` for nested
  namespaces, because lookup can still become confusing.
- MAY follow an existing C/C++/Rust interop or STL convention for genuinely
  analogous entities.
- MUST use respectful, inclusive, gender-neutral language in code and comments.

## 15. Comments and documentation

- PREFER clear names over comments explaining obscure names. Explain intent,
  contracts, invariants, and non-obvious reasoning rather than restating syntax.
- MAY use `//` or `/* ... */` consistently; `//` is more common.
- MUST use the project's appropriate license boilerplate. New files usually
  should not add an author line or copyright notice beyond required policy.
- Document a file's collection of user-facing abstractions when it has several,
  including what belongs there. Do not duplicate a header overview in its `.cc`
  or test file; an implementation without a header may need its own overview.
- MUST document non-obvious classes/structs: purpose, use, lifetime expectations,
  synchronization assumptions, and relevant invariants. A small usage example
  can help. Put usage documentation at the interface, operational details at
  implementation.
- SHOULD document almost every non-obvious function declaration, including private
  and `.cc`-local functions. Start with a descriptive verb phrase such as
  “Returns” or “Loads.” Obvious accessors and trivial operations need no prose.
- Document inputs/outputs, retained arguments, borrowing lifetimes, pointer
  nullability and null behavior, whether outputs append or overwrite, error
  behavior, and relevant performance implications.
- Override comments should describe differences rather than repeat base contracts.
- Constructor/destructor comments should explain non-obvious ownership or cleanup,
  not merely say that they construct/destroy.
- MUST explain member invariants, sentinel values, or lifetime relationships not
  obvious from names/types. Global variables need purpose comments and, when
  unclear, an explanation of why they are global.
- Explain tricky algorithms and synchronization before the relevant code.
- For unclear call arguments, prefer named constants, enums instead of booleans,
  an options struct/class, or named intermediate expressions. As a last resort,
  use `/*parameter_name=*/value` argument comments.
- Use readable grammar, capitalization, and punctuation; avoid redundant prose.
- Temporary compromises MAY use `TODO(identifier): explanation`. Include a real
  issue, owner, email, or contextual reference; do not invent one. Time-based
  TODOs need a specific date or concrete removal condition.

## 16. Formatting

Use repository formatting tools. Where no project configuration is provided,
these are the Google defaults; a formatter is not a substitute for semantic
review and may not capture every source rule.

### Layout and whitespace

- MUST use spaces, not tabs, with two-space block indentation.
- MUST normally limit code to 80 columns.
- Exceptions include unsplittable comments/commands/URLs, semantically sensitive
  string literals, includes, guards, and using-declarations.
- Unsplittable strings should appear near the top at namespace scope except in
  tests. Disable formatting narrowly if necessary to preserve such content.
- MUST NOT introduce trailing whitespace. Clean existing whitespace on changed
  lines; keep unrelated cleanup separate.
- Use blank lines to separate meaningful groups, not at every opportunity or
  immediately inside ordinary blocks.
- Do not indent namespace contents.
- Put preprocessor `#` in column one, including inside indented C++ blocks.
- Separate end-of-line comments from code by at least two spaces.

### Functions, calls, and lists

- Keep return type with function name when practical; if split, do not indent
  the name merely because the return type occupies its own line.
- No space between a function name and `(`; no padding inside parameter lists.
- Put the opening function brace on the last signature line, separated by a
  space. Closing brace normally occupies its own line; trivial bodies may fit
  on one line.
- Put attributes before the return type at the beginning of a declaration.
- Name parameters unless unused. For non-obvious unused parameters, retain the
  name in a comment in the definition, e.g. `double /*angle*/`.
- Keep calls on one line when readable. Otherwise align continuation arguments
  with the first argument, or start below with a four-space continuation indent.
- Do not impose one argument per line universally. Preserve useful structure
  and extract complex expressions into named locals where helpful.
- Format braced initializer lists like the corresponding function call.
- Format lambda parameters/bodies like functions and captures like other lists.
  Write `[&value]` without a space after `&`.

### Branches and loops

- Use a space after control keywords: `if (ready) {`.
- Put braces around controlled blocks by default; opening brace stays on the
  control line, closing brace on its own line, with `} else {` together.
- Historical source exceptions permit a brief single-keyword statement on one
  line, or a one-line condition plus one-line unbraced body. They are discouraged
  and do not apply to `if ... else` or `do ... while`. A project may require
  braces universally; use braces in new code by default.
- Empty loops MUST use braces or `continue`, not a bare semicolon.
- Indent `case` labels two spaces inside a switch; body statements another two.
  Braces around case bodies are optional.
- Keep long boolean expression wrapping consistent; trailing operators are
  common, leading operators are allowed. Add useful parentheses, not clutter.

### Pointers, operators, and initialization

- MUST attach pointer/reference symbols to the type: `Widget* widget`,
  `const Widget& widget`. Do not write `Widget *widget`.
- MUST NOT declare multiple variables together if any has a pointer/reference
  declarator. Plain declarations such as `int x, y;` are allowed when helpful.
- No spaces around member access `.`/`->` or after unary dereference/address-of.
- Put spaces around assignment operators; ordinarily around binary operators.
  Tighter factor formatting is allowed when clear. Unary operators attach to
  their operands. Template angle brackets and casts have no internal padding.
- Use spaces around range-for colons and inheritance/initializer-list colons.
- Do not pad ordinary parentheses. Spaces inside braced initializer lists are
  optional but must be symmetric.
- Use `{}` for an empty body; inline nonempty bodies have internal spaces.
- For a multiline constructor initializer list, break before `:` and indent it
  four spaces; align subsequent members, normally one per line.
- Initialize members in their declaration order to reflect actual C++ semantics.
  This is an agent correctness check in addition to the source layout guidance.

### Class layout example

This small example demonstrates layout and explicit noncopyability, not a
universal class template. Each real class still needs an appropriate contract.

```cpp
class RecordStore {
 public:
  RecordStore() = default;
  RecordStore(const RecordStore&) = delete;
  RecordStore& operator=(const RecordStore&) = delete;

  int size() const { return record_count_; }

 private:
  int record_count_ = 0;
};
```

## 17. Existing code and Windows exceptions

- MAY diverge from these defaults to maintain necessary local consistency in
  code written under another style. Do not treat old code as justification for
  copying unsafe patterns or ignoring a documented migration direction.
- Windows code follows the same naming and `.cc` conventions; no Hungarian
  notation.
- MAY use Windows API types where appropriate. Prefer spellings close to the
  underlying C++ type, such as `const TCHAR*` over `LPCTSTR`.
- For MSVC, MUST use warning level 3 or higher and treat warnings as errors.
- MUST use include guards instead of `#pragma once`.
- Necessary DLL import/export extensions are allowed through portability macros.
- Multiple implementation inheritance is permitted for required COM/ATL/WTL
  interfaces.
- Disable ATL exceptions with `_ATL_NO_EXCEPTIONS`. Investigate disabling STL
  exceptions; compiler exception support may remain enabled if required to
  compile the STL. This does not permit writing exception-handling code.
- Prefer forced inclusion (`/FI`) for precompiled headers rather than explicit
  inclusion throughout source files, except the precompiled-header `.cc`.
- Windows resource headers containing only macros are exempt from these rules.

## 18. Final agent review checklist

- [ ] Applicable repository instructions, baseline, and local exceptions checked.
- [ ] No forbidden language/library features or unauthorized dependencies added.
- [ ] Headers are self-contained, guarded, and include their direct dependencies.
- [ ] Include paths, grouping, and alphabetical order checked.
- [ ] Public API remains readable; implementation detail stays internal.
- [ ] Ownership, nullability, retained arguments, and lifetimes are clear.
- [ ] Copy/move behavior and constructors preserve valid object states.
- [ ] No virtual constructor calls, accidental conversions, or slicing hazards.
- [ ] Static/thread-local initialization and destruction rules satisfied.
- [ ] Errors follow project conventions; no new exception paths introduced.
- [ ] Integer widths, conversions, overflow, and portable representations checked.
- [ ] Lambda captures and asynchronous lifetimes checked.
- [ ] Const operations and shared state satisfy documented concurrency contracts.
- [ ] Templates/deduction/operators improve clarity rather than conceal behavior.
- [ ] Names, comments, TODOs, and formatting follow the applicable conventions.
- [ ] Relevant formatting/lint/build/tests completed, or blockers reported plainly.
- [ ] Final diff contains only intended changes.

## 19. Source section index

The following index links every titled topic in the reviewed page to its
corresponding agent section. Consult the source for rationale and edge cases.

| Source topic | Agent section |
| --- | --- |
| [Background](https://google.github.io/styleguide/cppguide.html#Background) | 1 |
| [Goals of the Style Guide](https://google.github.io/styleguide/cppguide.html#Goals) | 1 |
| [C++ Version](https://google.github.io/styleguide/cppguide.html#C++_Version) | 2 |
| [Header Files](https://google.github.io/styleguide/cppguide.html#Header_Files) | 3 |
| [Self-contained Headers](https://google.github.io/styleguide/cppguide.html#Self_contained_Headers) | 3 |
| [The #define Guard](https://google.github.io/styleguide/cppguide.html#The__define_Guard) | 3 |
| [Include What You Use](https://google.github.io/styleguide/cppguide.html#Include_What_You_Use) | 3 |
| [Forward Declarations](https://google.github.io/styleguide/cppguide.html#Forward_Declarations) | 3 |
| [Defining Functions in Header Files](https://google.github.io/styleguide/cppguide.html#Defining_Functions_in_Header_Files) | 3 |
| [Names and Order of Includes](https://google.github.io/styleguide/cppguide.html#Names_and_Order_of_Includes) | 3 |
| [Scoping](https://google.github.io/styleguide/cppguide.html#Scoping) | 4 |
| [Namespaces](https://google.github.io/styleguide/cppguide.html#Namespaces) | 4 |
| [Internal Linkage](https://google.github.io/styleguide/cppguide.html#Internal_Linkage) | 4 |
| [Nonmember, Static Member, and Global Functions](https://google.github.io/styleguide/cppguide.html#Nonmember,_Static_Member,_and_Global_Functions) | 4 |
| [Local Variables](https://google.github.io/styleguide/cppguide.html#Local_Variables) | 4 |
| [Static and Global Variables](https://google.github.io/styleguide/cppguide.html#Static_and_Global_Variables) | 5 |
| [thread_local Variables](https://google.github.io/styleguide/cppguide.html#thread_local) | 5 |
| [Classes](https://google.github.io/styleguide/cppguide.html#Classes) | 6 |
| [Doing Work in Constructors](https://google.github.io/styleguide/cppguide.html#Doing_Work_in_Constructors) | 6 |
| [Implicit Conversions](https://google.github.io/styleguide/cppguide.html#Implicit_Conversions) | 6 |
| [Copyable and Movable Types](https://google.github.io/styleguide/cppguide.html#Copyable_Movable_Types) | 6 |
| [Structs vs. Classes](https://google.github.io/styleguide/cppguide.html#Structs_vs._Classes) | 6 |
| [Structs vs. Pairs and Tuples](https://google.github.io/styleguide/cppguide.html#Structs_vs._Tuples) | 6 |
| [Inheritance](https://google.github.io/styleguide/cppguide.html#Inheritance) | 6 |
| [Operator Overloading](https://google.github.io/styleguide/cppguide.html#Operator_Overloading) | 6 |
| [Access Control](https://google.github.io/styleguide/cppguide.html#Access_Control) | 6 |
| [Declaration Order](https://google.github.io/styleguide/cppguide.html#Declaration_Order) | 6 |
| [Functions](https://google.github.io/styleguide/cppguide.html#Functions) | 7 |
| [Inputs and Outputs](https://google.github.io/styleguide/cppguide.html#Inputs_and_Outputs) | 7 |
| [Write Short Functions](https://google.github.io/styleguide/cppguide.html#Write_Short_Functions) | 7 |
| [Function Overloading](https://google.github.io/styleguide/cppguide.html#Function_Overloading) | 7 |
| [Default Arguments](https://google.github.io/styleguide/cppguide.html#Default_Arguments) | 7 |
| [Trailing Return Type Syntax](https://google.github.io/styleguide/cppguide.html#trailing_return) | 7 |
| [Google-Specific Magic](https://google.github.io/styleguide/cppguide.html#Google-Specific_Magic) | 1, 18 |
| [cpplint](https://google.github.io/styleguide/cppguide.html#cpplint) | 1, 18 |
| [Other C++ Features](https://google.github.io/styleguide/cppguide.html#Other_C++_Features) | 8–13 |
| [Ownership and Smart Pointers](https://google.github.io/styleguide/cppguide.html#Ownership_and_Smart_Pointers) | 8 |
| [Rvalue References](https://google.github.io/styleguide/cppguide.html#Rvalue_references) | 8 |
| [Friends](https://google.github.io/styleguide/cppguide.html#Friends) | 6 |
| [Exceptions](https://google.github.io/styleguide/cppguide.html#Exceptions) | 2, 9, 17 |
| [noexcept](https://google.github.io/styleguide/cppguide.html#noexcept) | 9 |
| [Run-Time Type Information (RTTI)](https://google.github.io/styleguide/cppguide.html#Run-Time_Type_Information__RTTI_) | 9 |
| [Casting](https://google.github.io/styleguide/cppguide.html#Casting) | 9 |
| [Streams](https://google.github.io/styleguide/cppguide.html#Streams) | 13 |
| [Preincrement and Predecrement](https://google.github.io/styleguide/cppguide.html#Preincrement_and_Predecrement) | 12 |
| [Use of const](https://google.github.io/styleguide/cppguide.html#Use_of_const) | 10 |
| [Use of constexpr, constinit, and consteval](https://google.github.io/styleguide/cppguide.html#Use_of_constexpr) | 10 |
| [Integer Types](https://google.github.io/styleguide/cppguide.html#Integer_Types) | 10 |
| [Floating-Point Types](https://google.github.io/styleguide/cppguide.html#Floating-Point_Types) | 10 |
| [Architecture Portability](https://google.github.io/styleguide/cppguide.html#Architecture_Portability) | 10 |
| [Preprocessor Macros](https://google.github.io/styleguide/cppguide.html#Preprocessor_Macros) | 13 |
| [0 and nullptr/NULL](https://google.github.io/styleguide/cppguide.html#0_and_nullptr/NULL) | 12 |
| [sizeof](https://google.github.io/styleguide/cppguide.html#sizeof) | 12 |
| [Type Deduction (including auto)](https://google.github.io/styleguide/cppguide.html#Type_deduction) | 11 |
| [Class Template Argument Deduction](https://google.github.io/styleguide/cppguide.html#CTAD) | 11 |
| [Designated Initializers](https://google.github.io/styleguide/cppguide.html#Designated_initializers) | 12 |
| [Lambda Expressions](https://google.github.io/styleguide/cppguide.html#Lambda_expressions) | 12, 16 |
| [Template Metaprogramming](https://google.github.io/styleguide/cppguide.html#Template_metaprogramming) | 11 |
| [Concepts and Constraints](https://google.github.io/styleguide/cppguide.html#Concepts) | 11 |
| [C++20 modules](https://google.github.io/styleguide/cppguide.html#modules) | 2 |
| [Coroutines](https://google.github.io/styleguide/cppguide.html#coroutines) | 2 |
| [Disallowed standard library features](https://google.github.io/styleguide/cppguide.html#Disallowed_Stdlib) | 2 |
| [Third-party Libraries](https://google.github.io/styleguide/cppguide.html#Third_party_Libraries) | 11 |
| [Nonstandard Extensions](https://google.github.io/styleguide/cppguide.html#Nonstandard_Extensions) | 2, 13 |
| [Aliases](https://google.github.io/styleguide/cppguide.html#Aliases) | 11 |
| [Switch Statements](https://google.github.io/styleguide/cppguide.html#Switch_Statements) | 12 |
| [Inclusive Language](https://google.github.io/styleguide/cppguide.html#Inclusive_Language) | 14 |
| [Naming](https://google.github.io/styleguide/cppguide.html#Naming) | 14 |
| [Choosing Names](https://google.github.io/styleguide/cppguide.html#General_Naming_Rules) | 14 |
| [File Names](https://google.github.io/styleguide/cppguide.html#File_Names) | 14 |
| [Type Names](https://google.github.io/styleguide/cppguide.html#Type_Names) | 14 |
| [Concept Names](https://google.github.io/styleguide/cppguide.html#Concept_Names) | 14 |
| [Variable Names](https://google.github.io/styleguide/cppguide.html#Variable_Names) | 14 |
| [Constant Names](https://google.github.io/styleguide/cppguide.html#Constant_Names) | 14 |
| [Function Names](https://google.github.io/styleguide/cppguide.html#Function_Names) | 14 |
| [Namespace Names](https://google.github.io/styleguide/cppguide.html#Namespace_Names) | 14 |
| [Enumerator Names](https://google.github.io/styleguide/cppguide.html#Enumerator_Names) | 14 |
| [Template Parameter Names](https://google.github.io/styleguide/cppguide.html#Template_Parameter_Names) | 14 |
| [Macro Names](https://google.github.io/styleguide/cppguide.html#Macro_Names) | 14 |
| [Aliases](https://google.github.io/styleguide/cppguide.html#Naming_Aliases) | 14 |
| [Exceptions to Naming Rules](https://google.github.io/styleguide/cppguide.html#Exceptions_to_Naming_Rules) | 14 |
| [Comments](https://google.github.io/styleguide/cppguide.html#Comments) | 15 |
| [Comment Style](https://google.github.io/styleguide/cppguide.html#Comment_Style) | 15 |
| [File Comments](https://google.github.io/styleguide/cppguide.html#File_Comments) | 15 |
| [Struct and Class Comments](https://google.github.io/styleguide/cppguide.html#Class_Comments) | 15 |
| [Function Comments](https://google.github.io/styleguide/cppguide.html#Function_Comments) | 15 |
| [Variable Comments](https://google.github.io/styleguide/cppguide.html#Variable_Comments) | 15 |
| [Implementation Comments](https://google.github.io/styleguide/cppguide.html#Implementation_Comments) | 15 |
| [Punctuation, Spelling, and Grammar](https://google.github.io/styleguide/cppguide.html#Punctuation_Spelling_and_Grammar) | 15 |
| [TODO Comments](https://google.github.io/styleguide/cppguide.html#TODO_Comments) | 15 |
| [Formatting](https://google.github.io/styleguide/cppguide.html#Formatting) | 16 |
| [Line Length](https://google.github.io/styleguide/cppguide.html#Line_Length) | 16 |
| [Non-ASCII Characters](https://google.github.io/styleguide/cppguide.html#Non-ASCII_Characters) | 13, 16 |
| [Spaces vs. Tabs](https://google.github.io/styleguide/cppguide.html#Spaces_vs._Tabs) | 16 |
| [Function Declarations and Definitions](https://google.github.io/styleguide/cppguide.html#Function_Declarations_and_Definitions) | 16 |
| [Lambda Expressions](https://google.github.io/styleguide/cppguide.html#Formatting_Lambda_Expressions) | 16 |
| [Floating-point Literals](https://google.github.io/styleguide/cppguide.html#Floating_Literals) | 13, 16 |
| [Function Calls](https://google.github.io/styleguide/cppguide.html#Function_Calls) | 16 |
| [Braced Initializer List Format](https://google.github.io/styleguide/cppguide.html#Braced_Initializer_List_Format) | 16 |
| [Looping and Branching Statements](https://google.github.io/styleguide/cppguide.html#Formatting_Looping_Branching) | 16 |
| [Pointer and Reference Expressions and Types](https://google.github.io/styleguide/cppguide.html#Pointer_and_Reference_Expressions) | 16 |
| [Boolean Expressions](https://google.github.io/styleguide/cppguide.html#Boolean_Expressions) | 16 |
| [Return Values](https://google.github.io/styleguide/cppguide.html#Return_Values) | 16 |
| [Variable and Array Initialization](https://google.github.io/styleguide/cppguide.html#Variable_and_Array_Initialization) | 16 |
| [Preprocessor Directives](https://google.github.io/styleguide/cppguide.html#Preprocessor_Directives) | 16 |
| [Class Format](https://google.github.io/styleguide/cppguide.html#Class_Format) | 16 |
| [Constructor Initializer Lists](https://google.github.io/styleguide/cppguide.html#Constructor_Initializer_Lists) | 16 |
| [Namespace Formatting](https://google.github.io/styleguide/cppguide.html#Namespace_Formatting) | 16 |
| [Horizontal Whitespace](https://google.github.io/styleguide/cppguide.html#Horizontal_Whitespace) | 16 |
| [Vertical Whitespace](https://google.github.io/styleguide/cppguide.html#Vertical_Whitespace) | 16 |
| [Exceptions to the Rules](https://google.github.io/styleguide/cppguide.html#Exceptions_to_the_Rules) | 17 |
| [Existing Non-conformant Code](https://google.github.io/styleguide/cppguide.html#Existing_Non-conformant_Code) | 17 |
| [Windows Code](https://google.github.io/styleguide/cppguide.html#Windows_Code) | 17 |
