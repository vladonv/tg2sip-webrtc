/* pytgcalls' prebuilt static libgio-2.0.a references the legacy BIND
 * resolver API (__dn_expand/__res_nquery) for a DNS TXT/SRV lookup path
 * (g_resolver_records_from_res_query) that ntgcalls never exercises for a
 * plain P2P call. Modern glibc (>=2.34, merged NSS) dropped these exact
 * symbol names from libresolv.a on this system - stub them out rather than
 * dragging in an older libresolv or patching the prebuilt glib archive. */
#include <errno.h>

int __dn_expand(void) {
    errno = ENOSYS;
    return -1;
}

int __res_nquery(void) {
    errno = ENOSYS;
    return -1;
}
