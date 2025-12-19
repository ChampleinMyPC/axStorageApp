#include "../inc/camera_req.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <syslog.h>

// Toujours commenter : jsmn est header-only.
// -> On compile l’implémentation ici en "static" pour éviter les symboles dupliqués.
#define JSMN_STATIC
#include "../../include/jsmn.h"

// -------------------------
// Helpers jsmn (privés)
// -------------------------

// Toujours commenter : compare un token STRING à une clé C
static int jsoneq(const char *json, const jsmntok_t *tok, const char *s)
{
    if (tok->type != JSMN_STRING)
        return -1;

    int tok_len = tok->end - tok->start;
    int s_len = (int)strlen(s);

    return (tok_len == s_len && strncmp(json + tok->start, s, (size_t)tok_len) == 0) ? 0 : -1;
}

// Toujours commenter : saute récursivement un token + ses enfants et renvoie l'index du suivant
static int tok_next(const jsmntok_t *toks, int i)
{
    int j;

    switch (toks[i].type)
    {
        case JSMN_PRIMITIVE:
        case JSMN_STRING:
            return i + 1;

        case JSMN_ARRAY:
            j = i + 1;
            for (int k = 0; k < toks[i].size; k++)
                j = tok_next(toks, j);
            return j;

        case JSMN_OBJECT:
            j = i + 1;
            for (int k = 0; k < toks[i].size; k++)
            {
                j = tok_next(toks, j); // clé
                j = tok_next(toks, j); // valeur
            }
            return j;

        default:
            return i + 1;
    }
}

// Toujours commenter : récupère l'index du token valeur pour une clé dans un objet
static int obj_get(const char *json, const jsmntok_t *toks, int obj_i, const char *key)
{
    if (toks[obj_i].type != JSMN_OBJECT)
        return -1;

    int i = obj_i + 1;
    for (int p = 0; p < toks[obj_i].size; p++)
    {
        int key_i = i;
        int val_i = i + 1;

        if (jsoneq(json, &toks[key_i], key) == 0)
            return val_i;

        i = tok_next(toks, val_i);
    }
    return -1;
}

// Toujours commenter : copie token STRING vers buffer C
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

// Toujours commenter : parse int depuis token PRIMITIVE (ex: 123)
static int tok_to_int(const char *json, const jsmntok_t *tok, int *out)
{
    if (!out || tok->type != JSMN_PRIMITIVE)
        return 0;

    int len = tok->end - tok->start;
    if (len <= 0 || len > 31)
        return 0;

    char buf[32];
    memcpy(buf, json + tok->start, (size_t)len);
    buf[len] = '\0';

    *out = atoi(buf);
    return 1;
}

// -------------------------
// Helpers publics
// -------------------------
// Toujours commenter : copie sûre type strlcpy (garantit toujours le '\0')
static void aoa_strlcpy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0)
        return;

    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    size_t n = strlen(src);
    if (n >= dst_sz)
        n = dst_sz - 1;

    memcpy(dst, src, n);
    dst[n] = '\0';
}

// -------------------------
// API publique
// -------------------------

const char *aoa_dir_to_string(aoa_dir_t dir)
{
    // Toujours commenter : strings normalisées (utile pour compat côté back)
    return (dir == AOA_DIR_LTR) ? "leftToRight" : "rightToLeft";
}

const char *aoa_dir_to_inout(aoa_dir_t dir)
{
    return (dir == AOA_DIR_LTR) ? "IN" : "OUT";
}

int aoa_parse_scenario_name(const char *name,
                            char *out_zone_name, size_t zone_name_cap,
                            int *out_zone_id,
                            aoa_dir_t *out_dir)
{
    if (!name || !out_zone_name || zone_name_cap == 0 || !out_zone_id || !out_dir)
        return 0;

    out_zone_name[0] = '\0';
    *out_zone_id = -1;
    *out_dir = AOA_DIR_LTR;

    // Toujours commenter : NomScenario = avant le premier '_'
    const char *first_us = strchr(name, '_');
    if (!first_us)
        return 0;

    size_t zn = (size_t)(first_us - name);
    if (zn >= zone_name_cap)
        zn = zone_name_cap - 1;

    memcpy(out_zone_name, name, zn);
    out_zone_name[zn] = '\0';

    // Toujours commenter : suffixe IN/OUT = dernier bloc après le dernier '_'
    const char *last_us = strrchr(name, '_');
    if (!last_us || last_us[1] == '\0')
        return 0; // finit par '_' => invalide

    const char *suffix = last_us + 1;
    if (strcmp(suffix, "IN") == 0)
        *out_dir = AOA_DIR_LTR;
    else if (strcmp(suffix, "OUT") == 0)
        *out_dir = AOA_DIR_RTL;
    else
        return 0;

    // Toujours commenter : IdZone = premier nombre trouvé après le premier '_'
    // Ex: "PARIS_ROUEN_12_backup_OUT" => après 1er '_' => "ROUEN_12_backup_OUT" => premier nombre = 12
    const char *q = first_us + 1;
    while (*q && !isdigit((unsigned char)*q))
        q++;

    if (!*q)
        return 0;

    *out_zone_id = atoi(q); // règle validée : atoi fait le taf
    return 1;
}

