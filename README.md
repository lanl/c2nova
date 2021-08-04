c2nova
======

Description
-----------

This program translates simple kernels of C code to [Singular Computing](https://www.singularcomputing.com/)'s Nova macro language.

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

* 1-D and 2-D arrays must have a constant number of elements.

* `for` loops need to be expressed directly in Nova: `CUFor(i, IntConst(1), IntConst(10), IntConst(1));`…`CuForEnd();`.

* Hiding C constructs behind C macros will often confuse `c2nova`.

* All C integer data types are mapped to a Nova `Int`, and all C floating-point data types are mapped to a nova `Approx`.  No other data types are supported.

* Pre/post increment/decrement (i.e., `x++`, `x--`, `++x`, and `--x`) must appear only as standalone statements, not as expressions nested within other expressions.

* C operations with no Nova equivalent (e.g., `%`) are left untranslated.

* Function headers (e.g., `int myfunc(int x)`) have no Nova equivalent but will be translated anyway.

* Array accesses appearing on the left-hand side of a compound assignment operator (e.g., `+=`) will not be translated to Nova.

* `c2nova` will translate casts between integers and floating-point values, but these are not actually supported by Nova.

* `c2nova` will translate integer multiplication and division, but these are not actually supported by Nova.

As of this writing, `c2nova` has not been tested against the actual Nova macro library.  It is quite possible that *nothing* works.

Author
------

Scott Pakin, *pakin@lanl.gov*
