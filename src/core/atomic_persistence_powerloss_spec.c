#include "settings.h"
#include "subsonic_saved_servers.h"
#include "queue_resume.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t notified;
static void on_notice(int sig) { (void)sig; notified = 1; }
static void checkpoint(const char *stage) {
    const char *wanted = getenv("PERSIST_STAGE");
    if (!wanted || strcmp(wanted, stage)) return;
    (void)kill(getppid(), SIGUSR1);
    for (;;) pause();
}
extern int __real_fsync(int);
extern int __real_rename(const char *, const char *);
extern int __real_renameat(int, const char *, int, const char *);
extern int __real_fflush(FILE *);
extern int __real_fclose(FILE *);
int __wrap_fsync(int fd) {
    const char *mode = getenv("PERSIST_STAGE");
    if (mode && (!strcmp(mode, "settings-eio") || !strcmp(mode, "settings-enospc") || !strcmp(mode, "queue-eio"))) {
        int error = !strcmp(mode, "settings-enospc") ? ENOSPC : EIO;
        unsetenv("PERSIST_STAGE"); errno = error; return -1;
    }
    checkpoint("precommit");
    return __real_fsync(fd);
}
int __wrap_rename(const char *from, const char *to) {
    const char *mode = getenv("PERSIST_STAGE");
    if (mode && !strcmp(mode, "settings-rename-eio")) { errno = EIO; return -1; }
    int rc = __real_rename(from, to);
    if (rc == 0) checkpoint("postcommit");
    return rc;
}
int __wrap_renameat(int olddir, const char *from, int newdir, const char *to) {
    int rc = __real_renameat(olddir, from, newdir, to);
    if (rc == 0) checkpoint("postcommit");
    return rc;
}
int __wrap_fflush(FILE *f) {
    const char *mode = getenv("PERSIST_STAGE");
    if (mode && !strcmp(mode, "settings-fflush-eio")) { unsetenv("PERSIST_STAGE"); errno = EIO; return EOF; }
    return __real_fflush(f);
}
int __wrap_fclose(FILE *f) {
    const char *mode = getenv("PERSIST_STAGE");
    if (mode && !strcmp(mode, "settings-fclose-eio")) {
        unsetenv("PERSIST_STAGE");
        int rc = __real_fclose(f);
        errno = EIO;
        return rc == 0 ? EOF : rc;
    }
    return __real_fclose(f);
}

void subsonic_saved_servers_load(subsonic_saved_server_t **rows, int *count) { *rows=NULL; *count=0; }
void subsonic_saved_servers_upsert(const char *url,const char *user,const char *password,bool tls) { (void)url;(void)user;(void)password;(void)tls; }
static void save_settings(float volume) {
    player_settings_t s; (void)settings_load(&s); s.volume = volume; settings_save(&s);
}
static float load_volume(void) {
    player_settings_t s; assert(settings_load(&s)); return s.volume;
}
static size_t read_settings_bytes(unsigned char *out, size_t cap) {
    FILE *f=fopen(".compas/settings.txt","rb"); assert(f); size_t n=fread(out,1,cap,f); assert(!ferror(f)); assert(feof(f)); assert(fclose(f)==0); return n;
}
static void assert_settings_unchanged(const unsigned char *expected, size_t expected_len) {
    unsetenv("PERSIST_STAGE");
    unsigned char actual[65536]; size_t n=read_settings_bytes(actual,sizeof(actual));
    assert(n==expected_len && memcmp(actual,expected,n)==0);
    assert(access(".compas/settings.txt.tmp",F_OK)!=0 && errno==ENOENT);
}
static queue_resume_t queue_state(const char *path) {
    queue_resume_t q = {.count=1,.current=0,.position=4.0};
    q.paths = calloc(1, sizeof(*q.paths)); assert(q.paths); q.paths[0] = strdup(path); assert(q.paths[0]); return q;
}
static void prepare_queue(const char *path) { queue_resume_t q=queue_state(path); assert(queue_resume_write("queue.bin",&q)); queue_resume_free(&q); }
static void assert_queue(const char *expected) { queue_resume_t q={0}; assert(queue_resume_read("queue.bin",&q)); assert(q.count==1 && !strcmp(q.paths[0],expected)); queue_resume_free(&q); }
static int child_run(const char *kind, const char *stage) {
    setenv("PERSIST_STAGE", stage, 1);
    if (!strcmp(kind,"settings")) save_settings(0.75f);
    else { queue_resume_t q=queue_state("/new/complete.track"); (void)queue_resume_write("queue.bin",&q); queue_resume_free(&q); }
    _exit(0);
}
static void kill_at(const char *kind, const char *stage) {
    sigset_t signal_set, previous_mask, suspend_mask;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGUSR1);
    assert(sigprocmask(SIG_BLOCK, &signal_set, &previous_mask) == 0);
    suspend_mask = previous_mask;
    sigdelset(&suspend_mask, SIGUSR1);
    notified=0;
    pid_t p=fork(); assert(p>=0); if (!p) child_run(kind,stage);
    while (!notified) (void)sigsuspend(&suspend_mask);
    assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
    assert(kill(p,SIGKILL)==0);
    int st;
    assert(waitpid(p,&st,0)==p && WIFSIGNALED(st) && WTERMSIG(st)==SIGKILL);
}
int main(void) {
    struct sigaction sa={0}; sa.sa_handler=on_notice; sigemptyset(&sa.sa_mask); assert(sigaction(SIGUSR1,&sa,NULL)==0);
    assert(mkdir(".compas",0700)==0 || errno==EEXIST);
    save_settings(0.25f); kill_at("settings","precommit"); assert(load_volume()==0.25f);
    save_settings(0.25f); kill_at("settings","postcommit"); assert(load_volume()==0.75f);
    prepare_queue("/old/complete.track"); kill_at("queue","precommit"); assert_queue("/old/complete.track");
    prepare_queue("/old/complete.track"); kill_at("queue","postcommit"); assert_queue("/new/complete.track");
    prepare_queue("/old/complete.track"); setenv("PERSIST_STAGE","queue-eio",1); queue_resume_t q=queue_state("/new/complete.track"); assert(!queue_resume_write("queue.bin",&q)); queue_resume_free(&q); assert_queue("/old/complete.track");
    const char *fail_stages[]={"settings-eio","settings-enospc","settings-fflush-eio","settings-fclose-eio","settings-rename-eio"};
    for (size_t i=0;i<sizeof(fail_stages)/sizeof(fail_stages[0]);++i) {
        save_settings(0.25f);
        unsigned char old_bytes[65536]; size_t old_len=read_settings_bytes(old_bytes,sizeof(old_bytes));
        player_settings_t next; (void)settings_load(&next); next.volume=0.75f;
        setenv("PERSIST_STAGE",fail_stages[i],1); settings_save(&next);
        assert_settings_unchanged(old_bytes,old_len);
    }
    puts("atomic persistence interruption and failure invariants passed"); return 0;
}
