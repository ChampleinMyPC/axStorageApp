#include <curl/curl.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include "include/jsmn.h"
static void aoa_log_configuration_scenarios(const char *ip_or_host, const char *user, const char *pass);
void aoa_log_scenarios_table_from_json(const char *json);
void aoa_log_raw_response(const char *tag, const char *s);
void aoa_log_scenarios_table_jsmn(const char *json);
// Toujours commenter : buffer mémoire pour récupérer la réponse HTTP (déjà similaire chez toi)
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

// Toujours commenter : POST JSON-RPC getConfiguration sur control.cgi (DigestAuth)
static gboolean aoa_get_configuration_json(const char *url,
                                          const char *user,
                                          const char *pass,
                                          char **out_json,
                                          long *out_http_code)
{
    if (!url || !user || !pass || !out_json)
        return FALSE;

    *out_json = NULL;
    if (out_http_code)
        *out_http_code = 0;

    CURL *curl = curl_easy_init();
    if (!curl)
        return FALSE;

    // Toujours commenter : corps JSON-RPC minimal pour getConfiguration
    const char *body =
        "{"
        "\"apiVersion\":\"1.6\","
        "\"context\":\"acap\","
        "\"method\":\"getConfiguration\""
        "}";

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    MemoryStruct chunk = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);

    // Toujours commenter : JSON-RPC => POST
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    // Toujours commenter : Digest auth comme dans Postman
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, pass);

    // Toujours commenter : lecture réponse
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    // Toujours commenter : timeouts pour éviter de bloquer ton main loop
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (out_http_code)
        *out_http_code = code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || code != 200 || !chunk.memory || chunk.size == 0)
    {
        free(chunk.memory);
        return FALSE;
    }

    *out_json = chunk.memory; // ownership donné au caller
    return TRUE;
}

// 
// Toujours commenter : extraction simple des scénarios depuis la réponse JSON brute
// Objectif : log id / name / countingDirection sans dépendance externe
// Toujours commenter : trouve la fin d'un objet JSON { ... } en gérant les strings et l'imbrication
// Toujours commenter : trouve la fin d'un objet JSON { ... } en gérant les strings et l'imbrication
static const char *find_json_object_end(const char *p)
{
    int depth = 0;
    int in_str = 0;
    int esc = 0;

    for (; *p; p++)
    {
        char c = *p;

        if (in_str)
        {
            // Toujours commenter : gestion des échappements dans les strings
            if (esc) { esc = 0; continue; }
            if (c == '\\') { esc = 1; continue; }
            if (c == '"') in_str = 0;
            continue;
        }

        if (c == '"') { in_str = 1; continue; }
        if (c == '{') { depth++; continue; }
        if (c == '}')
        {
            depth--;
            if (depth == 0) return p; // fin de l'objet racine courant
        }
    }
    return NULL;
}


