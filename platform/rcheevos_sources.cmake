# Shared rcheevos build definition.
#
# M2 deliberately builds without RC_CLIENT_SUPPORTS_HASH: the Vita side already
# computes the PSP hash with tools/psp_hash.py-compatible logic and passes the
# precomputed hash to rc_client_begin_load_game().

set(RA_RCHEEVOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/rcheevos")
set(RA_RCHEEVOS_INCLUDE_DIRS
    "${RA_RCHEEVOS_ROOT}/include"
    "${RA_RCHEEVOS_ROOT}/src"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos"
    "${RA_RCHEEVOS_ROOT}/src/rapi"
    "${RA_RCHEEVOS_ROOT}/src/rhash"
)

set(RA_RCHEEVOS_SOURCES
    "${RA_RCHEEVOS_ROOT}/src/rc_compat.c"
    "${RA_RCHEEVOS_ROOT}/src/rc_client.c"
    "${RA_RCHEEVOS_ROOT}/src/rc_util.c"
    "${RA_RCHEEVOS_ROOT}/src/rc_version.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/alloc.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/condition.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/condset.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/consoleinfo.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/format.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/lboard.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/memref.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/operand.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/rc_validate.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/richpresence.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/runtime.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/runtime_progress.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/trigger.c"
    "${RA_RCHEEVOS_ROOT}/src/rcheevos/value.c"
    "${RA_RCHEEVOS_ROOT}/src/rhash/md5.c"
    "${RA_RCHEEVOS_ROOT}/src/rhash/hash.c"
    "${RA_RCHEEVOS_ROOT}/src/rhash/hash_disc.c"
    "${RA_RCHEEVOS_ROOT}/src/rhash/cdreader.c"
    "${RA_RCHEEVOS_ROOT}/src/rapi/rc_api_common.c"
    "${RA_RCHEEVOS_ROOT}/src/rapi/rc_api_editor.c"
    "${RA_RCHEEVOS_ROOT}/src/rapi/rc_api_info.c"
    "${RA_RCHEEVOS_ROOT}/src/rapi/rc_api_runtime.c"
    "${RA_RCHEEVOS_ROOT}/src/rapi/rc_api_user.c"
)

# rc_client has no internal worker threads. All server callbacks must either
# complete synchronously or be driven by the caller's own event loop.
# Only the PSP disc hasher is needed; ROM/ZIP/encrypted hashers are excluded
# from the Vita build to keep the user module small.
set(RA_RCHEEVOS_DEFINITIONS RC_NO_THREADS RC_HASH_NO_ROM RC_HASH_NO_ZIP RC_HASH_NO_ENCRYPTED)
