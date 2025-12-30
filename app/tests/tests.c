// #include "test_camera_req.h"

#include <curl/curl.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>

// Toujours commenter : on teste la fonction de ta lib (pas de duplication)
#include "../camera_requests/inc/camera_req.h"
#include "tests.h"

// Toujours commenter : buffer mémoire pour récupérer la réponse HTTP
typedef struct
{
    char *memory;
    size_t size;
} MemoryStruct;

// Toujours commenter : callback libcurl pour accumuler la réponse
static size_t write_mem_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr)
        return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';
    return realsize;
}

// Toujours commenter : log en chunks pour éviter troncature syslog
static void log_raw_chunks(const char *tag, const char *s)
{
    if (!s)
    {
        syslog(LOG_INFO, "%s: (null)", tag ? tag : "TEST");
        return;
    }

    const size_t CHUNK = 900;
    size_t len = strlen(s);

    syslog(LOG_INFO, "%s: raw response (len=%zu) BEGIN", tag ? tag : "TEST", len);

    for (size_t off = 0; off < len; off += CHUNK)
    {
        char buf[CHUNK + 1];
        size_t n = len - off;
        if (n > CHUNK) n = CHUNK;

        memcpy(buf, s + off, n);
        buf[n] = '\0';

        syslog(LOG_INFO, "%s: %s", tag ? tag : "TEST", buf);
    }

    syslog(LOG_INFO, "%s: raw response END", tag ? tag : "TEST");
}

// Toujours commenter : petite requête curl “getConfiguration” (DigestAuth)
static int curl_get_configuration_json(const char *ip, const char *user, const char *pass, char **out_json, long *out_http)
{
    if (!ip || !*ip || !user || !pass || !out_json)
        return 0;

    *out_json = NULL;
    if (out_http) *out_http = 0;

    char url[256];
    snprintf(url, sizeof url, "http://%s/local/objectanalytics/control.cgi", ip);

    const char *body =
        "{"
        "\"apiVersion\":\"1.6\","
        "\"context\":\"acap\","
        "\"method\":\"getConfiguration\""
        "}";

    CURL *curl = curl_easy_init();
    if (!curl)
        return 0;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    MemoryStruct chunk;
    chunk.memory = NULL;
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, pass);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (out_http) *out_http = code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || code != 200 || !chunk.memory || chunk.size == 0)
    {
        free(chunk.memory);
        return 0;
    }

    *out_json = chunk.memory; // ownership caller
    return 1;
}

void test_camera_req_list_scenarios(const char *ip, const char *user, const char *pass)
{
    // Toujours commenter : syslog tag pour retrouver facilement les logs
    syslog(LOG_INFO, "TEST(camera_req): start ip=%s", ip ? ip : "(null)");

    long http = 0;
    char *json = NULL;

    if (!curl_get_configuration_json(ip, user, pass, &json, &http))
    {
        syslog(LOG_WARNING, "TEST(camera_req): curl getConfiguration failed (HTTP=%ld)", http);
        return;
    }

    syslog(LOG_INFO, "TEST(camera_req): got JSON (len=%zu, HTTP=%ld)", strlen(json), http);

    // Toujours commenter : debug brut (désactive si trop verbeux)
    log_raw_chunks("TEST(camera_req)", "UNCOMMENT ON NEED");
    // log_raw_chunks("TEST(camera_req)", json);

    // Toujours commenter : ICI on teste ta fonction cible
    aoa_scenario_t out[128];
    int n = aoa_list_valid_scenarios_from_controlcgi_json(json, out, (int)(sizeof(out) / sizeof(out[0])));

    syslog(LOG_INFO, "TEST(camera_req): aoa_list_valid_scenarios... => n=%d", n);

    syslog(LOG_INFO, "--------------------------------------------------------------------------");
    syslog(LOG_INFO, "| %-9s | %-20s | %-6s | %-5s | %-12s |", "scenarioId", "zone_name", "zoneId", "INOUT", "direction");
    syslog(LOG_INFO, "--------------------------------------------------------------------------");

    for (int i = 0; i < n; i++)
    {
        syslog(LOG_INFO, "| %-9d | %-20.20s | %-6d | %-5s | %-12s |",
               out[i].scenario_id,
               out[i].zone_name,
               out[i].zone_id,
               aoa_dir_to_inout(out[i].dir),
               aoa_dir_to_string(out[i].dir));
    }

    syslog(LOG_INFO, "--------------------------------------------------------------------------");

    free(json);
    syslog(LOG_INFO, "TEST(camera_req): done");
}
