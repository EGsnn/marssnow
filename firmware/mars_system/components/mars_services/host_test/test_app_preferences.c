#include "mars_app_preferences.h"
#include "nvs.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
static pthread_t caller;
static uint32_t committed, pending;
static int exists, fault, closes, opens;
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h)
{assert(!pthread_equal(caller,pthread_self()));assert(!strcmp(ns,"app_marsman"));++opens;if(fault==1)return 99;if(!exists && mode==NVS_READONLY)return 2;*h=1;return 0;}
esp_err_t nvs_get_u32(nvs_handle_t h,const char *k,uint32_t *v)
{assert(h==1 && !strcmp(k,"voice"));if(!exists)return 2;*v=committed;return 0;}
esp_err_t nvs_set_u32(nvs_handle_t h,const char *k,uint32_t v)
{assert(h==1 && !strcmp(k,"voice"));pending=v;return fault==2?99:0;}
esp_err_t nvs_commit(nvs_handle_t h)
{assert(h==1);if(fault==3)return 99;committed=pending;exists=1;return 0;}
void nvs_close(nvs_handle_t h){assert(h==1);++closes;}
static void *read_many(void *arg)
{
    (void)arg;
    for(int i=0;i<100;++i){uint32_t v=0;assert(!mars_app_pref_get_u32("app_marsman","voice",&v) && v==2);}
    return NULL;
}
int main(void)
{
    caller=pthread_self();
    assert(!mars_app_preferences_init());
    uint32_t v=7;
    assert(mars_app_pref_get_u32("app_marsman","voice",&v)==-2 && v==7);
    assert(!mars_app_pref_set_u32("app_marsman","voice",2));
    assert(!mars_app_pref_get_u32("app_marsman","voice",&v) && v==2);
    for(fault=1;fault<=3;++fault)assert(mars_app_pref_set_u32("app_marsman","voice",1)==-5);
    fault=0;assert(!mars_app_pref_get_u32("app_marsman","voice",&v) && v==2);
    int before=opens;
    assert(mars_app_pref_get_u32("websocket","voice",&v)==-22);
    assert(mars_app_pref_get_u32("app_marsman","voice",NULL)==-22);
    assert(mars_app_pref_set_u32(NULL,"voice",0)==-22);
    assert(mars_app_pref_set_u32("app_","voice",0)==-22);
    assert(mars_app_pref_set_u32("app_marsman","../token",0)==-22);
    assert(mars_app_pref_set_u32("app_marsman","abcdefghijklmnop",0)==-22);
    assert(opens==before && closes==5);
    pthread_t readers[3];
    for(int i=0;i<3;++i)assert(!pthread_create(&readers[i],NULL,read_many,NULL));
    for(int i=0;i<3;++i)pthread_join(readers[i],NULL);
    puts("PASS: app-only namespaces, stored readback, missing value, validation open/set/commit failures, separate NVS worker and 300 concurrent reads");
}
