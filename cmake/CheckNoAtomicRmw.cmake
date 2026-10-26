# Fails when a header uses a read-modify-write atomic, an atomic operator, a lock or thread_local (single
# writer per slot, no shared mutable state: CLAUDE.md rules 1 and 2). Allowed: atomic_ref / atomic plain
# load and store with relaxed, acquire or release order, and fences.
# Usage: cmake -DHEADER_DIR=<dir> -P CheckNoAtomicRmw.cmake    (every *.h under HEADER_DIR is checked)
#
# Each file is read whole and split into its real lines. Every character that CMake's list handling would treat
# specially ([ ] ; \) is neutralised first, so a bracket in a comment, a semicolon or a trailing backslash can
# never merge or split lines. The operator check is per identifier, over the whole file: every name declared
# as std::atomic<...> or std::atomic_ref<...> (a variable, member, reference or `auto x = std::atomic_ref<...>`)
# is collected, and ++, -- or a compound assignment applied to that name anywhere in the file fires, as does
# one applied directly to a std::atomic_ref<...>(...) temporary.
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
set(_id "[A-Za-z_][A-Za-z0-9_]*")
set(_op "(\\+\\+|--|\\+=|-=|\\*=|/=|%=|\\|=|&=|\\^=|<<=|>>=)")
# A declaration: std::atomic<...> name, std::atomic_ref<...>& name, ... (the template argument stops before
# ( ) { } = and the neutralised semicolon, so a cast like std::atomic_ref<T>(x) is not a declaration).
set(_decl "std::atomic(_ref)?<[^`(){}=]*>[ \t&*>]*(${_id})")
set(_decl_auto "(${_id})[ \t]*=[ \t]*std::atomic(_ref)?<")
set(_temp_op "std::atomic(_ref)?<[^`(){}=]*>[ \t]*\\([^`()]*\\)[ \t]*${_op}")
set(_temp_pre "(\\+\\+|--)[ \t]*std::atomic(_ref)?<")

file(GLOB_RECURSE _headers "${HEADER_DIR}/*.h")
if(NOT _headers)
    message(FATAL_ERROR "CheckNoAtomicRmw: no headers found under '${HEADER_DIR}'")
endif()
foreach(_h ${_headers})
    get_filename_component(_name "${_h}" NAME)
    file(READ "${_h}" _text)
    string(REPLACE "\\" "@BS@" _text "${_text}")
    string(REPLACE "[" "@LB@" _text "${_text}")
    string(REPLACE "]" "@RB@" _text "${_text}")
    string(REPLACE ";" "`" _text "${_text}")
    string(REPLACE "\r" "" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")

    set(_atomics "")
    foreach(_l IN LISTS _lines)
        foreach(_b ${_banned})
            if(_l MATCHES "${_b}")
                message(FATAL_ERROR "${_name}: read-modify-write atomic or lock is forbidden (${_b}): ${_l}")
            endif()
        endforeach()
        if(_l MATCHES "${_temp_op}" OR _l MATCHES "${_temp_pre}")
            message(FATAL_ERROR "${_name}: read-modify-write atomic or lock is forbidden (atomic operator): ${_l}")
        endif()
        string(REGEX MATCHALL "${_decl}" _ds "${_l}")
        foreach(_d IN LISTS _ds)
            if(_d MATCHES "${_decl}")
                list(APPEND _atomics "${CMAKE_MATCH_2}")
            endif()
        endforeach()
        string(REGEX MATCHALL "${_decl_auto}" _ds "${_l}")
        foreach(_d IN LISTS _ds)
            if(_d MATCHES "${_decl_auto}")
                list(APPEND _atomics "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endforeach()
    if(_atomics)
        list(REMOVE_DUPLICATES _atomics)
    endif()
    foreach(_a IN LISTS _atomics)
        foreach(_l IN LISTS _lines)
            if(_l MATCHES "(^|[^A-Za-z0-9_])${_a}[ \t]*${_op}" OR _l MATCHES "(\\+\\+|--)[ \t]*${_a}([^A-Za-z0-9_]|$)")
                message(FATAL_ERROR
                        "${_name}: read-modify-write atomic or lock is forbidden (atomic operator on ${_a}): ${_l}")
            endif()
        endforeach()
    endforeach()
endforeach()
