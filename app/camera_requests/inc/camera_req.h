#pragma once

#include <stddef.h>
#include <glib.h>
#include <stdio.h>
#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Toujours commenter : direction “loi sacrée” client
// OUT => leftToRight (LTR)
// IN => rightToLeft (RTL)
typedef enum
{
    AOA_DIR_LTR = 0,
    AOA_DIR_RTL = 1
} aoa_dir_t;
// Toujours commenter : petit buffer mémoire pour récupérer la réponse HTTP via libcurl.
typedef struct
{
    char *ptr;      // buffer alloué (malloc/realloc)
    size_t len;     // taille utilisée (hors '\0')
} camera_mem_buf_t;

// Toujours commenter : petit buffer mémoire pour récupérer la réponse HTTP via libcurl.
typedef struct
{
    char *memory;
    size_t size;
} aoa_mem_buf_t;

// Toujours commenter : scénario valide = type crosslinecounting + nom conforme
typedef struct
{
    int scenario_id;          // Axis scenario.id
    int zone_id;              // IdZone extrait du nom (atoi)
    aoa_dir_t dir;            // dérivé uniquement de IN/OUT
    char scenario_name[128];  // name complet (debug)
    char zone_name[64];       // avant le 1er '_'
} aoa_scenario_t;
// =========================================================================
// Phase 2.2/2.3 : Agrégation par zone_id + JSON multi-ROI
// =========================================================================
typedef struct
{
  int car, bike, truck, bus, human, other, total;
} ClassTotals;

typedef struct
{
  char from_iso[32], to_iso[32]; // bords minute (local)
  gboolean has_dir1, has_dir2;   // flags d'arrivée partielle
  ClassTotals dir1, dir2;        // deltas/minute
} MinuteEntry2;

typedef struct
{
  GDateTime *block_start_local; /* début fenêtre 15 min (local) */
  MinuteEntry2 m[15];           /* 15 buckets */
  int filled;                   /* combien de minutes remplies */
} Block15x2;
// Toujours commenter : une entrée minute pour UNE zone (dvg + gvd + from/to).
typedef struct
{
  char from_iso[32];
  char to_iso[32];
  gboolean has_dvg;   // IN (RTL)
  gboolean has_gvd;   // OUT (LTR)
  ClassTotals dvg;    // RTL / IN
  ClassTotals gvd;    // LTR / OUT
} ZoneMinuteEntry;

// Toujours commenter : bloc 15 minutes pour UNE zone_id.
typedef struct
{
  int zone_id;
  GDateTime *block_start_local;  // début tranche 15 min (local)
  ZoneMinuteEntry m[15];
} ZoneBlock;


/**
 * Paramètres :
 * - ip_or_host : IP/hostname de la caméra (ex "127.0.0.1" ou IP distante).
 * - user/pass  : identifiants Digest pour l’API Axis.
 * - out_json   : reçoit un buffer alloué (malloc) contenant la réponse JSON (à free()).
 * - out_http_code : reçoit le code HTTP (optionnel, peut être NULL).
 */
int camera_get_configuration_json(const char *ip_or_host,
                                  const char *user,
                                  const char *pass,
                                  char **out_json,
                                  long *out_http_code);

/**
 * Paramètres :
 * - name : le champ scenario.name à parser au format "NomScenario_IdZone_IN/OUT".
 * - out_zone_name/zone_name_cap : buffer de sortie pour NomScenario (avant le 1er '_').
 * - out_zone_id : reçoit IdZone (premier nombre après le 1er '_', via atoi()).
 * - out_dir : reçoit la direction déduite (OUT=>LTR, IN=>RTL).
 */
int aoa_parse_scenario_name(const char *name,
                            char *out_zone_name, size_t zone_name_cap,
                            int *out_zone_id,
                            aoa_dir_t *out_dir);

/**
 * Paramètres :
 * - json : réponse brute (string) de control.cgi/getConfiguration.
 * - out/out_cap : tableau de sortie + capacité, rempli avec les scénarios valides.
 * Retour : nombre d’éléments écrits dans out[].
 */
int aoa_list_valid_scenarios_from_controlcgi_json(const char *json,
                                                  aoa_scenario_t *out,
                                                  int out_cap);

/**
 * Phase 1 (orchestration) : rafraîchit la liste des scénarios valides en combinant :
 *  - camera_get_configuration_json() (HTTP Digest)
 *  - aoa_list_valid_scenarios_from_controlcgi_json() (parsing + règles métier)
 *
 * Paramètres :
 * - ip_or_host : IP/hostname de la caméra (ex "127.0.0.1").
 * - user/pass  : identifiants Digest.
 * - out_arr/out_cap : buffer de sortie.
 * - out_count : reçoit le nombre d'éléments écrits (optionnel, peut être NULL).
 * - out_http_code : reçoit le code HTTP (optionnel, peut être NULL).
 *
 * Retour :
 * - 1 si succès (HTTP 2xx + parsing OK)
 * - 0 sinon
 */
int aoa_refresh_valid_scenarios(const char *ip_or_host,
                                const char *user,
                                const char *pass,
                                aoa_scenario_t *out_arr,
                                int out_cap,
                                int *out_count,
                                long *out_http_code);

/**
 * Phase 1 : log en syslog un tableau lisible des scénarios valides.
 *
 * Paramètres :
 * - arr : tableau de scénarios.
 * - n   : nombre d'éléments.
 * - tag : préfixe dans les logs (optionnel, peut être NULL).
 */
void aoa_log_valid_scenarios_table(const aoa_scenario_t *arr, int n, const char *tag);

/**
 * Paramètres :
 * - name : le champ scenario.name à parser au format "NomScenario_IdZone_IN/OUT".
 * - out_zone_name/zone_name_cap : buffer de sortie pour NomScenario (avant le 1er '_').
 * - out_zone_id : reçoit IdZone (premier nombre après le 1er '_', via atoi()).
 * - out_dir : reçoit la direction déduite (OUT=>LTR, IN=>RTL).
 */

int aoa_parse_scenario_name(const char *name,
                            char *out_zone_name, size_t zone_name_cap,
                            int *out_zone_id,
                            aoa_dir_t *out_dir);

/**
 * Paramètres :
 * - dir : direction interne (AOA_DIR_LTR ou AOA_DIR_RTL).
 * Retour : chaîne "leftToRight" ou "rightToLeft" (utile pour logs/payload).
 */
const char *aoa_dir_to_string(aoa_dir_t dir);

/**
 * Paramètres :
 * - dir : direction interne (AOA_DIR_LTR ou AOA_DIR_RTL).
 * Retour : chaîne "IN" ou "OUT" (utile pour logs).
 */
const char *aoa_dir_to_inout(aoa_dir_t dir);

#ifdef __cplusplus
}
#endif
