#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "player_queue_model.h"

static bool fail_next_malloc;
static size_t wrapped_malloc_calls;
static bool track_releases;
static int tracked_fd=-1;
static void *track_path0, *track_path1, *track_paths, *track_ranks, *track_bag, *track_cont;
static char release_events[32]; static size_t release_event_count;
void __real_free(void *ptr);
void __wrap_free(void *ptr) {
    if (track_releases) {
        if (ptr==track_bag) release_events[release_event_count++]='B';
        else if (ptr==track_cont) release_events[release_event_count++]='C';
        else if (ptr==track_path0 || ptr==track_path1) release_events[release_event_count++]='p';
        else if (ptr==track_paths) release_events[release_event_count++]='P';
        else if (ptr==track_ranks) release_events[release_event_count++]='R';
    }
    __real_free(ptr);
}
int __real_close(int fd);
int __wrap_close(int fd) {
    if (track_releases && fd==tracked_fd) release_events[release_event_count++]='D';
    return __real_close(fd);
}
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
    wrapped_malloc_calls++;
    if (fail_next_malloc) { fail_next_malloc = false; return NULL; }
    return __real_malloc(size);
}

struct tagcache_snapshot { const char **paths; int count; int refs; int reads; };
struct file_browser_index { const char **paths; int count; int refs; int reads; };

bool metadata_db_snapshot_path_at(const metadata_db_snapshot_t *snapshot, int rank,
                                  char *out, size_t out_size) {
    struct tagcache_snapshot *s = (struct tagcache_snapshot *)snapshot;
    s->reads++;
    if (rank < 0 || rank >= s->count || !s->paths[rank]) return false;
    snprintf(out, out_size, "%s", s->paths[rank]);
    return true;
}
void metadata_db_snapshot_close(metadata_db_snapshot_t *snapshot) {
    if (track_releases) release_events[release_event_count++]='S';
    if (snapshot) ((struct tagcache_snapshot *)snapshot)->refs--;
}
bool file_browser_index_playable_path_at(const file_browser_index_t *index, unsigned rank,
                                         char *out, size_t out_size) {
    struct file_browser_index *s = (struct file_browser_index *)index;
    s->reads++;
    if (rank >= (unsigned)s->count || !s->paths[rank]) return false;
    snprintf(out, out_size, "%s", s->paths[rank]);
    return true;
}
void file_browser_index_close(file_browser_index_t *index) {
    if (track_releases) release_events[release_event_count++]='I';
    if (index) ((struct file_browser_index *)index)->refs--;
}
bool remote_track_path_is_remote(const char *path) { (void)path; return false; }

static void clear_model(void) {
    player_queue_model_clear_begin();
    player_queue_model_clear_finish();
}

static void install(const char *const *paths, int count, int current,
                    const int *order, int shuffle_pos,
                    const int *continuation, int pending, int cursor) {
    clear_model();
    char **owned_paths = count ? calloc((size_t)count, sizeof(*owned_paths)) : NULL;
    assert(!count || owned_paths);
    for (int i = 0; i < count; i++) {
        owned_paths[i] = paths[i] ? strdup(paths[i]) : NULL;
        assert(!paths[i] || owned_paths[i]);
    }
    int *owned_order = NULL, *owned_continuation = NULL;
    if (order) {
        owned_order = malloc((size_t)count * sizeof(*owned_order));
        assert(owned_order); memcpy(owned_order, order, (size_t)count * sizeof(*order));
    }
    if (continuation) {
        owned_continuation = malloc((size_t)count * sizeof(*owned_continuation));
        assert(owned_continuation);
        memcpy(owned_continuation, continuation, (size_t)count * sizeof(*continuation));
    }
    player_queue_model_install_take((player_queue_install_t){
        .paths = owned_paths, .count = count, .current = current,
        .shuffle_order = owned_order, .shuffle_continuation = owned_continuation,
        .shuffle_pos = shuffle_pos, .pending_count = pending,
        .next_insert_index = cursor, .queue_sd_dirfd = -1
    });
}

