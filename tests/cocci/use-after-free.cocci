// A pointer used, or freed again, after g_free()/free() without being
// assigned in between.
@@
expression E;
identifier fld;
identifier freefn = {g_free, free, stk_request_free, stk_menu_free};
@@
  freefn(E);
  ... when != E = ...
      when != &E
(
* E->fld
|
* freefn(E);
)
