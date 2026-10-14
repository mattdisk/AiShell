#ifndef AISHELL_DEMO_TRACE_H
#define AISHELL_DEMO_TRACE_H

/*
 * Compile-time demo/debug trace mode.
 *
 * Build with -DDEMO_TRACE to enable trace output on stderr.
 * Normal builds have zero overhead — the macro expands to nothing.
 *
 * Usage:
 *   make clean && make demo
 *   -- or --
 *   make clean && make CFLAGS_EXTRA="-DDEMO_TRACE"
 */

#ifdef DEMO_TRACE
#  include <stdio.h>
#  define TRACE(fmt, ...) fprintf(stderr, "[demo] " fmt "\n", ##__VA_ARGS__)
#else
#  define TRACE(fmt, ...) ((void)0)
#endif

#endif /* AISHELL_DEMO_TRACE_H */
