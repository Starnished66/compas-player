#ifndef TIMEZONE_LOCATION_H
#define TIMEZONE_LOCATION_H

#include <stdbool.h>
#include <stdint.h>

/* The embedded coordinates in timezone_location.c are derived from the
 * public-domain IANA tzdb /usr/share/zoneinfo/zone.tab and zone1970.tab;
 * verified same-place historical names are explicitly mapped by the
 * generator. Rule-sharing aliases with no specific coordinate are omitted. */

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the representative IANA location for a supported timezone ID.
 * Coordinates are signed millidegrees (for example, 9.9 degrees north is
 * 9900). False means the ID is unset, unsupported, or has no unique entry
 * in the IANA location tables; in that case output arguments are untouched. */
bool timezone_location_get(const char * iana_id,
                           int32_t * latitude_millidegrees,
                           int32_t * longitude_millidegrees);

#ifdef __cplusplus
}
#endif

#endif /* TIMEZONE_LOCATION_H */