static void test_modes_and_snapshot(void) {
    const char *p[] = {"a", "b", "c", "d"};
    int identity[] = {0,1,2,3};
    install(p,4,1,NULL,-1,NULL,0,-1);
    assert(player_queue_model_predict_auto_advance(PLAY_MODE_SEQUENTIAL,1)==2);
    assert(player_queue_model_predict_auto_advance(PLAY_MODE_SEQUENTIAL,3)==-1);
    assert(player_queue_model_predict_auto_advance(PLAY_MODE_REPEAT_ALL,3)==0);
    assert(player_queue_model_predict_auto_advance(PLAY_MODE_REPEAT_ONE,3)==3);
    install(p,4,1,identity,1,NULL,0,-1);
    assert(player_queue_model_predict_auto_advance(PLAY_MODE_SHUFFLE,1)==2);
    install(p,4,3,identity,3,NULL,0,-1);
    assert(player_queue_model_predict_auto_advance(PLAY_MODE_SHUFFLE_ONCE,3)==-1);
    install(p,4,1,identity,1,NULL,0,-1);
    player_queue_display_snapshot_t snap={0};
    assert(player_queue_model_display_snapshot(PLAY_MODE_SHUFFLE,&snap));
    assert(snap.count==4 && snap.current==1 && snap.display_order[1]==1);
    assert(player_queue_model_current()==1);
    player_queue_model_display_snapshot_free(&snap);
    clear_model();
}

static void test_pending_and_tail_gate(void) {
    const char *p[]={"a","b","c"}; int bag[]={0,2,1};
    install(p,3,2,bag,2,NULL,2,5);
    assert(player_queue_model_manual_step(PLAY_MODE_REPEAT_ALL,2,1)==0);
    assert(player_queue_model_pending_count()==2);
    player_queue_model_commit_auto_advance(PLAY_MODE_REPEAT_ALL);
    assert(player_queue_model_pending_count()==1);
    assert(player_queue_model_current()==2);
    install(p,3,0,bag,0,NULL,2,3);
    assert(player_queue_model_manual_step(PLAY_MODE_REPEAT_ONE,0,1)==1);
    assert(player_queue_model_pending_count()==1);
    assert(player_queue_model_manual_step(PLAY_MODE_REPEAT_ONE,1,-1)==0);
    assert(player_queue_model_pending_count()==0);
    clear_model();
}

static void test_shuffle_manual_auto_and_continuation(void) {
    const char *p[]={"a","b","c","d"}; int bag[]={0,1,2,3}, cont[]={3,2,1,0};
    install(p,4,3,bag,3,cont,0,-1);
    int selected=player_queue_model_manual_step(PLAY_MODE_SHUFFLE,3,1);
    assert(selected>=0 && selected<4);
    queue_resume_t saved={0};
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==0 && saved.continuation);
    queue_resume_free(&saved);
    install(p,4,3,bag,3,cont,0,-1);
    player_queue_model_commit_auto_advance(PLAY_MODE_SHUFFLE);
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==0 && saved.order && !saved.continuation);
    assert(memcmp(saved.order,cont,sizeof(cont))==0);
    queue_resume_free(&saved);
    clear_model();
}

static void test_add_remove_play_next_cursor(void) {
    const char *p[]={"a","b","c","d"};
    install(p,4,0,NULL,-1,NULL,2,3);
    assert(player_queue_model_play_next("next",PLAY_MODE_SEQUENTIAL));
    assert(player_queue_model_count()==5 && player_queue_model_pending_count()==3);
    assert(strcmp(player_queue_model_path_at(1),"next")==0);
    assert(player_queue_model_pending_count()==3);
    assert(player_queue_model_remove_pending(1));
    assert(player_queue_model_pending_count()==2);
    clear_model();
}

static void test_failure_plan_multi_consume_and_physical_current(void) {
    const char *p[]={"f0","f1","f2","f3","f4"}; int bag[]={0,1,2,3,4};
    const char *failed[]={"f1","f2","f3"};
    install(p,5,0,bag,0,NULL,2,3);
    assert(player_queue_model_advance_past_failed(PLAY_MODE_SHUFFLE,failed,3)==4);
    assert(player_queue_model_pending_count()==0);
    assert(player_queue_model_current()==0);
    queue_resume_t saved={0};
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==4);
    queue_resume_free(&saved);
    clear_model();
}

