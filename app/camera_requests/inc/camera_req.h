#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Toujours commenter : direction “loi sacrée” client
// IN  => leftToRight (LTR)
// OUT => rightToLeft (RTL)
typedef enum
{
    AOA_DIR_LTR = 0,
    AOA_DIR_RTL = 1
} aoa_dir_t;

// Toujours commenter : scénario valide = type crosslinecounting + nom conforme
typedef struct
{
    int scenario_id;          // Axis scenario.id
    int zone_id;              // IdZone extrait du nom (atoi)
    aoa_dir_t dir;            // dérivé uniquement de IN/OUT
    char scenario_name[128];  // name complet (debug)
    char zone_name[64];       // avant le 1er '_'
} aoa_scenario_t;

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
 * - out_dir : reçoit la direction déduite (IN=>LTR, OUT=>RTL).
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
