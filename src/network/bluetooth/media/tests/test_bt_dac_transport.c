#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <dbus/dbus.h>
#include "src/network/bluetooth/media/bt_dac_transport.c"

typedef struct node { int type; const char *text; dbus_bool_t boolean; struct node *child, *next; } node;
struct DBusMessage { const char *method; node *root; };
static struct DBusMessage request, objects_reply, status_reply;
static node connected_value = { .type=DBUS_TYPE_BOOLEAN, .boolean=true };
static node player_value = { .type=DBUS_TYPE_OBJECT_PATH, .text="/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/player2" };
static node status_value = { .type=DBUS_TYPE_STRING, .text="playing" };
static char sent[64];
static int calls;
static bool cancel_during_discovery, cancel_during_status, missing_status, retarget_during_discovery;
static bt_dac_stream_info_t stream_info;
void bt_control_get_dac_stream_info(bt_dac_stream_info_t *out) { *out = stream_info; }

static void fixtures(void) {
    static node connected_variant, connected_key, connected_entry, player_variant, player_key, player_entry;
    static node props, interface_key, interface_entry, interfaces, object_key, object_entry, objects, status;
    connected_variant=(node){.type=DBUS_TYPE_VARIANT,.child=&connected_value};
    connected_key=(node){.type=DBUS_TYPE_STRING,.text="Connected",.next=&connected_variant};
    connected_entry=(node){.type=DBUS_TYPE_DICT_ENTRY,.child=&connected_key,.next=&player_entry};
    player_variant=(node){.type=DBUS_TYPE_VARIANT,.child=&player_value};
    player_key=(node){.type=DBUS_TYPE_STRING,.text="Player",.next=&player_variant};
    player_entry=(node){.type=DBUS_TYPE_DICT_ENTRY,.child=&player_key};
    props=(node){.type=DBUS_TYPE_ARRAY,.child=&connected_entry};
    interface_key=(node){.type=DBUS_TYPE_STRING,.text="org.bluez.MediaControl1",.next=&props};
    interface_entry=(node){.type=DBUS_TYPE_DICT_ENTRY,.child=&interface_key};
    interfaces=(node){.type=DBUS_TYPE_ARRAY,.child=&interface_entry};
    object_key=(node){.type=DBUS_TYPE_OBJECT_PATH,.text="/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",.next=&interfaces};
    object_entry=(node){.type=DBUS_TYPE_DICT_ENTRY,.child=&object_key};
    objects=(node){.type=DBUS_TYPE_ARRAY,.child=&object_entry};
    status=(node){.type=DBUS_TYPE_VARIANT,.child=&status_value};
    objects_reply.root=&objects; status_reply.root=&status;
    sent[0]=0; calls=0; cancel_during_discovery=false; cancel_during_status=false; missing_status=false;
    connected_value.boolean=true; player_value.text="/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/player2";
    status_value.text="playing"; status_value.type=DBUS_TYPE_STRING;
    stream_info = (bt_dac_stream_info_t){0};
    bt_media_player_set_phone_target("AA:BB:CC:DD:EE:FF");
    retarget_during_discovery = false;
    bt_media_player_set_phone_control_enabled(true);
}
dbus_bool_t dbus_message_iter_init(DBusMessage *m, DBusMessageIter *i) { i->dummy1=m->root; return m->root != NULL; }
int dbus_message_iter_get_arg_type(DBusMessageIter *i) { node *n=i->dummy1; return n ? n->type : DBUS_TYPE_INVALID; }
void dbus_message_iter_recurse(DBusMessageIter *i, DBusMessageIter *out) { out->dummy1=((node*)i->dummy1)->child; }
dbus_bool_t dbus_message_iter_next(DBusMessageIter *i) { i->dummy1=((node*)i->dummy1)->next; return i->dummy1 != NULL; }
void dbus_message_iter_get_basic(DBusMessageIter *i, void *out) {
    node *n=i->dummy1;
    if(n->type==DBUS_TYPE_BOOLEAN) *(dbus_bool_t*)out=n->boolean;
    else *(const char**)out=n->text;
}
DBusMessage *dbus_message_new_method_call(const char *d,const char *p,const char *i,const char *m) {
    (void)d;(void)i;
    if(!strcmp(m,"Next") || !strcmp(m,"Previous") || !strcmp(m,"Play") || !strcmp(m,"Pause"))
        assert(!strcmp(p,"/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/player2"));
    request.method=m; return &request;
}
dbus_bool_t dbus_message_append_args(DBusMessage *m,int type,...) { (void)m;(void)type;return true; }
DBusMessage *dbus_connection_send_with_reply_and_block(DBusConnection *c,DBusMessage *m,int timeout,DBusError *err) {
    (void)c;(void)err;assert(timeout==1000);
    if(!strcmp(m->method,"GetManagedObjects")) {
        if(cancel_during_discovery) bt_media_player_set_phone_control_enabled(false);
        if(retarget_during_discovery) bt_media_player_set_phone_target("11:22:33:44:55:66");
        return &objects_reply;
    }
    if(!strcmp(m->method,"Get")) {
        if(cancel_during_status) bt_media_player_set_phone_control_enabled(false);
        return missing_status ? NULL : &status_reply;
    }
    snprintf(sent,sizeof(sent),"%s",m->method);calls++;return NULL;
}
void dbus_message_unref(DBusMessage *m) { (void)m; }
DBusConnection *dbus_bus_get_private(DBusBusType t,DBusError *e) { (void)t;(void)e;return (DBusConnection*)1; }
void dbus_connection_set_exit_on_disconnect(DBusConnection *c,dbus_bool_t b) { (void)c;(void)b; }
void dbus_connection_close(DBusConnection *c) { (void)c; }
void dbus_connection_unref(DBusConnection *c) { (void)c; }
static void command(int direction,const char *method) {
    bt_media_player_control_phone(direction);bt_media_player_dispatch_phone_command();
    assert(!strcmp(sent,method));sent[0]=0;
}
int main(void) {
    fixtures();command(1,"Next");command(-1,"Previous");command(0,"Pause");
    status_value.text="paused";command(0,"Play");
    status_value.text="stopped";command(0,"Play");
    status_value.text="unknown";command(0,"Play");
    status_value.type=DBUS_TYPE_BOOLEAN;command(0,"Play");
    fixtures();missing_status=true;command(0,"Play");
    stream_info.running=true;snprintf(stream_info.mac,sizeof(stream_info.mac),"AA:BB:CC:DD:EE:FF");command(0,"Pause");
    snprintf(stream_info.mac,sizeof(stream_info.mac),"11:22:33:44:55:66");command(0,"Play");
    fixtures();bt_media_player_set_phone_target("11:22:33:44:55:66");command(1,"");
    fixtures();retarget_during_discovery=true;command(1,"");
    fixtures();bt_media_player_control_phone(1);bt_media_player_set_phone_target(NULL);
    bt_media_player_dispatch_phone_command();assert(calls==0);
    command(1,"");
    fixtures();bt_media_player_control_phone(1);bt_media_player_set_phone_target("11:22:33:44:55:66");
    bt_media_player_set_phone_target("AA:BB:CC:DD:EE:FF");bt_media_player_dispatch_phone_command();assert(calls==0);
    fixtures();cancel_during_status=true;command(0,"");
    fixtures();player_value.text="/";command(1,"");
    fixtures();connected_value.boolean=false;command(1,"");
    fixtures();cancel_during_discovery=true;command(1,"");
    fixtures();bt_media_player_control_phone(1);bt_media_player_set_phone_control_enabled(false);
    bt_media_player_dispatch_phone_command();assert(calls==0);
    fixtures();bt_media_player_control_phone(-1);bt_media_player_control_phone(1);
    bt_media_player_dispatch_phone_command();assert(!strcmp(sent,"Previous"));
    bt_media_player_dispatch_phone_command();assert(!strcmp(sent,"Next"));
    fixtures();for(int i=0;i<100;i++)bt_media_player_control_phone(1);
    for(int i=0;i<100;i++)bt_media_player_dispatch_phone_command();
    assert(calls==PHONE_COMMAND_CAPACITY);
    puts("Bluetooth DAC AVRCP tests passed");
}
