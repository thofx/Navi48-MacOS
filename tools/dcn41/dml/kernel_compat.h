/* kernel_compat.h - the kernel compiler-attribute macros Linux's DML2.1 uses and tools/dcn41/linuxshim does not define
 * (include/linux/compiler_types.h, compiler_attributes.h) and INT_MIN, which the kernel's headers bring in implicitly.
 * Force-included by run.sh; the attributes are hints, not semantics. */
#ifndef N48_DML_KERNEL_COMPAT_H
#define N48_DML_KERNEL_COMPAT_H
#include <limits.h>
#define __counted_by(member)
#define noinline __attribute__((__noinline__))
#define noinline_for_stack noinline
#endif
