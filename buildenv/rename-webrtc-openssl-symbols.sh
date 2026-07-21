#!/bin/bash
# ntgcalls' prebuilt WebRTC static library (third_party/ntgcalls's
# deps/libwebrtc/src/lib/libwebrtc.a, fetched by ntgcalls' own CMake at
# configure time) bundles BoringSSL and exports its entire OpenSSL-API-
# compatible surface (EVP_sha256, SHA256_Init, HMAC_Init_ex, EVP_PKEY_free,
# ~1900 names in total) as GLOBAL symbols. tg2sip-webrtc also links TDLib
# and PJSIP, both built against - and expecting - the system's real
# OpenSSL (libssl.so.3/libcrypto.so.3). Since BoringSSL and OpenSSL 3.x use
# different internal struct layouts (EVP_MD, HMAC_CTX, SHA256_CTX,
# EVP_PKEY, ...) behind the same public C function names, a strong GLOBAL
# definition of any of these names anywhere in the final link - or even
# just present in the main executable's own symbol table, since ELF's
# default symbol scope lets the main executable interpose symbols for
# every shared library it loads - silently redirects one side's calls into
# the other's differently-shaped implementation. Confirmed real, not
# hypothetical, three separate times now:
#   1. TDLib's binlog key derivation (td::pbkdf2_impl's
#      CHECK(dest.size() == hash_size)).
#   2. PJSIP/libsrtp's HMAC_CTX (glibc "buffer overflow detected" via
#      __fortify_fail inside HMAC_Init_ex).
#   3. WebRTC's *own* internal DTLS certificate generation
#      (webrtc::BoringSSLIdentity, needs BoringSSL's own
#      EVP_PKEY_free/ENGINE_finish, self-consistently, across multiple
#      separate .o files within libwebrtc.a) - found after an earlier
#      version of this fix (objcopy --localize-symbols, converting
#      BoringSSL's colliding exports from GLOBAL to LOCAL binding) fixed
#      #1/#2 but broke this: a LOCAL symbol is only visible within the .o
#      file that defines it, so localizing broke every OTHER .o file's
#      reference to it - and instead of a loud link error, the linker
#      silently re-resolved those now-broken internal references against
#      the closest available REAL symbol, real system libcrypto.so.3
#      pulled in for TDLib/PJSIP's sake. Net effect: WebRTC's own
#      BoringSSL-allocated EVP_PKEY/certificate objects got passed to
#      *real* OpenSSL's EVP_PKEY_free/ENGINE_finish - the exact same
#      type-confusion bug, just hitting WebRTC's own DTLS setup instead
#      (segfault inside libcrypto.so.3, confirmed via gdb/coredumpctl
#      during a real live call).
#
# Correct fix has two matched halves, both needed:
#
#   A. objcopy --redefine-syms on libwebrtc.a itself: rename every
#      colliding symbol to a private, unambiguous name (ntgw_<original>)
#      *consistently across the whole archive*. This preserves WebRTC's
#      own internal cross-.o calls (both the definition and every
#      reference get renamed together, so they still resolve to each
#      other, just under the new name), while completely removing the
#      original OpenSSL-API names from libwebrtc.a's symbol table.
#
#   B. A force-included header (#define <name> ntgw_<name> for every
#      colliding symbol) applied to ntgcalls' *own* source
#      (target_compile_options(... -include ...) on the wrtc/
#      ntgcalls-native CMake targets, see the ntgcalls add_subdirectory
#      block in the root CMakeLists.txt). ntgcalls' own code
#      (wrtc/src/utils/encryption.cpp, wrtc/src/utils/random.cpp - the
#      P2P call's actual key-derivation/AES-CTR encryption path) calls
#      these same raw BoringSSL functions *directly*, expecting
#      BoringSSL's own struct layouts (SHA256_CTX, AES_KEY, ...) since
#      it's compiled against BoringSSL's own headers. Without B, A alone
#      just moves the exact same collision one level out: ntgcalls' own
#      encryption code would then be the one silently resolving to real
#      OpenSSL's differently-shaped structs instead of BoringSSL's - on
#      the literal encryption path for call media, a worse place for a
#      layout mismatch to hide than a crash. The #define approach is
#      textual/preprocessor-level, so it also catches any indirect/inline
#      header usage a plain grep for direct call sites might miss - more
#      robust than hand-patching the two files found so far.
#
# Both halves rename to *exactly* the same ntgw_<original> names, computed
# from the same symbol list, so they line up: object code compiled against
# the header's macros calls ntgw_<original>, which the archive now defines
# under that exact name.
#
# Computed against the *build machine's* real libcrypto.so.3/libssl.so.3
# (not a committed static list) so this automatically tracks whatever
# OpenSSL version is actually present, rather than potentially drifting
# from it on a newer/older distro.
#
# Usage: rename-webrtc-openssl-symbols.sh <path-to-libwebrtc.a> <path-to-output-header>

