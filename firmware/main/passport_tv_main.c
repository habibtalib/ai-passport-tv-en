// Standalone host: Wi-Fi/setup lifecycle only. The shared component owns playback.
#include "passport_tv.h"
#include "passport_wifi.h"
#include "folotalk_portal_access.h"
#include "lwip/inet.h"
#include "tv_config.h"
#include "pairing_client.h"
#include "ui_text.h"
#include "ui_status.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

static const char *TAG="passport_tv";
static tv_connection_t connection;
static bool configured,managed;
static char auth_origin[TV_ORIGIN_CAP];
typedef struct { tv_connection_t connection; char origin[TV_ORIGIN_CAP]; uint8_t managed; } saved_state_t;
static atomic_bool setup_saved,setup_requested,connect_requested,scan_requested;
static atomic_bool setup_active,setup_info_requested;
static TaskHandle_t owner;
static QueueHandle_t keys;
static uint8_t *stripes,*workspace,*frame_extra;
static uint8_t volume=60,brightness=80;
static SemaphoreHandle_t portal_lock;
static struct {wifi_ap_record_t scan[16];uint16_t count;bool scanning;const char *phase,*error;char ssid[33],password[65];} *portal;
static passport_wifi_profile_t inherited[PASSPORT_WIFI_MAX_NETWORKS];
static unsigned inherited_count;
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