// Toujours commenter : extraction simple mais correcte des scénarios depuis la réponse JSON brute
void aoa_log_scenarios_table_from_json(const char *json)
{
    if (!json) return;

    // Toujours commenter : on se positionne sur le tableau "scenarios"
    const char *p = strstr(json, "\"scenarios\"");
    if (!p)
    {
        syslog(LOG_WARNING, "AOA: missing scenarios");
        return;
    }

    // Toujours commenter : trouver le '[' qui commence le tableau
    p = strchr(p, '[');
    if (!p) return;
    p++; // après '['

    syslog(LOG_INFO, "--------------------------------------------");
    syslog(LOG_INFO, "| id   | name                     | direction          |");
    syslog(LOG_INFO, "--------------------------------------------");

    // Toujours commenter : itération sur chaque objet { ... } du tableau scenarios
    while (*p)
    {
        // Toujours commenter : fin du tableau
        if (*p == ']') break;

        // Toujours commenter : chercher le début d'un objet scénario
        const char *obj = strchr(p, '{');
        if (!obj) break;

        const char *end = find_json_object_end(obj);
        if (!end) break;

        int id = -1;
        char name[64] = {0};
        char direction[32] = "-";

        // Toujours commenter : extraction dans la sous-chaîne [obj..end]
        // -> on copie une fenêtre pour ne pas matcher en dehors du scénario
        size_t len = (size_t)(end - obj + 1);
        char *win = (char *)malloc(len + 1);
        if (!win) break;

        memcpy(win, obj, len);
        win[len] = '\0';

        // Toujours commenter : ici "id" est bien scenario.id (pas device.id) car on est dans l'objet scénario
        const char *id_ptr = strstr(win, "\"id\"");
        if (id_ptr)
            sscanf(id_ptr, "\"id\"%*[^0-9]%d", &id);

        const char *name_ptr = strstr(win, "\"name\"");
        if (name_ptr)
            sscanf(name_ptr, "\"name\"%*[^\"\"]\"%63[^\"]\"", name);

        const char *dir_ptr = strstr(win, "\"countingDirection\"");
        if (dir_ptr)
            sscanf(dir_ptr, "\"countingDirection\"%*[^\"\"]\"%31[^\"]\"", direction);

        syslog(LOG_INFO, "| %-4d | %-24.24s | %-18.18s |",
               id,
               name[0] ? name : "(no-name)",
               direction);

        free(win);

        // Toujours commenter : on continue après l'objet courant
        p = end + 1;
    }

    syslog(LOG_INFO, "--------------------------------------------");
}// Toujours commenter : log la réponse JSON brute en morceaux (évite troncature syslog)
void aoa_log_raw_response(const char *tag, const char *s)
{
    if (!s)
    {
        syslog(LOG_INFO, "%s: (null)", tag ? tag : "AOA");
        return;
    }

    const char *prefix = tag ? tag : "AOA";
    const size_t CHUNK = 900; // Toujours commenter : marge pour éviter les limites syslog
    size_t len = strlen(s);

    syslog(LOG_INFO, "%s: raw response (len=%zu) BEGIN", prefix, len);

    for (size_t off = 0; off < len; off += CHUNK)
    {
        // Toujours commenter : on coupe proprement sans modifier la source
        char buf[CHUNK + 1];
        size_t n = len - off;
        if (n > CHUNK) n = CHUNK;

        memcpy(buf, s + off, n);
        buf[n] = '\0';

        syslog(LOG_INFO, "%s: %s", prefix, buf);
    }

    syslog(LOG_INFO, "%s: raw response END", prefix);
}


// Toujours commenter : compare un token jsmn (string) avec une clé C
static int jsoneq(const char *json, const jsmntok_t *tok, const char *s)
{
    // Toujours commenter : token doit être une string et avoir la même longueur que s
    if (tok->type != JSMN_STRING)
        return -1;

    int tok_len = tok->end - tok->start;
    int s_len = (int)strlen(s);

    if (tok_len == s_len && strncmp(json + tok->start, s, (size_t)tok_len) == 0)
        return 0;

    return -1;
}

// Toujours commenter : calcule l'index du token "suivant" (saute récursivement un token + ses enfants)
static int tok_next(const jsmntok_t *toks, int i)
{
    int j;

    switch (toks[i].type)
    {
        case JSMN_PRIMITIVE:
        case JSMN_STRING:
            return i + 1;

        case JSMN_ARRAY:
            // Toujours commenter : sauter tous les éléments du tableau
            j = i + 1;
            for (int k = 0; k < toks[i].size; k++)
                j = tok_next(toks, j);
            return j;

        case JSMN_OBJECT:
            // Toujours commenter : sauter toutes les paires clé/valeur
            j = i + 1;
            for (int k = 0; k < toks[i].size; k++)
            {
                // clé
                j = tok_next(toks, j);
                // valeur
                j = tok_next(toks, j);
            }
            return j;

        default:
            return i + 1;
    }
}