set -euo pipefail

# comm requires both inputs sorted in the exact same collation order; under
# any locale other than C (e.g. this box's default uk_UA.UTF-8), sort's
# locale-aware collation silently disagrees with itself often enough that
# comm -12 can find zero matches even though the same symbol names are
# genuinely present in both inputs - a real, confirmed failure mode here
# (not just theoretical), not just a byte-order nicety.
export LC_ALL=C

LIBWEBRTC="${1:?usage: $0 <path-to-libwebrtc.a> <path-to-output-header>}"
OUT_HEADER="${2:?usage: $0 <path-to-libwebrtc.a> <path-to-output-header>}"
RENAME_PREFIX="ntgw_"

SYMLIST="$(mktemp)"
trap 'rm -f "$SYMLIST"' EXIT

# Idempotency: if a previous run already renamed the archive, its original
# GLOBAL OpenSSL-API symbols are gone (now ntgw_-prefixed), so re-running
# the comm-against-real-OpenSSL computation below would legitimately find
# 0 matches - expected, not a bug, in this specific branch. Reconstruct
# the same symbol list (for the header, which must still be regenerated
# every run) by stripping the prefix back off the archive's already-
# renamed symbols instead of recomputing it from scratch.
nm_output="$(nm "$LIBWEBRTC" 2>/dev/null)"
already_patched=0
if ! grep -qE "^[0-9a-f]+ T EVP_sha256\$" <<< "$nm_output"; then
    already_patched=1
fi

if [ "$already_patched" -eq 1 ]; then
    awk -v prefix="$RENAME_PREFIX" -v plen="${#RENAME_PREFIX}" \
        '$2=="T" && index($3, prefix)==1 {print substr($3, plen+1)}' \
        <<< "$nm_output" | sort -u > "$SYMLIST"
else
    comm -12 \
        <(awk '$2=="T"{print $3}' <<< "$nm_output" | sort -u) \
        <( { nm -D /lib/x86_64-linux-gnu/libcrypto.so.3 2>/dev/null; nm -D /lib/x86_64-linux-gnu/libssl.so.3 2>/dev/null; } \
            | awk '$2=="T"{sub(/@.*/,"",$3); print $3}' | sort -u) \
        > "$SYMLIST"
fi

count="$(wc -l < "$SYMLIST")"

# A real, pinned ntgcalls release's BoringSSL shares on the order of ~1900
# symbol names with system OpenSSL - 0 here almost certainly means the
# nm/comm pipeline broke (wrong nm output format, empty/missing input,
# locale regression) rather than a genuine "nothing collides" result. Fail
# loudly instead of silently no-op'ing and leaving the real bug this
# script exists to fix back in place.
if [ "$count" -eq 0 ]; then
    echo "rename-webrtc-openssl-symbols: 0 colliding symbols found in $LIBWEBRTC (already_patched=$already_patched) - this is almost certainly a bug in this script, not a real result. Refusing to proceed." >&2
    exit 1
fi

# Part B: (re)generate the force-include header unconditionally - cheap,
# deterministic, and needs to exist/be current every configure regardless
# of whether the archive itself still needs patching below.
{
    echo "// Generated by buildenv/rename-webrtc-openssl-symbols.sh - do not edit."
    echo "// Force-included into ntgcalls' own sources (wrtc/ntgcalls-native CMake"
    echo "// targets only) so their direct calls into BoringSSL's raw crypto API"
    echo "// resolve to the same private names libwebrtc.a's own symbols were"
    echo "// renamed to below, instead of colliding with TDLib's/PJSIP's separate,"
    echo "// correct use of real system OpenSSL under the original names."
    echo "#pragma once"
    awk -v prefix="$RENAME_PREFIX" '{print "#define " $1 " " prefix $1}' "$SYMLIST"
} > "$OUT_HEADER"
echo "rename-webrtc-openssl-symbols: wrote $count symbol #define(s) to $OUT_HEADER"

# Part A: patch the archive itself - skip if already done (checked above).
if [ "$already_patched" -eq 1 ]; then
    echo "rename-webrtc-openssl-symbols: $LIBWEBRTC already patched (EVP_sha256 no longer present under its original name), nothing to do"
    exit 0
fi

echo "rename-webrtc-openssl-symbols: renaming $count colliding OpenSSL-API symbol(s) in $LIBWEBRTC (prefixing with $RENAME_PREFIX)"
MAPFILE="$(mktemp)"
trap 'rm -f "$SYMLIST" "$MAPFILE"' EXIT
awk -v prefix="$RENAME_PREFIX" '{print $1, prefix $1}' "$SYMLIST" > "$MAPFILE"
objcopy --redefine-syms="$MAPFILE" "$LIBWEBRTC"