static bool load_connection(void) {
    nvs_handle_t h;
    if(nvs_open("ptv_config",NVS_READONLY,&h)!=ESP_OK)return false;
    saved_state_t state={0};size_t n=sizeof state;
    esp_err_t e=nvs_get_blob(h,"state_v1",&state,&n);bool ok=false;
    if(e==ESP_OK && n==sizeof state){
        char normalized[TV_ORIGIN_CAP];
        ok=state.managed<=1 && (!state.managed || (memchr(state.origin,0,sizeof state.origin) &&
            tv_origin_from_input(state.origin,80,NULL,normalized) && !strcmp(state.origin,normalized)));
        if(ok){connection=state.connection;managed=state.managed;strlcpy(auth_origin,managed?state.origin:"",sizeof auth_origin);}
    }else if(e==ESP_ERR_NVS_NOT_FOUND){
        n=sizeof connection;uint8_t old_managed=0;
        ok=nvs_get_blob(h,"connection",&connection,&n)==ESP_OK && n==sizeof connection;
        esp_err_t marker=nvs_get_u8(h,"managed",&old_managed);
        if(marker!=ESP_OK && marker!=ESP_ERR_NVS_NOT_FOUND)ok=false;
        if(old_managed){
            managed=true;
            ok=ok && old_managed==1 && tv_origin_from_input("",80,pairing_client_default_origin(),auth_origin) &&
                pairing_client_migrate()==ESP_OK;
        }
    }
    nvs_get_u8(h,"volume",&volume);nvs_get_u8(h,"brightness",&brightness);nvs_close(h);
    memset(&state,0,sizeof state);
    if(volume>100)volume=60;
    if(brightness<10 || brightness>100)brightness=80;
    return ok && tv_connection_valid(&connection);
}
static esp_err_t save_connection(const tv_connection_t *c,const char *origin) {
    // A single blob prevents a power loss from mixing credentials and issuers.
    saved_state_t state={.connection=*c,.managed=1};strlcpy(state.origin,origin,sizeof state.origin);
    nvs_handle_t h;esp_err_t e=nvs_open("ptv_config",NVS_READWRITE,&h);
    if(e==ESP_OK){e=nvs_set_blob(h,"state_v1",&state,sizeof state);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
    memset(&state,0,sizeof state);return e;
}
static esp_err_t session_connection(void *context,passport_tv_connection_t *out,uint32_t timeout_ms) {
    (void)context;tv_connection_t next={.ssid="tv-session"};
    esp_err_t e=pairing_client_refresh(auth_origin,&next,timeout_ms);
    if(e==ESP_OK){strlcpy(out->host,next.host,sizeof out->host);out->port=next.port;strlcpy(out->ticket,next.token,sizeof out->ticket);}
    memset(&next,0,sizeof next);return e;
}
static void settings(uint8_t v,uint8_t b) { volume=v;brightness=b; }
static void save_settings(void) {
    nvs_handle_t h;if(nvs_open("ptv_config",NVS_READWRITE,&h)!=ESP_OK)return;
    if(nvs_set_u8(h,"volume",volume)==ESP_OK && nvs_set_u8(h,"brightness",brightness)==ESP_OK)
        (void)nvs_commit(h);
    nvs_close(h);
}
static void finished(void) { xTaskNotifyGive(owner); }
static void key_event(bsp_btn_t key,bsp_btn_ev_t ev,void *ctx) {
    (void)ctx;
    if(passport_tv_running()){passport_tv_key(key,ev);return;}
    if(key==BSP_BTN_OK && ev==BSP_BTN_LONG)atomic_store(&setup_requested,true);
    if(key==BSP_BTN_OK && ev==BSP_BTN_CLICK)(void)xQueueSend(keys,&key,0);
}
// Physical USB only: queue the same gestures as the buttons. Credentials are
// never accepted here; actual provisioning still goes through the AP/HTTP page.
static void console_command(const char *cmd) {
    bool playing=passport_tv_running();
    if(!strcmp(cmd,"state")) {
        printf("PTV_STATE setup=%u playing=%u connected=%u\n",(unsigned)atomic_load(&setup_active),
            (unsigned)playing,(unsigned)passport_wifi_is_online());
    } else if(!strcmp(cmd,"setup-info") && atomic_load(&setup_active)) {
        // The AP owner responds with its current open-network name and URL.
        atomic_store(&setup_info_requested,true);
    } else if(!strcmp(cmd,"inspect") && playing) ft_tv_inspect();
    else if(!strcmp(cmd,"stop") && playing) passport_tv_stop();
    else if(!strcmp(cmd,"start") && !playing && !atomic_load(&setup_active)) {
        bsp_btn_t key=BSP_BTN_OK;(void)xQueueSend(keys,&key,0);
    } else if(!strcmp(cmd,"setup") && !playing)atomic_store(&setup_requested,true);
    else if(playing && (!strcmp(cmd,"up") || !strcmp(cmd,"down") || !strcmp(cmd,"ok") || !strcmp(cmd,"back"))) {
        bsp_btn_t key=!strcmp(cmd,"up")?BSP_BTN_UP:!strcmp(cmd,"down")?BSP_BTN_DOWN:BSP_BTN_OK;
        passport_tv_key(key,!strcmp(cmd,"back")?BSP_BTN_LONG:BSP_BTN_CLICK);
    } else printf("PTV_ERROR unavailable\n");
    fflush(stdout);
}
static void console_task(void *arg) {
    (void)arg;char line[80];size_t used=0;bool overflow=false;
    for(;;){
        int c=getchar();
        if(c==EOF){clearerr(stdin);vTaskDelay(pdMS_TO_TICKS(50));continue;}
        if(c=='\r' || c=='\n'){
            if(!overflow && used){line[used]=0;if(!strncmp(line,"PTV ",4))console_command(line+4);}
            used=0;overflow=false;memset(line,0,sizeof line);
        } else if(c==0 || used>=sizeof line-1)overflow=true;
        else if(!overflow)line[used++]=(char)c;
    }
}
static void screen(const char *title,const char *a,const char *b,const char *c) {
    // No LVGL task or full-screen buffer; the launcher owns this static raw page.
    if(bsp_display_raw_claim()!=ESP_OK)return;
    const int soc=bsp_battery_soc();
    for(int y=0;y<240;y+=12){
        ui_surface_t s={stripes,320,y,12};
        ui_text_fill(s,0,0,320,240,0x0864);
        ui_text_fill(s,18,20,284,4,0x6e57);
        ui_text_draw(s,24,40,"PASSPORT TV",0x6e57,0x0864);
        ui_status_battery(s,296,40+(int)UI_TEXT_LINE_H/2,soc,0x0864);
        ui_text_draw(s,24,76,title,UI_COLOR_TEXT,0x0864);
        if(a)ui_text_draw(s,24,112,a,UI_COLOR_TEXT,0x0864);
        if(b)ui_text_draw(s,24,146,b,UI_COLOR_DIM,0x0864);
        if(c)ui_text_draw(s,24,180,c,UI_COLOR_DIM,0x0864);
        if(bsp_display_raw_submit_nowait(y,12,stripes)!=ESP_OK ||
           bsp_display_raw_drain(1,200)!=ESP_OK)break;
    }
    (void)bsp_display_raw_drain(32,200);(void)bsp_display_raw_release();
    bsp_display_backlight(brightness);
}
static bool local_request(httpd_req_t *r){return ft_portal_socket_allowed(httpd_req_to_sockfd(r),inet_addr("192.168.4.1"));}
static esp_err_t local_session(httpd_handle_t server,int fd){(void)server;return ft_portal_socket_allowed(fd,inet_addr("192.168.4.1"))?ESP_OK:ESP_FAIL;}
static esp_err_t setup_get(httpd_req_t *r) {
    httpd_resp_set_type(r,"text/html; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_send(r,setup_html_start,setup_html_end-setup_html_start-1);
}
static bool copy_field(cJSON *j,const char *key,char *out,size_t cap) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if(!cJSON_IsString(v) || strlen(v->valuestring)>=cap)return false;
    strlcpy(out,v->valuestring,cap);return true;
}
static bool post_allowed(httpd_req_t *r) {
    char origin[64];
    if(!local_request(r))return false;
    return !httpd_req_get_hdr_value_len(r,"Origin") ||
        (httpd_req_get_hdr_value_str(r,"Origin",origin,sizeof origin)==ESP_OK && !strcmp(origin,"http://192.168.4.1"));
}
static cJSON *json_body(httpd_req_t *r) {
    if(!post_allowed(r) || r->content_len<2 || r->content_len>2048)return NULL;
    char body[2049];size_t n=0;
    while(n<r->content_len){int got=httpd_req_recv(r,body+n,r->content_len-n);if(got<=0)return NULL;n+=(size_t)got;}
    body[n]=0;cJSON *j=cJSON_ParseWithLength(body,n+1);memset(body,0,sizeof body);return j;
}
static esp_err_t json_reply(httpd_req_t *r,cJSON *j) {
    if(!j)return ESP_ERR_NO_MEM;
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!text)return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t e=httpd_resp_sendstr(r,text);cJSON_free(text);return e;
}
static esp_err_t status_get(httpd_req_t *r) {
    cJSON *j=cJSON_CreateObject();if(!j)return ESP_ERR_NO_MEM;
    xSemaphoreTake(portal_lock,portMAX_DELAY);
    cJSON_AddStringToObject(j,"phase",portal->phase);cJSON_AddStringToObject(j,"error",portal->error);
    cJSON_AddBoolToObject(j,"connected",passport_wifi_is_online());cJSON_AddStringToObject(j,"ssid",connection.ssid);
    cJSON_AddStringToObject(j,"host",managed?auth_origin:connection.host);
    cJSON_AddNumberToObject(j,"port",!managed && connection.port?connection.port:10745);
    cJSON_AddStringToObject(j,"default_origin",pairing_client_default_origin());
    cJSON_AddBoolToObject(j,"managed",managed);
    cJSON_AddBoolToObject(j,"has_token",connection.token[0]);xSemaphoreGive(portal_lock);
    return json_reply(r,j);
}
static esp_err_t networks_get(httpd_req_t *r) {
    cJSON *j=cJSON_CreateObject(),*entries=cJSON_CreateArray();if(!j || !entries){cJSON_Delete(j);cJSON_Delete(entries);return ESP_ERR_NO_MEM;}
    cJSON_AddItemToObject(j,"entries",entries);xSemaphoreTake(portal_lock,portMAX_DELAY);
    cJSON_AddBoolToObject(j,"scanning",portal->scanning);
    for(unsigned i=0;i<portal->count;i++){
        char hex[65];size_t n=strnlen((char*)portal->scan[i].ssid,32);
        for(size_t k=0;k<n;k++){snprintf(hex+2*k,3,"%02x",portal->scan[i].ssid[k]);}
        hex[2*n]=0;
        cJSON *row=cJSON_CreateObject();if(!row)continue;
        cJSON_AddStringToObject(row,"ssid_hex",hex);cJSON_AddNumberToObject(row,"rssi",portal->scan[i].rssi);cJSON_AddItemToArray(entries,row);
    }
    xSemaphoreGive(portal_lock);return json_reply(r,j);
}
static esp_err_t scan_post(httpd_req_t *r) {
    if(!post_allowed(r))return httpd_resp_send_err(r,HTTPD_403_FORBIDDEN,"origin");
    atomic_store(&scan_requested,true);return httpd_resp_sendstr(r,"{}");
}
static esp_err_t connect_post(httpd_req_t *r) {
    cJSON *j=json_body(r);char ssid[33],password[65];
    bool ok=j && copy_field(j,"ssid",ssid,sizeof ssid) && copy_field(j,"password",password,sizeof password);
    cJSON_Delete(j);tv_connection_t check={.host="localhost",.port=10745,.token="validation-placeholder"};
    if(ok){strlcpy(check.ssid,ssid,sizeof check.ssid);strlcpy(check.password,password,sizeof check.password);ok=tv_connection_valid(&check);}
    memset(&check,0,sizeof check);
    if(!ok)return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"password");
    xSemaphoreTake(portal_lock,portMAX_DELAY);
    if(!strcmp(portal->phase,"connecting")){xSemaphoreGive(portal_lock);return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"busy");}
    strlcpy(portal->ssid,ssid,sizeof portal->ssid);strlcpy(portal->password,password,sizeof portal->password);
    portal->phase="connecting";portal->error="";atomic_store(&connect_requested,true);xSemaphoreGive(portal_lock);memset(password,0,sizeof password);
    return httpd_resp_sendstr(r,"{}");
}
static esp_err_t setup_post(httpd_req_t *r) {
    if(!post_allowed(r))return httpd_resp_send_err(r,HTTPD_403_FORBIDDEN,"origin");
    if(!passport_wifi_is_online())return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"network_first");
    xSemaphoreTake(portal_lock,portMAX_DELAY);
    bool saved=!strcmp(portal->phase,"saved"),ready=!strcmp(portal->phase,"network_saved");
    xSemaphoreGive(portal_lock);
    if(saved)return httpd_resp_sendstr(r,"{}");
    if(!ready)return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"network_first");
    cJSON *j=json_body(r);tv_connection_t next=connection;
    char input[TV_ORIGIN_CAP]={0},origin[TV_ORIGIN_CAP]={0},code[5]={0};
    const cJSON *port=cJSON_GetObjectItemCaseSensitive(j,"port");
    bool ok=j && copy_field(j,"host",input,sizeof input) && copy_field(j,"code",code,sizeof code) &&
        cJSON_IsNumber(port) && port->valuedouble>=1 && port->valuedouble<=65535 && port->valuedouble==port->valueint &&
        tv_origin_from_input(input,(uint16_t)port->valueint,pairing_client_default_origin(),origin);
    cJSON_Delete(j);
    // A legacy long-token configuration can be retained, but never sent to an
    // activation endpoint or silently reused for another playback destination.
    char legacy_origin[TV_ORIGIN_CAP];
    bool legacy=ok && !code[0] && !managed && configured &&
        tv_origin_from_input(connection.host,connection.port,NULL,legacy_origin) && !strcmp(origin,legacy_origin);
    bool pending_used=false;
    esp_err_t e=ok?(legacy?ESP_OK:pairing_client_pair(origin,code,&next,&pending_used)):ESP_ERR_INVALID_ARG;
    if(e==ESP_OK && legacy){
        // Network credentials may have changed; persist the static legacy tuple.
        saved_state_t state={.connection=next};nvs_handle_t h;e=nvs_open("ptv_config",NVS_READWRITE,&h);
        if(e==ESP_OK){e=nvs_set_blob(h,"state_v1",&state,sizeof state);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
        memset(&state,0,sizeof state);
    }else if(e==ESP_OK){
        e=save_connection(&next,origin);
        if(e==ESP_OK && pending_used)e=pairing_client_committed(origin);
    }
    if(e==ESP_OK){
        xSemaphoreTake(portal_lock,portMAX_DELAY);connection=next;managed=!legacy;
        strlcpy(auth_origin,legacy?"":origin,sizeof auth_origin);portal->phase="saved";xSemaphoreGive(portal_lock);
    }
    memset(&next,0,sizeof next);memset(code,0,sizeof code);
    if(e!=ESP_OK)return httpd_resp_send_err(r,e==ESP_ERR_INVALID_ARG?HTTPD_400_BAD_REQUEST:HTTPD_500_INTERNAL_SERVER_ERROR,
        e==ESP_ERR_INVALID_ARG?"fields":"pairing");
    return httpd_resp_sendstr(r,"{}");
}
static esp_err_t finish_post(httpd_req_t *r) {
    if(!post_allowed(r))return httpd_resp_send_err(r,HTTPD_403_FORBIDDEN,"origin");
    xSemaphoreTake(portal_lock,portMAX_DELAY);bool saved=!strcmp(portal->phase,"saved");xSemaphoreGive(portal_lock);
    if(!saved)return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"network_first");
    esp_err_t e=httpd_resp_sendstr(r,"{}");atomic_store(&setup_saved,true);return e;
}
static bool configure(void) {
    passport_wifi_auto_reconnect(false);portal=calloc(1,sizeof *portal);portal_lock=xSemaphoreCreateMutex();
    if(!portal || !portal_lock){free(portal);portal=NULL;if(portal_lock)vSemaphoreDelete(portal_lock);return false;}
    portal->phase=passport_wifi_is_online()?"network_saved":"idle";portal->error="";
    char name[33];uint8_t mac[6];
    esp_err_t ready=passport_wifi_provision_prepare();
    if(ready!=ESP_OK || esp_efuse_mac_get_default(mac)!=ESP_OK){free(portal);vSemaphoreDelete(portal_lock);portal=NULL;return false;}
    snprintf(name,sizeof name,"PassportTV-%02X%02X-%03u",mac[4],mac[5],(unsigned)(esp_random()%1000));
    atomic_store(&setup_saved,false);atomic_store(&scan_requested,true);atomic_store(&connect_requested,false);
    httpd_handle_t http=NULL;bool ok=false;
    if(passport_wifi_setup_ap(name)!=ESP_OK)goto done;
    httpd_config_t hc=HTTPD_DEFAULT_CONFIG();hc.max_open_sockets=3;hc.lru_purge_enable=true;hc.stack_size=10240;hc.max_uri_handlers=8;hc.open_fn=local_session;
    if(httpd_start(&http,&hc)!=ESP_OK)goto done;
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=setup_get},{.uri="/status",.method=HTTP_GET,.handler=status_get},
        {.uri="/networks",.method=HTTP_GET,.handler=networks_get},{.uri="/networks",.method=HTTP_POST,.handler=scan_post},
        {.uri="/connect",.method=HTTP_POST,.handler=connect_post},{.uri="/configure",.method=HTTP_POST,.handler=setup_post},
        {.uri="/finish",.method=HTTP_POST,.handler=finish_post}};
    for(unsigned i=0;i<sizeof routes/sizeof *routes;i++)if(httpd_register_uri_handler(http,&routes[i])!=ESP_OK)goto done;
    screen("Join this hotspot on your phone",name,"No password needed","Open 192.168.4.1 in browser");
    atomic_store(&setup_info_requested,false);atomic_store(&setup_active,true);
    while(!atomic_load(&setup_saved)){
        if(atomic_exchange(&setup_info_requested,false)){printf("PTV_SETUP ssid=%s code=- url=http://192.168.4.1 auth=open\n",name);fflush(stdout);}
        if(atomic_exchange(&scan_requested,false)){
            xSemaphoreTake(portal_lock,portMAX_DELAY);portal->scanning=true;xSemaphoreGive(portal_lock);
            wifi_scan_config_t scan={.show_hidden=true,.scan_time.active={.min=40,.max=80},.home_chan_dwell_time=120};
            esp_err_t e=esp_wifi_scan_start(&scan,true);xSemaphoreTake(portal_lock,portMAX_DELAY);portal->count=16;
            if(e!=ESP_OK || esp_wifi_scan_get_ap_records(&portal->count,portal->scan)!=ESP_OK)portal->count=0;
            esp_wifi_clear_ap_list();portal->scanning=false;xSemaphoreGive(portal_lock);
        }
        if(atomic_exchange(&connect_requested,false)){
            passport_wifi_profile_t profile;xSemaphoreTake(portal_lock,portMAX_DELAY);
            strlcpy(profile.ssid,portal->ssid,sizeof profile.ssid);strlcpy(profile.password,portal->password,sizeof profile.password);xSemaphoreGive(portal_lock);
            esp_err_t e=passport_wifi_connect(&profile,1,20000,NULL);xSemaphoreTake(portal_lock,portMAX_DELAY);
            if(e==ESP_OK){strlcpy(connection.ssid,profile.ssid,sizeof connection.ssid);strlcpy(connection.password,profile.password,sizeof connection.password);portal->phase="network_saved";portal->error="";}
            else{portal->phase="error";portal->error="wifi";}
            memset(&profile,0,sizeof profile);memset(portal->password,0,sizeof portal->password);xSemaphoreGive(portal_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(300));ok=true;
 done:
    atomic_store(&setup_active,false);if(http)httpd_stop(http);passport_wifi_provision_cleanup();
    memset(portal,0,sizeof *portal);free(portal);portal=NULL;vSemaphoreDelete(portal_lock);portal_lock=NULL;
    passport_wifi_auto_reconnect(ok);return ok;
}
static void inherit_wifi_readonly(void) {
    // Borrow remembered networks only. Never save to FoloTalk's namespace.
    nvs_handle_t h;if(nvs_open("folotalk",NVS_READONLY,&h)!=ESP_OK)return;
    size_t bytes=sizeof inherited;
    if(nvs_get_blob(h,"wifi_history_v1",inherited,&bytes)==ESP_OK && bytes==sizeof inherited){
        while(inherited_count<PASSPORT_WIFI_MAX_NETWORKS && inherited[inherited_count].ssid[0]){
            if(!memchr(inherited[inherited_count].ssid,0,33) || !memchr(inherited[inherited_count].password,0,65))break;
            inherited_count++;
        }
    }
    if(!inherited_count){
        size_t n=0;
        if(nvs_get_blob(h,"profile",NULL,&n)==ESP_OK && n>=98 && n<=8192){
            uint8_t *profile=malloc(n);
            if(profile && nvs_get_blob(h,"profile",profile,&n)==ESP_OK){memcpy(inherited,profile,98);if(memchr(inherited[0].ssid,0,33)&&memchr(inherited[0].password,0,65)&&inherited[0].ssid[0])inherited_count=1;}
            if(profile){memset(profile,0,n);free(profile);}
        }
    }
    nvs_close(h);
}
static bool connect_station(void) {
    passport_wifi_auto_reconnect(false);passport_wifi_profile_t profiles[PASSPORT_WIFI_MAX_NETWORKS];unsigned count=0;
    if(connection.ssid[0]){strlcpy(profiles[0].ssid,connection.ssid,33);strlcpy(profiles[0].password,connection.password,65);count=1;}
    for(unsigned i=0;i<inherited_count && count<PASSPORT_WIFI_MAX_NETWORKS;i++){
        bool duplicate=false;for(unsigned k=0;k<count;k++)if(!strcmp(profiles[k].ssid,inherited[i].ssid))duplicate=true;
        if(!duplicate)profiles[count++]=inherited[i];
    }
    unsigned selected=0;esp_err_t e=count?passport_wifi_connect(profiles,count,12000,&selected):ESP_ERR_NOT_FOUND;
    if(e==ESP_OK){strlcpy(connection.ssid,profiles[selected].ssid,33);strlcpy(connection.password,profiles[selected].password,65);passport_wifi_auto_reconnect(true);}
    memset(profiles,0,sizeof profiles);return e==ESP_OK;
}
static void launcher(void *arg) {
    (void)arg;bool autoplay=true;
    if(connection.ssid[0]){screen("Connecting to saved network",NULL,NULL,NULL);(void)connect_station();}
    for(;;){
        if(!configured || atomic_exchange(&setup_requested,false)){
            configured=configure();autoplay=true;
            if(!configured){screen("Setup failed to start",NULL,"Retrying shortly",NULL);vTaskDelay(pdMS_TO_TICKS(3000));continue;}
        }
        if(!passport_wifi_is_online()){
            screen("Connecting to Wi-Fi",connection.ssid,NULL,"Hold OK to change settings");
            if(!connect_station()){
                screen("Wi-Fi connection failed",NULL,"OK: retry | Hold OK: setup",NULL);
                while(!passport_wifi_is_online() && !atomic_load(&setup_requested)){
                    bsp_btn_t retry;
                    if(xQueueReceive(keys,&retry,pdMS_TO_TICKS(100))==pdTRUE)break;
                }
                autoplay=true;continue;
            }
        }
        bsp_btn_t key;
        if(autoplay || xQueueReceive(keys,&key,pdMS_TO_TICKS(100))==pdTRUE){
            autoplay=false;
            if(!passport_wifi_is_online()){
                continue;
            }
            xQueueReset(keys);(void)ulTaskNotifyTake(pdTRUE,0);
            passport_tv_config_t c={.host=connection.host,.port=connection.port,.ticket=connection.token,
                .stripes=stripes,.stripes_bytes=FT_TV_STRIPE_BYTES,.control=workspace,.control_bytes=FT_TV_WORKSPACE_BYTES,
                .frame_extra=frame_extra,.frame_extra_bytes=16384,.volume=volume,.brightness=brightness,
                .done=finished,.settings=settings,.connection_provider=managed?session_connection:NULL,
                .session_task_stack_bytes=managed?10240:0};
            esp_err_t e=passport_tv_start(&c);
            if(e!=ESP_OK){ESP_LOGE(TAG,"Player start failed: %s",esp_err_to_name(e));screen("Playback failed to start",NULL,"OK: retry | Hold OK: setup",NULL);continue;}
            (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);save_settings();xQueueReset(keys);
            screen("TV paused",NULL,"Press OK to resume","Hold OK for connection setup");
        }
    }
}
void app_main(void) {
    // Never erase NVS or touch FoloTalk recovery/photo/account namespaces.
    ESP_ERROR_CHECK(nvs_flash_init());
    keys=xQueueCreate(4,sizeof(bsp_btn_t));assert(keys);
    ESP_ERROR_CHECK(bsp_display_init());ESP_ERROR_CHECK(bsp_audio_init());
    if(bsp_battery_init()!=ESP_OK)ESP_LOGW(TAG,"Battery gauge unavailable");
    stripes=heap_caps_malloc(FT_TV_STRIPE_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    workspace=heap_caps_malloc(FT_TV_WORKSPACE_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    frame_extra=heap_caps_malloc(16384,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    assert(stripes && workspace && frame_extra);
    configured=load_connection();inherit_wifi_readonly();
    if(!connection.ssid[0] && inherited_count){strlcpy(connection.ssid,inherited[0].ssid,33);strlcpy(connection.password,inherited[0].password,65);}
    ESP_ERROR_CHECK(bsp_button_init(key_event,NULL));
    assert(xTaskCreate(launcher,"ptv_host",4096,NULL,2,&owner)==pdPASS);
    assert(xTaskCreate(console_task,"ptv_usb",3072,NULL,1,NULL)==pdPASS);
}