static void test_failure_eager_continuation_and_no_target(void) {
    const char *p[]={"a","b","c","d"}; int bag[]={0,1,2,3};
    const char *failed_all[]={"b","c","d"};
    install(p,4,0,bag,0,NULL,0,-1);
    assert(player_queue_model_advance_past_failed(PLAY_MODE_SHUFFLE,failed_all,3)==-1);
    queue_resume_t saved={0};
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==0 && saved.continuation);
    queue_resume_free(&saved);
    clear_model();
}

static void test_delete_clear_and_revision(void) {
    const char *p[]={"delete-a","delete-b"};
    int ranks[]={10,20};
    char **slots=calloc(2,sizeof(*slots)); assert(slots);
    slots[0]=strdup(p[0]); slots[1]=strdup(p[1]); assert(slots[0]&&slots[1]);
    int *owned_ranks=malloc(2*sizeof(int)); assert(owned_ranks); memcpy(owned_ranks,ranks,sizeof(ranks));
    player_queue_model_install_take((player_queue_install_t){
        .paths=slots,.count=2,.current=0,.lazy_ranks=owned_ranks,
        .shuffle_pos=-1,.queue_sd_dirfd=-1
    });
    int next=-1; assert(player_queue_model_delete_current(&next));
    assert(next==0 && player_queue_model_count()==1);
    assert(strcmp(player_queue_model_path_at(0),"delete-b")==0);
    clear_model();

    uint64_t rev=player_queue_model_revision();
    install(p,2,0,NULL,-1,NULL,0,-1);
    rev=player_queue_model_revision();
    player_queue_model_note_next_armed(PLAY_MODE_SEQUENTIAL,0,NULL,NULL);
    assert(player_queue_model_revision()==rev+1);
    assert(player_queue_model_clear_to_current(PLAY_MODE_SEQUENTIAL));
    assert(player_queue_model_count()==1 && player_queue_model_current()==0);
    clear_model();
}

static void test_lazy_cache_clear_and_lazy_predicate(void) {
    const char *paths[]={"db/a",NULL};
    struct tagcache_snapshot db={.paths=paths,.count=2,.refs=1};
    int *ranks=malloc(2*sizeof(int)); assert(ranks); ranks[0]=0; ranks[1]=1;
    char **slots=calloc(2,sizeof(*slots)); assert(slots);
    player_queue_model_install_take((player_queue_install_t){
        .paths=slots,.count=2,.current=0,.lazy_ranks=ranks,.snapshot=(metadata_db_snapshot_t *)&db,
        .shuffle_pos=-1,.queue_sd_dirfd=-1
    });
    assert(player_queue_model_has_lazy_order());
    int reads=db.reads;
    assert(player_queue_model_count()==2 && db.reads==reads);
    assert(strcmp(player_queue_model_path_at(0),"db/a")==0);
    assert(strcmp(player_queue_model_path_at(0),"db/a")==0 && db.reads==1);
    assert(player_queue_model_clear_to_current(PLAY_MODE_SEQUENTIAL));
    assert(player_queue_model_count()==1 && !player_queue_model_has_lazy_order());
    assert(db.refs==0);
    clear_model();
}

static void test_checkpoint_is_physical_order(void) {
    const char *p[]={"a","b","c","d"}; int bag[]={0,2,1,3}, cont[]={3,1,0,2};
    install(p,4,2,bag,1,cont,1,4);
    queue_resume_t saved={0};
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,12.5,&saved));
    assert(saved.current==2 && saved.pending==1 && saved.shuffle_pos==1);
    assert(strcmp(saved.paths[0],"a")==0 && strcmp(saved.paths[2],"c")==0);
    assert(memcmp(saved.order,bag,sizeof(bag))==0);
    assert(memcmp(saved.continuation,cont,sizeof(cont))==0);
    queue_resume_free(&saved);
    clear_model();
}

