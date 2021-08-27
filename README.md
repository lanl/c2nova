c2nova
======

Description
-----------

This program translates simple kernels of C code to [Singular Computing](https://www.singularcomputing.com/)'s Nova macro language.

Installation
------------

`c2nova` requires [Clang](https://clang.llvm.org/) and the Clang libraries.  Given those, a simple
```
make
```
should work.  At the time of this writing, `c2nova` has been tested only with Clang 12 and only on Linux, but it should be portable to other platforms.

Usage
-----

From the command line,
```bash
c2nova input.c > output.nova
```

Variable declarations can be controlled using "magic comments":

| Code + comment         | Output                       |
| ---------------------- | ---------------------------- |
| `int x;`               | `DeclareApeVar(x, Int)`      |
| `int x; // [CU]`       | `DeclareCUVar(x, Int)`       |
| `int x; // [mem]`      | `DeclareApeMemVar(x, Int)`   |
| `int x; // [CU] [mem]` | `DeclareCUMemVar(x, Int)`    |

`if` statements normally run on the APEs.  To run them instead on the CU, define a macro
```C
#define CU_IF if
```
and use `CU_IF` in place of `if` when the statement needs to execute on the CU.

Because Nova is a set of C macros, Nova macros can be interspersed with C code.  This is how a program can exploit Nova features that have no direct C equivalent.

Limitations
-----------

The current implementation of `c2nova` is rather crude.  It therefore exhibits a number of limitations:

* 1-D and 2-D arrays must have a constant number of elements.  3-D and higher-dimensional arrays are not supported.

* `for` loops need to be expressed directly in Nova: `CUFor(i, 1, 10, 1);`…`CuForEnd();`.

* Hiding C constructs behind C macros will often confuse `c2nova`.

* The C99 `_Bool` type is mapped to a Nova `Bool`.  All other C integer data types are mapped to a Nova `Int`.  All C floating-point data types are mapped to a nova `Approx`.  No other data types are supported.

* Pre/post increment/decrement (i.e., `x++`, `x--`, `++x`, and `--x`) must appear only as standalone statements, not as expressions nested within other expressions.

* C operations with no Nova equivalent (e.g., `%`) are left untranslated.

* Function headers (e.g., `int myfunc(int x)`) have no Nova equivalent but will be translated anyway.

* `c2nova` will translate casts between integers and floating-point values, but these are not actually supported by Nova.

* `c2nova` will translate integer multiplication and division, but these are not actually supported by Nova.

As of this writing, `c2nova` has not been tested against the actual Nova macro library.  It is quite possible that *nothing* works.

Legal statement
---------------

Copyright © 2021 Triad National Security, LLC.
All rights reserved.

This program was produced under U.S. Government contract 89233218CNA000001 for Los Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC for the U.S.  Department of Energy/National Nuclear Security Administration. All rights in the program are reserved by Triad National Security, LLC, and the U.S. Department of Energy/National Nuclear Security Administration. The Government is granted for itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide license in this material to reproduce, prepare derivative works, distribute copies to the public, perform publicly and display publicly, and to permit others to do so.

This program is open source under the [BSD-3 License](LICENSE.md).  Its LANL-internal identifier is C21041.

Author
------

Scott Pakin, *pakin@lanl.gov*
