/**
 * @file lv_types.h
 *
 * Compat shim, not part of upstream lv_xml. The vendored XML sources include
 * "../../misc/lv_types.h", which resolves here first. It forwards to the real
 * LVGL header and adds the opaque XML typedefs that LVGL 9.4 declared in its
 * own lv_types.h but 9.5 no longer does.
 */
#ifndef LV_XML_COMPAT_TYPES_H
#define LV_XML_COMPAT_TYPES_H

#include <src/misc/lv_types.h>

typedef struct _lv_xml_component_scope_t lv_xml_component_scope_t;
typedef struct _lv_xml_parser_state_t lv_xml_parser_state_t;
typedef struct _lv_xml_load_t lv_xml_load_t;

#endif /* LV_XML_COMPAT_TYPES_H */
