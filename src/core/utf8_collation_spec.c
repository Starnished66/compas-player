#include "utf8_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int signum(int x) { return (x > 0) - (x < 0); }
static void expect(int (*cmp)(const char *,const char *), const char *a,const char *b,int want) {
    int got = signum(cmp(a,b));
    if (got != want) fprintf(stderr, "compare failed: [%s] vs [%s], got %d wanted %d\n", a, b, got, want);
    assert(got == want);
}
static void key_order(const char *a,const char *b,bool identity) {
    unsigned char ka[1024],kb[1024];
    bool fa= identity ? utf8_collate_identity_prefix(a,ka,sizeof ka) : utf8_collate_prefix(a,ka,sizeof ka);
    bool fb= identity ? utf8_collate_identity_prefix(b,kb,sizeof kb) : utf8_collate_prefix(b,kb,sizeof kb);
    assert(fa && fb);
    int key=signum(memcmp(ka,kb,sizeof ka));
    int full=signum(identity ? utf8_collate_identity_compare(a,b) : utf8_collate_compare(a,b));
    assert(key == full);
}
static void check_prefix_contract(const char *a, const char *b, size_t width, bool identity) {
    unsigned char ka[64], kb[64];
    bool full_a = identity ? utf8_collate_identity_prefix(a,ka,width) : utf8_collate_prefix(a,ka,width);
    bool full_b = identity ? utf8_collate_identity_prefix(b,kb,width) : utf8_collate_prefix(b,kb,width);
    if ((memchr(ka,0,width) != NULL) != full_a || (memchr(kb,0,width) != NULL) != full_b)
        fprintf(stderr,"prefix completeness mismatch width=%zu identity=%d [%s] [%s] full=%d/%d nul=%d/%d\n",
                width, identity, a, b, full_a, full_b, memchr(ka,0,width)!=NULL, memchr(kb,0,width)!=NULL);
    assert((memchr(ka,0,width) != NULL) == full_a);
    assert((memchr(kb,0,width) != NULL) == full_b);
    int prefix = signum(memcmp(ka,kb,width));
    int actual = signum(identity ? utf8_collate_identity_compare(a,b) : utf8_collate_compare(a,b));
    if (prefix) assert(prefix == actual);
    else if (memchr(ka,0,width) && memchr(kb,0,width)) assert(actual == 0);
    else assert(!full_a || !full_b || actual == 0);
}
int main(void) {
    expect(utf8_collate_compare,"Éclair","eclair",0);
    expect(utf8_collate_compare,"Æther","aether",0);
    expect(utf8_collate_compare,"Œuvre","oeuvre",0);
    expect(utf8_collate_compare,"Ħome","home",0);
    expect(utf8_collate_compare,"Ŧest","test",0);
    expect(utf8_collate_compare,"Ŀuna","luna",0);
    expect(utf8_collate_compare,"ıris","iris",0);
    expect(utf8_collate_compare,"Ĳssel","ijssel",0);
    expect(utf8_collate_compare,"  ‘...Hello","hello",0);
    expect(utf8_collate_compare,"¿Bolero yo?","Bolero yo?",0);
    expect(utf8_collate_compare,"¿Por qué regresa Abril?","Por qué regresa Abril?",0);
    expect(utf8_collate_compare,"¡...Bóléro!","Bolero!",0);
    expect(utf8_collate_identity_compare_v6,"¿Bolero yo?","Zulu",1);
    expect(utf8_collate_compare,"¿Bolero yo?","Bolero yo?",0);
    expect(utf8_collate_identity_compare_v6,"\x7f" "Alpha","Alpha",1);
    expect(utf8_collate_compare,"\x7f" "Alpha","Alpha",0);
    expect(utf8_collate_compare,"\x7f" "Alpha","alpha",0);
    expect(utf8_collate_compare,"Zürich","Ångström",1);
    expect(utf8_collate_compare,"Alpha","東京",-1);
    expect(utf8_collate_compare,"東京","Жук",1);
    expect(utf8_collate_compare,"a\xCC\x81","a",0);
    expect(utf8_collate_compare,"Ạnh","anh",0);
    expect(utf8_collate_compare,"Ếch","ech",0);
    expect(utf8_collate_compare,"cộ","co",0);
    expect(utf8_collate_compare,"\xCC\x81...Alpha","alpha",0);
    expect(utf8_collate_compare,"01","Alpha",-1);
    expect(utf8_collate_compare,"a z","az",-1);
    expect(utf8_collate_compare,"Жук","жук",0);
    expect(utf8_collate_compare,"Αθήνα","αθήνα",0);
    expect(utf8_collate_identity_compare,"Éclair","eclair",1);
    expect(utf8_collate_identity_compare,"alpha","ALPHA",0);
    expect(utf8_collate_compare,"x\xFF","x\xFE",1);
    const char *values[]={NULL,"","...","01","a","A","á","Æther","aether","!Beta",
        "東京","Жук","жук","z","Œuvre","oeuvre","x\xFF","x\xFE",
        "’Til","...and Soon","a z","az","a\xCC\x81","\xE0\x80",
        "¿Bolero yo?","¿Por qué regresa Abril?","¡...Bóléro!","\x7f" "Alpha"};
    for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) for(unsigned j=0;j<sizeof(values)/sizeof(values[0]);j++) {
        key_order(values[i],values[j],false); key_order(values[i],values[j],true);
        check_prefix_contract(values[i],values[j],1,false);
        check_prefix_contract(values[i],values[j],2,false);
        check_prefix_contract(values[i],values[j],3,false);
        check_prefix_contract(values[i],values[j],40,false);
        check_prefix_contract(values[i],values[j],1,true);
        check_prefix_contract(values[i],values[j],2,true);
        check_prefix_contract(values[i],values[j],3,true);
        check_prefix_contract(values[i],values[j],40,true);
    }
    for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) for(unsigned j=0;j<sizeof(values)/sizeof(values[0]);j++)
        for(unsigned k=0;k<sizeof(values)/sizeof(values[0]);k++)
            if (utf8_collate_compare(values[i],values[j]) <= 0 && utf8_collate_compare(values[j],values[k]) <= 0)
                assert(utf8_collate_compare(values[i],values[k]) <= 0);
    unsigned char short_key[2]; assert(!utf8_collate_prefix("long",short_key,sizeof short_key));
    puts("utf8 collation: PASS");
    return 0;
}
