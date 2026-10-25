/* Small deterministic DB/filesystem doubles for the unchanged pre-extraction
 * scanner body. No tagcache or metadata DB implementation is linked. */
#define TEST_MAX_ROWS 64
typedef struct { bool used, seen; int64_t mtime, size; char path[600]; cached_tags_t tags; } test_row_t;
static test_row_t committed[TEST_MAX_ROWS], staged[TEST_MAX_ROWS];
static bool update_active, targeted_update, db_available = true;
static int32_t test_generation;
static metadata_db_load_outcome_t test_outcome = METADATA_DB_LOAD_SUCCESS_NORMAL;
static bool rows_equal_tags(const cached_tags_t *a, const cached_tags_t *b) {
    return !strcmp(a->title,b->title) && !strcmp(a->artist,b->artist) &&
           !strcmp(a->album,b->album) && !strcmp(a->album_artist,b->album_artist) &&
           !strcmp(a->genre,b->genre) && a->track_number == b->track_number &&
           a->disc_number == b->disc_number && a->release_year == b->release_year;
}
static test_row_t *find_row(test_row_t *rows, const char *path) {
    for (int i=0;i<TEST_MAX_ROWS;i++) if(rows[i].used && !strcmp(rows[i].path,path)) return &rows[i];
    return NULL;
}
static int64_t song_count(void) { int64_t n=0; test_row_t *r=update_active?staged:committed; for(int i=0;i<TEST_MAX_ROWS;i++) n+=r[i].used; return n; }
metadata_db_load_outcome_t metadata_db_open(void) { return test_outcome; }
bool metadata_db_prepare_rebuild(void) { test_outcome=METADATA_DB_LOAD_SUCCESS_NORMAL; memset(committed,0,sizeof(committed)); test_generation++; return true; }
metadata_db_load_outcome_t metadata_db_get_load_outcome(void) { return test_outcome; }
bool metadata_db_storage_current(void) { return db_available && storage_available; }
bool sd_card_root_is_mounted(void) { return storage_available; }
int64_t metadata_db_get_song_count(void) { return song_count(); }
void metadata_db_begin_update(void) { memcpy(staged,committed,sizeof(staged)); for(int i=0;i<TEST_MAX_ROWS;i++)staged[i].seen=false; update_active=true; targeted_update=false; begin_calls++; }
void metadata_db_begin_targeted_update(void) { memcpy(staged,committed,sizeof(staged)); update_active=true; targeted_update=true; begin_calls++; }
void metadata_db_abort_update(void) { if(update_active)abort_calls++; update_active=false; }
bool metadata_db_get(const char *path,int64_t mt,int64_t sz,cached_tags_t*out) { test_row_t*r=find_row(update_active?staged:committed,path); if(!r)return false; if(update_active)r->seen=true; if(r->mtime!=mt||r->size!=sz)return false; if(out)*out=r->tags; return true; }
bool metadata_db_put(const char*path,int64_t mt,int64_t sz,const cached_tags_t*tags) { test_row_t*r=find_row(staged,path); bool changed=!r||!rows_equal_tags(&r->tags,tags); if(!r)for(int i=0;i<TEST_MAX_ROWS;i++)if(!staged[i].used){r=&staged[i];break;} assert(r); if(!r->used){memset(r,0,sizeof(*r));r->used=true;snprintf(r->path,sizeof(r->path),"%s",path);} r->seen=true;r->mtime=mt;r->size=sz;r->tags=*tags; return changed; }
bool metadata_db_end_update(void) { if(!update_active)return false; bool changed=false; if(!targeted_update)for(int i=0;i<TEST_MAX_ROWS;i++)if(staged[i].used&&!staged[i].seen){staged[i].used=false;changed=true;} for(int i=0;i<TEST_MAX_ROWS;i++){test_row_t*a=&committed[i],*b=&staged[i]; if(a->used!=b->used || (a->used&&(strcmp(a->path,b->path)||a->mtime!=b->mtime||a->size!=b->size||!rows_equal_tags(&a->tags,&b->tags))))changed=true;} memcpy(committed,staged,sizeof(committed));update_active=false;end_calls++;if(changed)test_generation++;test_outcome=METADATA_DB_LOAD_SUCCESS_NORMAL;return true; }
bool metadata_db_delete_target_path(const char*path) { test_row_t*r=find_row(staged,path);if(!r)return true;r->used=false;return true; }
bool metadata_db_get_song_by_path(const char*path,song_row_t*out) { test_row_t*r=find_row(update_active?staged:committed,path);if(!r)return false;memset(out,0,sizeof(*out));out->id=(r-staged)+1;snprintf(out->path,sizeof(out->path),"%s",r->path);out->tags=r->tags;return true; }
int metadata_db_get_album_songs(const char*album,const char*artist,int offset,song_row_t*out,int max) { (void)album;(void)artist;(void)offset;(void)out;(void)max;return 0; }
int metadata_db_get_artist_songs(const char*artist,int offset,song_row_t*out,int max) { (void)artist;(void)offset;(void)out;(void)max;return 0; }
int metadata_db_get_album_artist_songs(const char*artist,int offset,song_row_t*out,int max) { (void)artist;(void)offset;(void)out;(void)max;return 0; }
bool metadata_db_album_sort_available(metadata_db_album_sort_t sort) { (void)sort;return true; }
int32_t tagcache_generation(void) { return test_generation; }
uint64_t song_album_thumbnail_key(const song_row_t*row) { return (uint64_t)row->id; }
void gui_books_rescan(void) { books_rescan_calls++; }
void ensure_playlists_dir(void) {}
void playlist_files_reconcile(const char*path,bool mounted) { (void)path;(void)mounted; }
bool db_log_enabled(void) { return false; }
uint64_t db_log_now_ms(void) { return 1; }
long db_log_rss_kb(void) { return 0; }
static void parse_fixture_tags(const char*path,track_metadata_t*out) {
    FILE*f=fopen(path,"r");if(!f)return;char line[256];while(fgets(line,sizeof(line),f)){char*eq=strchr(line,'=');if(!eq)continue;*eq++='\0';eq[strcspn(eq,"\r\n")]='\0';
        if(!strcmp(line,"FAIL")){parse_failure=true;continue;}
        if(!strcmp(line,"title")){snprintf(out->title,sizeof(out->title),"%s",eq);out->has_title=true;}
        else if(!strcmp(line,"artist")){snprintf(out->artist,sizeof(out->artist),"%s",eq);out->has_artist=true;}
        else if(!strcmp(line,"album")){snprintf(out->album,sizeof(out->album),"%s",eq);out->has_album=true;}
    }fclose(f);
}
bool metadata_read_isolated(const char*path,track_metadata_t*out,int timeout) { (void)timeout;parser_calls++;if(cancel_during_parse)library_scan_request_cancel();if(lose_storage_during_parse)storage_available=false;if(parse_failure){parse_failure=false;return false;}memset(out,0,sizeof(*out));parse_fixture_tags(path,out);return true; }
static bool walk_dir(const char*root,const char*relative,const char*excluded,file_browser_song_visit_cb_t cb,void*user,int*count,atomic_int*progress,int*skipped) {
    char dirpath[PATH_MAX];snprintf(dirpath,sizeof(dirpath),"%s%s%s",root,relative[0]?"/":"",relative);DIR*d=opendir(dirpath);if(!d){(*skipped)++;return true;}struct dirent*ent;bool ok=true;
    while((ent=readdir(d))){if(!strcmp(ent->d_name,".")||!strcmp(ent->d_name,".."))continue;if(!relative[0]&&!strcasecmp(ent->d_name,excluded))continue;char child[PATH_MAX];snprintf(child,sizeof(child),"%s%s%s",relative,relative[0]?"/":"",ent->d_name);char full[PATH_MAX];snprintf(full,sizeof(full),"%s/%s",root,child);struct stat st;if(stat(full,&st)){(*skipped)++;continue;}atomic_fetch_add(progress,1);walk_visits++;if(S_ISDIR(st.st_mode)){if(!walk_dir(root,child,excluded,cb,user,count,progress,skipped))ok=false;}else if(S_ISREG(st.st_mode)){(*count)++;if(cancel_during_walk){cancel_during_walk=false;library_scan_request_cancel();}if(!cb(full,user)){ok=false;break;}}if(!ok)break;}closedir(d);return ok;
}
bool file_browser_walk_all_songs_excluding_top_level(const char*root,const char*excluded,file_browser_song_visit_cb_t cb,void*user,int*out_count,atomic_int*progress,int*out_skipped) { int count=0,skipped=0;bool ok=walk_dir(root,"",excluded,cb,user,&count,progress,&skipped);*out_count=count;*out_skipped=skipped;return ok; }
