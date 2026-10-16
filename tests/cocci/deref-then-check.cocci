// A pointer used and only then checked for NULL: the check comes too late.
@@
expression E;
identifier fld;
@@
* E->fld
  ... when != E = ...
(
* if (E == NULL || ...) { ... }
|
* if (!E || ...) { ... }
)