static void test_failure_wrap_promotion_and_stale_revision(void) {
    const char *p[]={"w0","w1","w2","w3","w4"};
    int bag[]={0,1,2,3,4}, cont[]={1,4,3,2,0};
    const char *failed[]={"w4"};
    install(p,5,3,bag,3,cont,0,-1);
    int target=player_queue_model_advance_past_failed(PLAY_MODE_SHUFFLE,failed,1);
    assert(target==1 && player_queue_model_current()==3);
    queue_resume_t saved={0};
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==0 && saved.order && !saved.continuation);
    assert(memcmp(saved.order,cont,sizeof(cont))==0);
    queue_resume_free(&saved);

    uint64_t rev=player_queue_model_revision();
    int *order=NULL, count=0, current=-1; uint64_t snaprev=0;
    player_queue_display_snapshot_t display={0};
    assert(player_queue_model_display_snapshot(PLAY_MODE_SEQUENTIAL,&display));
    free(display.display_order);
    player_queue_model_note_next_armed(PLAY_MODE_SEQUENTIAL,3,NULL,NULL);
    assert(player_queue_model_revision()==rev+1);
    assert(player_queue_model_edit_displayed(rev,4,3,PLAY_MODE_SEQUENTIAL)==PLAYER_QUEUE_EDIT_STALE);
    assert(!player_queue_model_select_displayed(rev,4,PLAY_MODE_SEQUENTIAL,NULL));
    (void)order; (void)count; (void)current; (void)snaprev;
    clear_model();
}

static void test_eager_nonwrap_and_failed_continuation_alloc(void) {
    const char *p[]={"a","b","c","d"}; int bag[]={0,1,2,3};
    const char *failed[]={"b","d"};
    install(p,4,0,bag,0,NULL,0,-1);
    fail_next_malloc=true;
    assert(player_queue_model_advance_past_failed(PLAY_MODE_SHUFFLE,failed,2)==2);
    queue_resume_t saved={0};
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==2 && !saved.continuation);
    queue_resume_free(&saved);

    install(p,4,0,bag,0,NULL,0,-1);
    assert(player_queue_model_advance_past_failed(PLAY_MODE_SHUFFLE,failed,2)==2);
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==2 && saved.continuation);
    queue_resume_free(&saved);
    clear_model();
}

static void test_lazy_failure_nonmutation_and_source_order(void) {
    const char *missing[]={NULL};
    const char *index_paths[]={NULL};
    struct tagcache_snapshot db={.paths=missing,.count=1,.refs=1};
    struct file_browser_index index={.paths=index_paths,.count=1,.refs=1};
    char **slots=calloc(2,sizeof(*slots)); assert(slots);
    int *ranks=malloc(2*sizeof(*ranks)); assert(ranks); ranks[0]=0; ranks[1]=0;
    player_queue_model_install_take((player_queue_install_t){
        .paths=slots,.count=2,.current=0,.lazy_ranks=ranks,
        .snapshot=(metadata_db_snapshot_t *)&db,.file_index=(file_browser_index_t *)&index,
        .shuffle_pos=-1,.queue_sd_dirfd=-1
    });
    int db_reads=db.reads, index_reads=index.reads;
    assert(player_queue_model_has_lazy_order());
    assert(!player_queue_model_has_resolved_sd_path("/sd"));
    assert(db.reads==db_reads && index.reads==index_reads);
    assert(!player_queue_model_clear_to_current(PLAY_MODE_SEQUENTIAL));
    assert(player_queue_model_count()==2 && player_queue_model_current()==0);
    assert(player_queue_model_path_if_resolved(0)[0]=='\0');
    assert(db.refs==1 && index.refs==1);
    assert(strcmp(player_queue_model_path_at(0),"")==0);
    index_paths[0]="fallback";
    assert(strcmp(player_queue_model_path_at(0),"fallback")==0);
    assert(db.reads==3 && index.reads==3);
    assert(player_queue_model_clear_to_current(PLAY_MODE_SEQUENTIAL));
    assert(db.refs==0 && index.refs==0);
    clear_model();
}