// Toujours commenter : récupère la valeur d'une clé dans un objet (retourne l'index du token valeur, sinon -1)
static int obj_get(const char *json, const jsmntok_t *toks, int obj_i, const char *key)
{
    if (toks[obj_i].type != JSMN_OBJECT)
        return -1;

    // Toujours commenter : dans un objet, les enfants sont: key, value, key, value, ...
    int i = obj_i + 1;
    for (int p = 0; p < toks[obj_i].size; p++)
    {
        int key_i = i;
        int val_i = i + 1;

        if (jsoneq(json, &toks[key_i], key) == 0)
            return val_i;

        // Toujours commenter : avancer de (key + value) en sautant correctement les sous-structures
        i = tok_next(toks, val_i);
    }
    return -1;
}

// Toujours commenter : extrait un int depuis un token PRIMITIVE (ex: 123)
static int tok_to_int(const char *json, const jsmntok_t *tok, int *out)
{
    if (tok->type != JSMN_PRIMITIVE)
        return 0;

    // Toujours commenter : on copie dans un buffer pour atoi
    int len = tok->end - tok->start;
    if (len <= 0 || len > 31)
        return 0;

    char buf[32];
    memcpy(buf, json + tok->start, (size_t)len);
    buf[len] = '\0';

    *out = atoi(buf);
    return 1;
}

// Toujours commenter : copie une string token dans un buffer C (en tronquant si besoin)
static void tok_to_cstr(const char *json, const jsmntok_t *tok, char *dst, size_t dst_sz)
{
    if (!dst || dst_sz == 0)
        return;

    dst[0] = '\0';

    if (tok->type != JSMN_STRING)
        return;

    int len = tok->end - tok->start;
    if (len <= 0)
        return;

    size_t n = (size_t)len;
    if (n >= dst_sz)
        n = dst_sz - 1;

    memcpy(dst, json + tok->start, n);
    dst[n] = '\0';
}

