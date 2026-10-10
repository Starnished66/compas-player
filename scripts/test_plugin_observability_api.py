#!/usr/bin/env python3
import pathlib, re, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
src=(root/'src/plugins/plugin_manager.c').read_text()
names=['l_plugin_get_eq_state','l_plugin_eq_apply_state','l_plugin_get_track_metadata','l_plugin_register_list_item','l_plugin_show_list','l_plugin_is_list_showing']
funcs=[]
for name in names:
 m=re.search(r'^static int '+name+r'\([^\n]*\) \{.*?^\}',src,re.M|re.S)
 if not m: raise RuntimeError('missing '+name)
 funcs.append(m.group())
validator=re.search(r'^static bool plugin_song_path_valid\([^\n]*\) \{.*?^\}',src,re.M|re.S)
if not validator: raise RuntimeError('missing SD validator')
header=(root/'src/plugins/plugin_manager.h').read_text()
limits='\n'.join(re.findall(r'^#define PLUGIN_MAX_[A-Z_]+_LIST_ITEMS \d+',header,re.M))
item=re.search(r'typedef struct \{[^{}]+\} plugin_list_item_t;',src).group()
a=src.index('typedef enum {\n    PLUGIN_LIST_TARGET_BOOKS')
b=src.index('static int plugin_list_item_counts',a)
b=src.index(';',b)+1
registry=src[a:b]
append=re.search(r'^static void append_list_item\([^\n]*\) \{.*?^\}',src,re.M|re.S).group()
main_state=re.search(r'^static lua_State \* plugin_main_state\([^\n]*\) \{.*?^\}',src,re.M|re.S).group()
start=src.index('typedef struct {\n    lua_State * L;\n    int select_ref;')
end=src.index('static lua_Integer plugin_list_next_handle = 1;', start)+len('static lua_Integer plugin_list_next_handle = 1;')
list_callbacks=src[start:end]
gui=(root/'src/ui/gui_plugins.c').read_text()
click=re.search(r'^static void plugin_list_row_click_cb\([^\n]*\) \{.*?^\}',gui,re.M|re.S).group()
registry_support=limits+'\n'+item+'\n'+registry+'\n#define PLUGIN_LIST_SCREEN_POOL_SIZE 4\n#define PLUGIN_MAX_LIST_ITEMS 500\n'+main_state+'\n'+list_callbacks+r'''
static int top_slot=-1, next_slot, navigation_pushes, renders;
static int plugin_list_selected_indices[4];
static lua_State* click_vm;
#define LV_EVENT_CLICKED 1
typedef struct {int code;intptr_t data;} lv_event_t;
static int lv_event_get_code(lv_event_t*e){return e->code;}
static void* lv_event_get_user_data(lv_event_t*e){return (void*)e->data;}
static void plugin_list_apply_selection(int slot,int selected){plugin_list_selected_indices[slot]=selected;}
static void plugin_manager_list_item_selected(int slot,int index){(void)slot;(void)index;assert(luaL_dostring(click_vm,"h=assert(show_list('new page',{'new'},function()end,{replace=h,selected=1}))")==LUA_OK);}
static bool gui_plugin_list_is_top(int slot){return slot>=0&&slot<4&&top_slot==slot;}
static int gui_plugin_show_list(const char*t,const char*const*l,const char*const*i,const char*const*z,const bool*w,int32_t h,int32_t width,int selected,int n,int columns,int replace_slot){
(void)t;(void)l;(void)i;(void)z;(void)w;(void)h;(void)width;(void)selected;(void)n;(void)columns;
int slot=replace_slot;
if(slot>=0){if(!gui_plugin_list_is_top(slot))return -1;}
else{slot=next_slot++%4;navigation_pushes++;}
top_slot=slot;plugin_list_selected_indices[slot]=selected;renders++;return slot;}
'''+r'''
static void utf8_truncate_safe(char*d,const char*s,size_t n){snprintf(d,n,"%s",s);}
static void utf8_sanitize(char*s){(void)s;}
static bool is_valid_text_size(const char*s){return !strcmp(s,"small")||!strcmp(s,"medium")||!strcmp(s,"large")||!strcmp(s,"mono");}
'''+append
with tempfile.TemporaryDirectory(prefix='compas-plugin-observability-') as td:
 c=pathlib.Path(td)/'test.c'
 c.write_text(r'''#define _XOPEN_SOURCE 700
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "peq.h"
#include "metadata_db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#define MUSIC_ROOT_DIR "./music"
static peq_band_t bands[PEQ_NUM_BANDS]; static bool bypass; static double preamp,width=1.0; static int writes;
static song_row_t expected; static int lookups;
static bool plugin_storage_path_is_reserved(const char*p){(void)p;return false;}
bool metadata_db_get_song_by_path(const char*p,song_row_t*out){lookups++;if(strcmp(p,expected.path))return false;*out=expected;return true;}
bool peq_get_bypass(void){return bypass;} void peq_set_bypass(bool v){bypass=v;}
double peq_get_preamp_db(void){return preamp;} void peq_set_preamp_db(double v){preamp=v;}
double peq_get_stereo_width(void){return width;} void peq_set_stereo_width(double v){width=v;}
const peq_band_t* peq_get_band(int i){return &bands[i];}
void peq_set_band(int i,double f,double g,double q){bands[i].freq_hz=f;bands[i].gain_db=g;bands[i].q=q;}
void peq_set_band_type(int i,peq_band_type_t t){bands[i].type=t;}
void peq_set_band_enabled(int i,bool e){bands[i].enabled=e;}
void peq_save(void){writes++;}
'''+registry_support+click+validator.group()+'\n'.join(funcs)+r'''
int main(void){lua_State*L=luaL_newstate();luaL_openlibs(L);
lua_pushcfunction(L,l_plugin_show_list);lua_setglobal(L,"show_list");lua_pushcfunction(L,l_plugin_is_list_showing);lua_setglobal(L,"is_list_showing");
assert(luaL_dostring(L,"h=show_list('page',{'first'},function()end); assert(is_list_showing(h)); old=h; for i=1,20 do h=assert(show_list('page '..i,{'row'},function()end,{replace=h})); assert(not is_list_showing(old)); old=h end")==LUA_OK);
assert(navigation_pushes==1 && renders==21);
assert(luaL_dostring(L,"assert(show_list('late',{},function()end,{replace=1})==nil)")==LUA_OK);assert(renders==21);
assert(luaL_dostring(L,"local co=coroutine.create(function() local h=show_list('from coroutine',{},function()end); assert(is_list_showing(h)); local next=show_list('replace from coroutine',{},function()end,{replace=h}); assert(next>h and is_list_showing(next)) end); assert(coroutine.resume(co))")==LUA_OK);assert(renders==23);
lua_State*foreign=luaL_newstate();luaL_openlibs(foreign);lua_pushcfunction(foreign,l_plugin_show_list);lua_setglobal(foreign,"show_list");lua_pushinteger(foreign,plugin_list_callbacks[top_slot].handle);lua_setglobal(foreign,"h");assert(luaL_dostring(foreign,"assert(show_list('foreign',{},function()end,{replace=h})==nil)")==LUA_OK);lua_close(foreign);assert(renders==23);
top_slot=-1;assert(luaL_dostring(L,"assert(show_list('closed',{},function()end,{replace=h})==nil)")==LUA_OK);assert(renders==23);
lua_pushcfunction(L,l_plugin_register_list_item);lua_setglobal(L,"register_list_item");
assert(luaL_dostring(L,"register_list_item('music_controls','sentinel',function()end); for i=1,16 do register_list_item('playback','p'..i,function()end); register_list_item('music_audio','s'..i,function()end) end; assert(not pcall(register_list_item,'playback','overflow',function()end)); assert(not pcall(register_list_item,'music_audio','overflow',function()end))")==LUA_OK);
assert(plugin_list_item_counts[PLUGIN_LIST_TARGET_PLAYBACK]==16 && plugin_list_item_counts[PLUGIN_LIST_TARGET_MUSIC_AUDIO]==16);
assert(!strcmp(plugin_list_items[PLUGIN_LIST_TARGET_PLAYBACK][15].label,"p16"));
assert(!strcmp(plugin_list_items[PLUGIN_LIST_TARGET_MUSIC_AUDIO][0].label,"s1"));
assert(!strcmp(plugin_list_items[PLUGIN_LIST_TARGET_MUSIC_AUDIO][15].label,"s16"));
assert(plugin_list_item_counts[PLUGIN_LIST_TARGET_MUSIC_CONTROLS]==1 && !strcmp(plugin_list_items[PLUGIN_LIST_TARGET_MUSIC_CONTROLS][0].label,"sentinel"));
for(int i=0;i<10;i++){bands[i]=(peq_band_t){100.0+i,0.5,1.0,PEQ_TYPE_PEAKING,true};}
bypass=true;preamp=-2.5;width=1.2;
lua_pushcfunction(L,l_plugin_get_eq_state);lua_setglobal(L,"get_eq_state");lua_pushcfunction(L,l_plugin_eq_apply_state);lua_setglobal(L,"eq_apply_state");lua_pushcfunction(L,l_plugin_get_track_metadata);lua_setglobal(L,"get_track_metadata");
assert(luaL_dostring(L,"s=get_eq_state(); assert(s.bypass and s.preamp_db==-2.5 and s.stereo_width==1.2 and #s.bands==10 and s.bands[10].index==10 and s.bands[1].type=='peaking')")==LUA_OK);
assert(luaL_dostring(L,"s.preamp_db=3; s.bands[1].gain_db=4; assert(eq_apply_state(s,{persist=false}))")==LUA_OK);assert(preamp==3&&bands[0].gain_db==4&&writes==0);
assert(luaL_dostring(L,"s=get_eq_state(); s.preamp_db=7; s.bands[1].gain_db=9; s.bands[10].q=0/0; assert(not pcall(eq_apply_state,s))")==LUA_OK);assert(preamp==3&&bands[0].gain_db==4&&writes==0);
assert(luaL_dostring(L,"s=get_eq_state(); s.preamp_db=7; assert(not pcall(eq_apply_state,s,{persist='false'}))")==LUA_OK);assert(preamp==3&&writes==0);
assert(luaL_dostring(L,"s=get_eq_state(); s.bands[1].gain_db=99; assert(get_eq_state().bands[1].gain_db==4)")==LUA_OK);
assert(luaL_dostring(L,"s=get_eq_state(); assert(eq_apply_state(s))")==LUA_OK&&writes==1);
strcpy(expected.path,"music/test.mp3");expected.id=17;strcpy(expected.tags.title,"Title");strcpy(expected.tags.artist,"Artist");strcpy(expected.tags.album,"Album");strcpy(expected.tags.album_artist,"Album Artist");strcpy(expected.tags.genre,"Jazz");
assert(luaL_dostring(L,"m=get_track_metadata('music/test.mp3'); assert(m.id==17 and m.title=='Title' and m.artist=='Artist' and m.album=='Album' and m.album_artist=='Album Artist' and m.genre=='Jazz')")==LUA_OK);assert(lookups==1);
click_vm=L;assert(luaL_dostring(L,"h=show_list('click page',{'1','2','3','4','5','6'},function()end,{selected=6})")==LUA_OK);
lv_event_t click_event={LV_EVENT_CLICKED,((intptr_t)top_slot<<16)|4};plugin_list_row_click_cb(&click_event);assert(plugin_list_selected_indices[top_slot]==0);
expected.tags.genre[0]='\0';assert(luaL_dostring(L,"assert(get_track_metadata('music/test.mp3').genre=='')")==LUA_OK&&lookups==2);
assert(luaL_dostring(L,"assert(get_track_metadata('https://example.test/a.mp3')==nil)")==LUA_OK&&lookups==2);lua_close(L);puts("observability bindings passed");return 0;}
''')
 lua=[str(x) for x in (root/'lua/src').glob('*.c') if x.name not in ('lua.c','luac.c')]
 cmd=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-DLUA_USE_LINUX','-I'+str(root/'lua/src'),'-I'+str(root/'src/audio'),'-I'+str(root/'src/library'),'-I'+str(root/'src/plugins'),str(c),*lua,'-lm','-ldl','-o',str(pathlib.Path(td)/'test')]
 fixture=pathlib.Path(td)/'music/test.mp3';fixture.parent.mkdir();fixture.touch()
 subprocess.run(cmd,check=True,cwd=root);subprocess.run([str(pathlib.Path(td)/'test')],check=True,cwd=td)