int aoa_list_valid_scenarios_from_controlcgi_json(const char *json,
                                                  aoa_scenario_t *out,
                                                  int out_cap)
{
    if (!json || !out || out_cap <= 0)
        return 0;

    // Toujours commenter : augmenter si la réponse control.cgi grossit
    enum { MAX_TOKENS = 4096 };

    jsmn_parser p;
    jsmntok_t *toks = (jsmntok_t *)calloc(MAX_TOKENS, sizeof(jsmntok_t));
    if (!toks)
        return 0;

    jsmn_init(&p);
    int r = jsmn_parse(&p, json, strlen(json), toks, MAX_TOKENS);
    if (r < 0 || r == 0 || toks[0].type != JSMN_OBJECT)
    {
        syslog(LOG_WARNING, "AOA: jsmn_parse failed (r=%d). If NOMEM increase MAX_TOKENS", r);
        free(toks);
        return 0;
    }

    // Toujours commenter : root.data.scenarios
    int data_i = obj_get(json, toks, 0, "data");
    int scenarios_i = (data_i >= 0) ? obj_get(json, toks, data_i, "scenarios") : -1;

    if (data_i < 0 || toks[data_i].type != JSMN_OBJECT || scenarios_i < 0 || toks[scenarios_i].type != JSMN_ARRAY)
    {
        syslog(LOG_WARNING, "AOA: missing data.scenarios");
        free(toks);
        return 0;
    }

    int filled = 0;
    int idx = scenarios_i + 1;

    for (int s = 0; s < toks[scenarios_i].size; s++)
    {
        int sc_i = idx;
        idx = tok_next(toks, sc_i); // Toujours commenter : avance quoiqu’il arrive

        if (toks[sc_i].type != JSMN_OBJECT)
            continue;

        // Toujours commenter : lire scenario.id / name / type
        int scenario_id = -1;
        char scenario_name[128] = {0};
        char type[64] = {0};

        int id_i = obj_get(json, toks, sc_i, "id");
        if (id_i >= 0)
            (void)tok_to_int(json, &toks[id_i], &scenario_id);

        int name_i = obj_get(json, toks, sc_i, "name");
        if (name_i >= 0)
            tok_to_cstr(json, &toks[name_i], scenario_name, sizeof scenario_name);

        int type_i = obj_get(json, toks, sc_i, "type");
        if (type_i >= 0)
            tok_to_cstr(json, &toks[type_i], type, sizeof type);

        // Toujours commenter : règle maintenue => uniquement crosslinecounting
        if (strcmp(type, "crosslinecounting") != 0)
        {
            syslog(LOG_INFO, "AOA: ignore scenario id=%d name='%s' (type='%s')",
                   scenario_id, scenario_name, type);
            continue;
        }

        // Toujours commenter : parse name selon cahier des charges
        char zone_name[64] = {0};
        int zone_id = -1;
        aoa_dir_t dir = AOA_DIR_LTR;

        if (!aoa_parse_scenario_name(scenario_name, zone_name, sizeof zone_name, &zone_id, &dir))
        {
            syslog(LOG_WARNING, "AOA: ignore crosslinecounting id=%d name='%s' (bad naming)",
                   scenario_id, scenario_name);
            continue;
        }

        // Toujours commenter : on accepte, le sens vient uniquement de IN/OUT
        if (filled < out_cap)
        {
            out[filled].scenario_id = scenario_id;
            out[filled].zone_id = zone_id;
            out[filled].dir = dir;

            aoa_strlcpy(out[filled].scenario_name, sizeof(out[filled].scenario_name), scenario_name);
            aoa_strlcpy(out[filled].zone_name, sizeof(out[filled].zone_name), zone_name);


            filled++;
        }
        else
        {
            syslog(LOG_WARNING, "AOA: out buffer full (cap=%d), dropping scenario id=%d name='%s'",
                   out_cap, scenario_id, scenario_name);
            break;
        }
    }

    free(toks);
    return filled;
}
