/**
 * @file lv_xml_compat.h
 *
 * Compat layer, not part of upstream lv_xml. Holds the engine's global state
 * that LVGL 9.4 kept in core/lv_global.h (LV_GLOBAL_DEFAULT()->xml_*). The
 * opaque typedefs live in src/misc/lv_types.h (shim).
 */
#ifndef LV_XML_COMPAT_H
#define LV_XML_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../../lvgl.h"

typedef struct {
    const char * xml_path_prefix;
    uint32_t lv_event_xml_store_timeline;
} lv_xml_compat_globals_t;

extern lv_xml_compat_globals_t lv_xml_compat_globals;

#ifdef __cplusplus
}
#endif

#endif /* LV_XML_COMPAT_H */
