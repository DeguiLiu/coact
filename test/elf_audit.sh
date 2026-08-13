#!/usr/bin/env bash
# coact ELF/map zero-heap symbol audit (design §15.8 / §15.9).
#
# Blacklist per design §15.8: the coact/cmdfw framework must not reference
#   rt_malloc/rt_free, rt_*_create (rt_thread_create / rt_sem_create /
#   rt_mutex_create / rt_event_create / ...), malloc/free/realloc/calloc,
#   operator new/delete, __cxa_throw, __cxa_allocate_exception,
#   __dynamic_cast, or libatomic (__atomic_* / libatomic.so).
#
# Usage:
#   test/elf_audit.sh <nm-tool> <ELF-or-object>...
#   e.g. test/elf_audit.sh arm-none-eabi-nm \
#            build/applications/coact_static_pal_qemu.o \
#            /home/dgliu/coact/src/core/pal_rtthread.o
#
# Exit 0 when every input is clean (no banned symbol); exit 1 and print the
# offending symbol + object otherwise. `nm -u` on a relocatable object lists
# the references the linker must resolve; on a final ELF it lists true
# undefined symbols (dynamic) - for static images pass the framework OBJECTS.
set -u

NM="${1:?usage: $0 <nm-tool> <object...>}"
shift

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <nm-tool> <object...>" >&2
    exit 2
fi

# Banned symbols (design §15.8 blacklist). `-E` matches any of these as a
# word. rt_*_create is matched by `rt_.*_create`.
BANNED_RE='rt_malloc|rt_free|(^|[^_a-zA-Z0-9])(malloc|free|realloc|calloc)([^_a-zA-Z0-9]|$)|rt_.*_create|operator[[:space:]]+new|operator[[:space:]]+delete|__cxa_throw|__cxa_allocate_exception|__cxa_free_exception|__dynamic_cast|__atomic_|__sync_'

rc=0
for obj in "$@"; do
    if [ ! -f "$obj" ]; then
        echo "MISSING: $obj" >&2
        rc=1
        continue
    fi
    hits=$("$NM" -u --demangle "$obj" 2>/dev/null | grep -E "$BANNED_RE" || true)
    if [ -n "$hits" ]; then
        echo "HIT: $obj"
        echo "$hits"
        rc=1
    else
        echo "CLEAN: $obj"
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "elf_audit: PASS (no banned symbols in $(echo "$*" | wc -w) object(s))"
else
    echo "elf_audit: FAIL (banned symbol(s) above)"
fi
exit "$rc"
