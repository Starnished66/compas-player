#include "lvgl.h"
#include "lv_xml.h"
#include "lv_xml_component_private.h"
#include <assert.h>
#include <stdio.h>

static void reject_gradient(const char * name, const char * gradient) {
    char xml[512];
    snprintf(xml, sizeof(xml), "<component><gradients>%s</gradients><view><lv_obj/></view></component>", gradient);
    assert(lv_xml_register_component_from_data(name, xml) == LV_RESULT_INVALID);
    assert(lv_xml_component_get_scope(name) == NULL);
}

int main(void) {
    lv_init();
    lv_xml_init();
    reject_gradient("test.gradient_missing", "<linear name=\"broken\"/>");
    reject_gradient("test.gradient_empty", "<linear name=\"broken\" start=\"\" end=\"0 100\"/>");
    reject_gradient("test.gradient_single", "<linear name=\"broken\" start=\"0\" end=\"0 100\"/>");
    reject_gradient("test.radial_single", "<radial name=\"broken\" center=\"50\"/>");
    reject_gradient("test.conical_empty", "<conical name=\"broken\" angle=\" \"/>");

    const char * radius_only =
        "<component><gradients><radial name=\"radius_only\" center=\"10 20\" radius=\"5\"/>"
        "</gradients><view><lv_obj/></view></component>";
    assert(lv_xml_register_component_from_data("test.radius_only", radius_only) == LV_RESULT_OK);
    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("test.radius_only");
    assert(scope);
    lv_xml_grad_t * grad = lv_ll_get_head(&scope->gradient_ll);
    assert(grad && grad->grad_dsc.params.radial.end_extent.x == 15 &&
           grad->grad_dsc.params.radial.end_extent.y == 20);
    assert(lv_xml_component_unregister("test.radius_only") == LV_RESULT_OK);
    puts("PASS malformed gradient rejection and radial radius-only parsing");
    return 0;
}
