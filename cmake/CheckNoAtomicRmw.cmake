# Fails when a header uses a read-modify-write atomic, an atomic operator, a lock or thread_local (single
# writer per slot, no shared mutable state: CLAUDE.md rules 1 and 2). Allowed: atomic_ref / atomic plain
# load and store with relaxed, acquire or release order, and fences.
# Usage: cmake -DHEADER_DIR=<dir> -P CheckNoAtomicRmw.cmake    (every *.h under HEADER_DIR is checked)
set(_banned
    "fetch_[a-z]+"
    "exchange"
    "test_and_set"
    "atomic_flag"
    "mutex"
    "spinlock"
    "thread_local"
    "lock_guard"
    "unique_lock"
    "scoped_lock"
    "shared_lock")
# An operator applied on a line that mentions an atomic: ++ -- += -= |= &= ^= (and the plain compound forms).
set(_atomic_operator "atomic.*(\\+\\+|--|\\+=|-=|\\|=|&=|\\^=)")

file(GLOB_RECURSE _headers "${HEADER_DIR}/*.h")
if(NOT _headers)
    message(FATAL_ERROR "CheckNoAtomicRmw: no headers found under '${HEADER_DIR}'")
endif()
foreach(_h ${_headers})
    get_filename_component(_name "${_h}" NAME)
    file(STRINGS "${_h}" _lines)
    foreach(_l ${_lines})
        foreach(_b ${_banned})
            if(_l MATCHES "${_b}")
                message(FATAL_ERROR "${_name}: read-modify-write atomic or lock is forbidden (${_b}): ${_l}")
            endif()
        endforeach()
        if(_l MATCHES "${_atomic_operator}")
            message(FATAL_ERROR "${_name}: read-modify-write atomic or lock is forbidden (atomic operator): ${_l}")
        endif()
    endforeach()
endforeach()
