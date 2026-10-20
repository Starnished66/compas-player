#include "import_web.h"
#include "remote_import_files.h"

/* Compatibility names retained for gui_network.c's established async
 * lifecycle. The session now only enables the PIN-protected native API. */
bool import_web_start(void) {
    return remote_import_files_start();
}

void import_web_stop(void) {
    remote_import_files_stop();
}
