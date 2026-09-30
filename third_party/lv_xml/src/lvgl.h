/**
 * @file lvgl.h
 *
 * Compat shim, not part of upstream lv_xml. In LVGL 9.4 the umbrella header
 * pulled in the XML engine; the vendored sources rely on that. Forwards to the
 * real 9.5 umbrella header and adds the XML public headers.
 */
#ifndef LV_XML_COMPAT_LVGL_H
#define LV_XML_COMPAT_LVGL_H

#include <lvgl.h>
#include "others/xml/lv_xml.h"

#endif /* LV_XML_COMPAT_LVGL_H */