static void test_auto_wrap_prediction_commit_once(void) {
    const char *p[]={"s0","s1","s2","s3"}; int bag[]={0,1,2,3}, cont[]={2,0,3,1};
    install(p,4,3,bag,3,cont,0,-1);
    int next=-1; uint64_t rev;
    const char *next_path=player_queue_model_note_next_armed(PLAY_MODE_SHUFFLE,3,&next,&rev);
    assert(next==2 && strcmp(next_path,"s2")==0);
    assert(player_queue_model_predict_auto_advance(PLAY_MODE_SHUFFLE,3)==next);
    queue_resume_t saved={0};
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==3 && saved.continuation);
    queue_resume_free(&saved);
    player_queue_model_commit_auto_advance(PLAY_MODE_SHUFFLE);
    assert(player_queue_model_checkpoint_snapshot(PLAY_MODE_SHUFFLE,0,&saved));
    assert(saved.shuffle_pos==0 && !saved.continuation && saved.order[0]==next);
    queue_resume_free(&saved);
    clear_model();
}

static void test_edit_select_displayed_operations(void) {
    const char *p[]={"a","b","c","d"}; int bag[]={2,0,3,1};
    install(p,4,0,bag,1,NULL,1,2);
    uint64_t rev=player_queue_model_revision();
    assert(player_queue_model_edit_displayed(rev,2,3,PLAY_MODE_SHUFFLE)==PLAYER_QUEUE_EDIT_OK);
    player_queue_display_snapshot_t display={0};
    assert(player_queue_model_display_snapshot(PLAY_MODE_SHUFFLE,&display));
    assert(display.count==4 && display.current==1);
    assert(strcmp(player_queue_model_path_at(display.display_order[2]),"b")==0);
    player_queue_model_display_snapshot_free(&display);
    int physical=-1;
    assert(player_queue_model_select_displayed(player_queue_model_revision(),2,
                                               PLAY_MODE_SHUFFLE,&physical));
    assert(physical==2 && player_queue_model_current()==1);
    player_queue_model_select_physical(physical);
    assert(player_queue_model_current()==2);
    clear_model();
}

static void test_paired_clear_revision_fd_and_release_order(void) {
    const char *p[]={"p0","p1"}; int bag[]={0,1}, cont[]={1,0}, ranks[]={0,1};
    char **slots=calloc(2,sizeof(*slots)); assert(slots);
    slots[0]=strdup(p[0]); slots[1]=strdup(p[1]); assert(slots[0]&&slots[1]);
    int *owned_ranks=malloc(sizeof(ranks)); int *owned_bag=malloc(sizeof(bag));
    int *owned_cont=malloc(sizeof(cont)); assert(owned_ranks&&owned_bag&&owned_cont);
    memcpy(owned_ranks,ranks,sizeof(ranks)); memcpy(owned_bag,bag,sizeof(bag));
    memcpy(owned_cont,cont,sizeof(cont));
    struct tagcache_snapshot db={.paths=p,.count=2,.refs=1};
    struct file_browser_index index={.paths=p,.count=2,.refs=1};
    int fd=open("/dev/null",O_RDONLY); assert(fd>=0);
    player_queue_model_install_take((player_queue_install_t){
        .paths=slots,.count=2,.current=0,.lazy_ranks=owned_ranks,.snapshot=(metadata_db_snapshot_t *)&db,
        .file_index=(file_browser_index_t *)&index,.shuffle_order=owned_bag,
        .shuffle_continuation=owned_cont,.shuffle_pos=0,.pending_count=1,
        .next_insert_index=2,.queue_sd_dirfd=fd
    });
    uint64_t before=player_queue_model_revision();
    track_path0=slots[0]; track_path1=slots[1]; track_paths=slots;
    track_ranks=owned_ranks; track_bag=owned_bag; track_cont=owned_cont; tracked_fd=fd;
    release_event_count=0; track_releases=true;
    player_queue_model_clear_begin();
    assert(player_queue_model_revision()==before+1);
    assert(player_queue_model_count()==2 && player_queue_model_current()==0);
    assert(player_queue_model_pending_count()==0 && player_queue_model_has_sd_dirfd_pin());
    assert(release_event_count==2 && release_events[0]=='B' && release_events[1]=='C');
    player_queue_model_clear_finish();
    track_releases=false;
    release_events[release_event_count]='\0';
    assert(strcmp(release_events,"BCppPDRSI")==0);
    assert(db.refs==0 && index.refs==0 && !player_queue_model_has_sd_dirfd_pin());
    assert(player_queue_model_count()==0 && player_queue_model_current()==-1);
    errno=0; assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF);
    tracked_fd=-1;
}

