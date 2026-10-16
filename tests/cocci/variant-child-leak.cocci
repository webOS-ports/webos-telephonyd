// g_variant_get_child_value() returns a new reference; passing it straight
// into another call loses it.
@@
expression V, N;
identifier f !~ "^g_variant_unref$";
@@
* f(..., g_variant_get_child_value(V, N), ...)