// Toujours commenter : parse la réponse getConfiguration et log id/name/direction par scénario
void aoa_log_scenarios_table_jsmn(const char *json)
{
    if (!json)
    {
        syslog(LOG_WARNING, "AOA: json is NULL");
        return;
    }

    // Toujours commenter : taille à ajuster si la réponse grandit
    enum { MAX_TOKENS = 4096 };

    jsmn_parser p;
    jsmntok_t *toks = (jsmntok_t *)calloc(MAX_TOKENS, sizeof(jsmntok_t));
    if (!toks)
    {
        syslog(LOG_WARNING, "AOA: calloc tokens failed");
        return;
    }

    jsmn_init(&p);
    int r = jsmn_parse(&p, json, strlen(json), toks, MAX_TOKENS);

    if (r < 0)
    {
        // Toujours commenter : -1 = no tokens, -2 = invalid json, -3 = not enough tokens (selon jsmn)
        syslog(LOG_WARNING, "AOA: jsmn_parse failed (r=%d). If r==-3 increase MAX_TOKENS", r);
        free(toks);
        return;
    }

    // Toujours commenter : root doit être un objet
    if (r == 0 || toks[0].type != JSMN_OBJECT)
    {
        syslog(LOG_WARNING, "AOA: root is not an object");
        free(toks);
        return;
    }

    // Toujours commenter : naviguer root.data.scenarios
    int data_i = obj_get(json, toks, 0, "data");
    if (data_i < 0 || toks[data_i].type != JSMN_OBJECT)
    {
        syslog(LOG_WARNING, "AOA: missing/invalid data");
        free(toks);
        return;
    }

    int scenarios_i = obj_get(json, toks, data_i, "scenarios");
    if (scenarios_i < 0 || toks[scenarios_i].type != JSMN_ARRAY)
    {
        syslog(LOG_WARNING, "AOA: missing/invalid data.scenarios");
        free(toks);
        return;
    }

    syslog(LOG_INFO, "-----------------------de JSMN---------------------");
    syslog(LOG_INFO, "| id   | name                     | direction          |");
    syslog(LOG_INFO, "--------------------------------------------");

    // Toujours commenter : itérer sur chaque élément (objet scénario) du tableau
    int idx = scenarios_i + 1;
    for (int s = 0; s < toks[scenarios_i].size; s++)
    {
        int sc_i = idx;

        // Toujours commenter : sécurité, on attend un objet scénario
        if (toks[sc_i].type != JSMN_OBJECT)
        {
            idx = tok_next(toks, sc_i);
            continue;
        }

        int id = -1;
        char name[64] = {0};
        char direction[64] = "-"; // peut contenir "a,b,c"

        // Toujours commenter : scenario.id
        int id_i = obj_get(json, toks, sc_i, "id");
        if (id_i >= 0)
            (void)tok_to_int(json, &toks[id_i], &id);

        // Toujours commenter : scenario.name
        int name_i = obj_get(json, toks, sc_i, "name");
        if (name_i >= 0)
            tok_to_cstr(json, &toks[name_i], name, sizeof name);

        // Toujours commenter : scenario.triggers[*].countingDirection ou alarmDirection
        int triggers_i = obj_get(json, toks, sc_i, "triggers");
        if (triggers_i >= 0 && toks[triggers_i].type == JSMN_ARRAY && toks[triggers_i].size > 0)
        {
            // Toujours commenter : concat directions dans un buffer simple
            direction[0] = '\0';

            int t_idx = triggers_i + 1;
            for (int t = 0; t < toks[triggers_i].size; t++)
            {
                int trig_i = t_idx;

                if (toks[trig_i].type == JSMN_OBJECT)
                {
                    int cd_i = obj_get(json, toks, trig_i, "countingDirection");
                    if (cd_i < 0)
                        cd_i = obj_get(json, toks, trig_i, "alarmDirection");

                    if (cd_i >= 0 && toks[cd_i].type == JSMN_STRING)
                    {
                        char tmp[32];
                        tok_to_cstr(json, &toks[cd_i], tmp, sizeof tmp);

                        // Toujours commenter : concat avec virgule si déjà présent
                        if (tmp[0])
                        {
                            if (direction[0] && strlen(direction) + 1 < sizeof direction)
                                strncat(direction, ",", sizeof(direction) - strlen(direction) - 1);

                            strncat(direction, tmp, sizeof(direction) - strlen(direction) - 1);
                        }
                    }
                }

                t_idx = tok_next(toks, trig_i);
            }

            if (!direction[0])
                strncpy(direction, "-", sizeof direction);
        }

        syslog(LOG_INFO, "| %-4d | %-24.24s | %-18.18s |",
               id,
               name[0] ? name : "(no-name)",
               direction);

        // Toujours commenter : avancer au scénario suivant
        idx = tok_next(toks, sc_i);
    }

    syslog(LOG_INFO, "--------------------------------------------");

    free(toks);
}

// Wrapper : appelle l’API et log la table
static void aoa_log_configuration_scenarios(const char *ip_or_host, const char *user, const char *pass)
{
    // Toujours commenter : si tu veux appeler une IP distante, remplace 127.0.0.1 par ip_or_host
    char url[256];
    snprintf(url, sizeof url, "http://%s/local/objectanalytics/control.cgi", ip_or_host ? ip_or_host : "127.0.0.1");

    char *json = NULL;
    long http_code = 0;

    if (!aoa_get_configuration_json(url, user, pass, &json, &http_code))
    {
        syslog(LOG_WARNING, "AOA getConfiguration failed (HTTP %ld) url=%s", http_code, url);
        return;
    }

    aoa_log_scenarios_table_from_json(json);
    aoa_log_scenarios_table_jsmn(json);
    aoa_log_raw_response(NULL, json);
    free(json);
}