static void test_empty_delete_defers_final_cleanup(void) {
    const char *old_path[]={"old"}; int next=-2;
    install(old_path,1,0,NULL,-1,NULL,0,-1);
    assert(player_queue_model_delete_current(&next) && next==-1);
    assert(player_queue_model_count()==0 && player_queue_model_current()==0);
    player_queue_model_finish_empty_delete();
    assert(player_queue_model_count()==0 && player_queue_model_current()==-1);
    clear_model();
}

static void test_shuffle_once_missing_bag_does_not_allocate(void) {
    const char *p[]={"a","b","c"};
    install(p,3,0,NULL,-1,NULL,0,-1);
    size_t before=wrapped_malloc_calls;
    assert(player_queue_model_advance_past_failed(PLAY_MODE_SHUFFLE_ONCE,NULL,0)==-1);
    assert(wrapped_malloc_calls==before);
    clear_model();
}

static void test_empty_play_next_noop_preserves_insert_order(void) {
    const char *p[]={"a","b","c","d"};
    install(p,4,0,NULL,-1,NULL,0,3);
    assert(!player_queue_model_play_next("",PLAY_MODE_SEQUENTIAL));
    const char *later[]={"later"}; int added=0;
    assert(player_queue_model_try_add_many(later,1,PLAY_MODE_SEQUENTIAL,&added) && added==1);
    assert(strcmp(player_queue_model_path_at(3),"later")==0);
    clear_model();
}

static void test_stream_cache_predicate_and_owned_pin_cleanup(void) {
    assert(player_queue_model_is_stream_cache_path("/tmp/subsonic_stream_cache/x"));
    assert(player_queue_model_is_stream_cache_path("/tmp/subsonic_stream_cache-extra"));
    assert(!player_queue_model_is_stream_cache_path("/tmp/other"));
    char dir[]="/tmp/player-queue-model-cleanup.XXXXXX";
    assert(mkdtemp(dir));
    int dirfd=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC); assert(dirfd>=0);
    int file=openat(dirfd,"queue.bin",O_CREAT|O_WRONLY|O_CLOEXEC,0600); assert(file>=0); close(file);
    const char *p[]={"x"}; char **slots=calloc(1,sizeof(*slots)); assert(slots);
    slots[0]=strdup(p[0]); assert(slots[0]);
    player_queue_model_install_take((player_queue_install_t){
        .paths=slots,.count=1,.current=0,.shuffle_pos=-1,.next_insert_index=-1,
        .queue_sd_dirfd=dirfd
    });
    player_queue_model_remove_sd_checkpoint("queue.bin");
    struct stat st; assert(fstatat(dirfd,"queue.bin",&st,0)==-1 && errno==ENOENT);
    clear_model();
    assert(rmdir(dir)==0);
}

int main(void) {
    test_modes_and_snapshot();
    test_pending_and_tail_gate();
    test_shuffle_manual_auto_and_continuation();
    test_add_remove_play_next_cursor();
    test_failure_plan_multi_consume_and_physical_current();
    test_failure_eager_continuation_and_no_target();
    test_delete_clear_and_revision();
    test_lazy_cache_clear_and_lazy_predicate();
    test_checkpoint_is_physical_order();
    test_failure_wrap_promotion_and_stale_revision();
    test_eager_nonwrap_and_failed_continuation_alloc();
    test_lazy_failure_nonmutation_and_source_order();
    test_auto_wrap_prediction_commit_once();
    test_edit_select_displayed_operations();
    test_paired_clear_revision_fd_and_release_order();
    test_empty_delete_defers_final_cleanup();
    test_shuffle_once_missing_bag_does_not_allocate();
    test_empty_play_next_noop_preserves_insert_order();
    test_stream_cache_predicate_and_owned_pin_cleanup();
    puts("Public player queue model characterization passed");
    return 0;
}
