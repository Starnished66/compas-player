#ifndef REMOTE_PLUGIN_STORE_H
#define REMOTE_PLUGIN_STORE_H
#include "remote_plugins.h"
/* UI thread only. Uses the native verified catalog/download/install worker.
 * Installation does not reload plugins: clients explicitly request refresh
 * after the store job reaches a terminal state. */
void remote_plugin_store_execute(const remote_plugins_job_t * job, remote_plugins_result_t * result);
#endif
