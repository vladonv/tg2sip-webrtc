#!/bin/bash
# Same bug class as buildenv/rename-webrtc-openssl-symbols.sh, one library
# over: ntgcalls' prebuilt WebRTC static library (libwebrtc.a) bundles its
# own copy of libsrtp (2.0.0) and exports its entire public API (srtp_create,
# srtp_crypto_policy_set_*, srtp_protect, ...) as GLOBAL symbols. PJSIP is
# built against - and bundles its own copy of - libsrtp 2.1.0
# (libsrtp-x86_64-unknown-linux-gnu.a). 2.1.0 added MKI (Master Key
# Identifier) support, which changed srtp_policy_t's layout versus 2.0.0.
# Since both copies export the *same* ~140 plain C symbol names, the linker
# keeps only one definition for the whole binary - confirmed via nm on a
# real build that PJSIP's 2.1.0 srtp_create wins (srtp_get_version_string
# returns "libsrtp 2.1.0" in the final tg2sip-webrtc binary). WebRTC's own
# SrtpSession code is compiled against 2.0.0's struct layout but ends up
# calling 2.1.0's srtp_create() with it - a real, confirmed cause of
# "Failed to init SRTP, err=2" (srtp_err_status_bad_param) immediately after
# a live call's DTLS handshake completes, RTP packets from the peer then
# silently dropped ("Inactive SRTP transport received an RTP packet").
#
# Unlike the OpenSSL/BoringSSL case, no "Part B" force-include header is
# needed here: ntgcalls' own source (wrtc/src, ntgcalls/src) never calls
# libsrtp directly (confirmed by grep - only the prebuilt libwebrtc.a uses
# it internally), so renaming the archive's symbols alone is sufficient -
# nothing else needs to be told about the new names.
#
# Usage: rename-webrtc-libsrtp-symbols.sh <path-to-libwebrtc.a> <path-to-pjsip-libsrtp.a>

set -euo pipefail

# See rename-webrtc-openssl-symbols.sh for why this must be C, not the box's
# default locale - comm -12 silently returns 0 matches otherwise.
export LC_ALL=C

LIBWEBRTC="${1:?usage: $0 <path-to-libwebrtc.a> <path-to-pjsip-libsrtp.a>}"
PJSIP_LIBSRTP="${2:?usage: $0 <path-to-libwebrtc.a> <path-to-pjsip-libsrtp.a>}"
# Deliberately a different prefix from rename-webrtc-openssl-symbols.sh's
# "ntgw_": that script's idempotency check reconstructs its symbol list by
# stripping "ntgw_" off every matching symbol already in the archive - reusing
# the same prefix here would make it pick up these libsrtp renames too and
# regenerate its (unrelated, OpenSSL-only) force-include header with ~140
# spurious extra #defines. Harmless in practice (ntgcalls' own code never
# calls libsrtp directly, so nothing would ever use those #defines), but a
# confusing cross-contamination between two otherwise-independent fixes.
RENAME_PREFIX="ntgsrtp_"

nm_output="$(nm "$LIBWEBRTC" 2>/dev/null)"
if ! grep -qE "^[0-9a-f]+ T srtp_create\$" <<< "$nm_output"; then
    echo "rename-webrtc-libsrtp-symbols: $LIBWEBRTC already patched (srtp_create no longer present under its original name), nothing to do"
    exit 0
fi

SYMLIST="$(mktemp)"
trap 'rm -f "$SYMLIST"' EXIT
comm -12 \
    <(awk '$2=="T"||$2=="D"||$2=="B"{print $3}' <<< "$nm_output" | sort -u) \
    <(nm "$PJSIP_LIBSRTP" 2>/dev/null | awk '$2=="T"||$2=="D"||$2=="B"{print $3}' | sort -u) \
    > "$SYMLIST"

count="$(wc -l < "$SYMLIST")"

# A real pinned ntgcalls release's bundled libsrtp shares on the order of
# ~140 symbol names with PJSIP's own libsrtp - 0 here almost certainly means
# the nm/comm pipeline broke rather than a genuine "nothing collides"
# result. Fail loudly instead of silently leaving the real bug in place.
if [ "$count" -eq 0 ]; then
    echo "rename-webrtc-libsrtp-symbols: 0 colliding symbols found between $LIBWEBRTC and $PJSIP_LIBSRTP - this is almost certainly a bug in this script, not a real result. Refusing to proceed." >&2
    exit 1
fi

echo "rename-webrtc-libsrtp-symbols: renaming $count colliding libsrtp symbol(s) in $LIBWEBRTC (prefixing with $RENAME_PREFIX)"
MAPFILE="$(mktemp)"
trap 'rm -f "$SYMLIST" "$MAPFILE"' EXIT
awk -v prefix="$RENAME_PREFIX" '{print $1, prefix $1}' "$SYMLIST" > "$MAPFILE"
objcopy --redefine-syms="$MAPFILE" "$LIBWEBRTC"
