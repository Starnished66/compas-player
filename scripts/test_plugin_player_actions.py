#!/usr/bin/env python3
"""Run the production player-action Lua bindings against real Lua."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'src/plugins/plugin_manager.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?(?:int|void|bool) ' + name + r'\([^;]*?\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError('missing production function ' + name)
    return match.group()

def pointer_function(name):
    match = re.search(r'^static plugin_instance_t \* ' + name + r'\([^\n]*\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError('missing production function ' + name)
    return match.group()

struct = re.search(r'^typedef struct \{\n    lua_State \* L;\n    char id\[64\];.*?^\} plugin_instance_t;', src, re.M | re.S)
# The real plugin_instance_t has many unrelated fields; mirror the fields used by these bindings.
assert struct
registry = re.search(r'^typedef struct \{\n    lua_State \* L;\n    int callback_ref;\n    char owner\[64\];\n    char action\[PLAYER_LAYOUT_ACTION_MAX\];\n\} plugin_player_action_t;\n\nstatic plugin_player_action_t plugin_player_actions\[PLUGIN_MAX_PLAYER_ACTIONS\];', src, re.M)
assert registry
functions = '\n'.join(function(name) for name in (
    'plugin_player_action_dispatch', 'plugin_player_action_name_valid',
    'plugin_player_actions_clear_owner', 'plugin_player_actions_clear_all',
    'plugin_set_player_layout_xml', 'l_plugin_register_player_action',
    'l_plugin_restore_player_layout'))

body = r'''
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define PLUGIN_MAX_PLAYER_ACTIONS 32
#define PLUGIN_MAX_FILES 4
#define PLAYER_LAYOUT_ACTION_MAX 64
#define LUA_NOREF (-2)
typedef struct { lua_State *L; char id[64]; bool defined,disposing,aborted; } plugin_instance_t;
static plugin_instance_t plugin_instances[4];
typedef void (*player_layout_action_handler_t)(const char *, const char *);
static void plugin_player_actions_clear_owner(lua_State *, const char *, bool);
static lua_State *plugin_main_state(lua_State *L);
static lua_State *plugin_main_state(lua_State *L) {
    lua_rawgeti(L,LUA_REGISTRYINDEX,LUA_RIDX_MAINTHREAD); lua_State *m=lua_tothread(L,-1); lua_pop(L,1); return m?m:L;
}
static int call_errors;
static int plugin_call(lua_State *L,int nargs,int nresults,int errfunc) {
    int rc=lua_pcall(L,nargs,nresults,errfunc); if(rc!=LUA_OK) call_errors++; return rc;
}
static player_layout_action_handler_t handler;
static char cleared_owners[8][64]; static int clear_count;
static char registered_layout[64], registered_path[128], selected_layout[64], bound_layout[64], bound_owner[64], saved_layout[64]="saved.layout";
static bool player_screen=true; static int reload_requests;
static void player_layouts_upsert(const char *id,const char *path){snprintf(registered_layout,64,"%s",id);snprintf(registered_path,128,"%s",path);bound_layout[0]='\0';bound_owner[0]='\0';}
static bool player_layouts_session_register_xml(const char *id,const char *name,const char *path,bool from_callback){(void)name;(void)from_callback;if(!id||!path||!id[0]||!path[0])return false;player_layouts_upsert(id,path);return true;}
static bool player_layouts_session_select_xml(const char *id,const char *name,const char *path,bool from_callback){char before[64];snprintf(before,64,"%s",selected_layout);if(!player_layouts_session_register_xml(id,name,path,from_callback))return false;snprintf(selected_layout,64,"%s",id);return strcmp(before,selected_layout)!=0;}
static bool player_layouts_set_plugin_owner(const char *id,const char *owner){if(strcmp(id,registered_layout))return false;snprintf(bound_layout,64,"%s",id);snprintf(bound_owner,64,"%s",owner);return true;}
static bool player_layouts_session_clear_selection(void){if(!selected_layout[0])return false;bool changed=strcmp(selected_layout,saved_layout)!=0;selected_layout[0]='\0';return changed;}
static bool player_layouts_session_clear_plugin_selection(const char *owner){if(!owner||!owner[0]||!selected_layout[0]||strcmp(selected_layout,bound_layout)||strcmp(owner,bound_owner))return false;return player_layouts_session_clear_selection();}
static void player_layouts_set_action_handler(player_layout_action_handler_t h){handler=h;}
static void player_layouts_clear_plugin_owner(const char *owner){snprintf(cleared_owners[clear_count++],64,"%s",owner);}
static bool gui_player_get_screen(void){return player_screen;}
static void gui_player_layout_reload_request(void){reload_requests++;}
''' + registry.group() + '\n' + pointer_function('plugin_instance_for_state') + '\n' + functions + r'''
static void register_fn(lua_State *L){lua_pushcfunction(L,l_plugin_register_player_action);lua_setglobal(L,"register_action");lua_pushcfunction(L,l_plugin_restore_player_layout);lua_setglobal(L,"restore_layout");}
static void run(lua_State *L,const char *s){int rc=luaL_dostring(L,s);if(rc!=LUA_OK)fprintf(stderr,"%s\n",lua_tostring(L,-1));assert(rc==LUA_OK);}
int main(void){
 lua_State *a=luaL_newstate(),*b=luaL_newstate(),*c=luaL_newstate();assert(a&&b&&c);luaL_openlibs(a);luaL_openlibs(b);luaL_openlibs(c);
 plugin_instances[0]=(plugin_instance_t){.L=a,.defined=true};strcpy(plugin_instances[0].id,"owner.a");
 plugin_instances[1]=(plugin_instance_t){.L=b,.defined=true};strcpy(plugin_instances[1].id,"owner.b");register_fn(a);register_fn(b);
 register_fn(c);run(c,"assert(not pcall(restore_layout))");
 bool changed=false;strcpy(selected_layout,"runtime.layout");
 assert(plugin_set_player_layout_xml(a,"runtime.layout","Runtime","/plugins/runtime.xml",false,&changed));
 assert(!changed&&!strcmp(bound_layout,"runtime.layout")&&!strcmp(bound_owner,"owner.a"));
 assert(plugin_set_player_layout_xml(b,"load.layout","Load","/plugins/load.xml",true,NULL));
 assert(!strcmp(bound_layout,"load.layout")&&!strcmp(bound_owner,"owner.b"));
 assert(plugin_set_player_layout_xml(a,"runtime.layout","Runtime","/plugins/runtime.xml",false,&changed));
 assert(!changed&&!strcmp(bound_layout,"runtime.layout")&&!strcmp(bound_owner,"owner.a"));
 run(b,"assert(not restore_layout())");assert(!strcmp(selected_layout,"runtime.layout")&&reload_requests==0);
 run(a,"assert(restore_layout())");assert(!selected_layout[0]&&reload_requests==1);
 run(a,"assert(not restore_layout())");
 plugin_instances[1].disposing=true;run(b,"assert(not pcall(restore_layout))");plugin_instances[1].disposing=false;
 player_screen=false;assert(plugin_set_player_layout_xml(a,"runtime.layout","Runtime","/plugins/runtime.xml",false,&changed));
 assert(changed);run(a,"assert(restore_layout())");assert(reload_requests==1&&!selected_layout[0]);player_screen=true;
 run(a,"hits=0; register_action('menu.open',function(action) assert(action=='menu.open'); hits=hits+1 end)");
 run(b,"hits=0; register_action('menu.open',function() hits=hits+10 end)");
 plugin_player_action_dispatch("owner.a","menu.open");plugin_player_action_dispatch("owner.b","menu.open");
 run(a,"assert(hits==1)");run(b,"assert(hits==10)");
 run(a,"register_action('menu.open',function(action) assert(action=='menu.open'); hits=hits+2 end)");plugin_player_action_dispatch("owner.a","menu.open");run(a,"assert(hits==3)");
 run(a,"register_action('menu.open',function() error('expected callback failure') end)");plugin_player_action_dispatch("owner.a","menu.open");assert(call_errors==1);plugin_player_action_dispatch("owner.b","menu.open");run(b,"assert(hits==20)");
 run(a,"register_action('menu.open',function() register_action('menu.open',function() hits=hits+4 end); hits=hits+1 end)");plugin_player_action_dispatch("owner.a","menu.open");plugin_player_action_dispatch("owner.a","menu.open");run(a,"assert(hits==8)");
 run(a,"assert(not pcall(register_action,'library',function() end)); assert(not pcall(register_action,'none',function() end)); assert(not pcall(register_action,'bad/name',function() end)); assert(not pcall(register_action,'',function() end)); assert(not pcall(register_action,string.rep('x',64),function() end)); assert(not pcall(register_action,'valid',true))");
 run(a,"register_action('x',function() end)");run(b,"register_action('y',function() end)");
 plugin_player_actions_clear_owner(a,"owner.a",false);assert(clear_count==1&&!strcmp(cleared_owners[0],"owner.a"));
 plugin_player_action_dispatch("owner.a","menu.open");plugin_player_action_dispatch("owner.b","menu.open");run(b,"assert(hits==30)");
 assert(plugin_player_actions[0].L==NULL || strcmp(plugin_player_actions[0].owner,"owner.b")!=0);
 plugin_instances[1].aborted=true;plugin_player_action_dispatch("owner.b","menu.open");assert(clear_count==2&&!strcmp(cleared_owners[1],"owner.b"));
 plugin_instances[0].aborted=true;plugin_player_actions_clear_all();assert(handler==NULL);
 for(int i=0;i<PLUGIN_MAX_PLAYER_ACTIONS;i++)assert(plugin_player_actions[i].L==NULL);
 assert(clear_count==2); /* clear_all invalidates handler; owner clears happen in deinit integration */
 lua_close(a);lua_close(b);lua_close(c);puts("plugin player-action bindings passed");return 0;
}
'''

# Verify production registration and lifecycle wiring, including the callback API table.
assert '{ "register_player_action",    l_plugin_register_player_action }' in src
assert '{ "restore_player_layout",     l_plugin_restore_player_layout }' in src
assert '"ui.player_actions"' in src
assert '"ui.player_layout_restore"' in src
assert 'plugin_player_actions_clear_owner(L, failed_inst && failed_inst->defined ? failed_inst->id : NULL, aborted);' in function('discard_failed_plugin_resources')
assert 'plugin_player_actions_clear_all();' in function('plugin_manager_deinit')
assert src.index('plugin_player_actions_clear_all();', src.index('void plugin_manager_deinit')) < src.index('lua_close(plugin_instances[i].L)', src.index('void plugin_manager_deinit'))
with tempfile.TemporaryDirectory(prefix='compas-player-actions-') as folder:
    folder = pathlib.Path(folder); c = folder / 'test.c'; binary = folder / 'test'; c.write_text(body)
    lua = [str(p) for p in (root / 'lua/src').glob('*.c') if p.name not in ('lua.c','luac.c')]
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-DLUA_USE_LINUX','-I'+str(root/'lua/src'),str(c),*lua,'-lm','-ldl','-o',str(binary)],check=True,cwd=root)
    subprocess.run([str(binary)],check=True,cwd=root)
