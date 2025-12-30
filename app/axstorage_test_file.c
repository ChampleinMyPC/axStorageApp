#include <axsdk/axstorage.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <curl/curl.h>
#include "myLib.h"
#include "./tests/tests.h"
#include "./include/env.h"
#include "./camera_requests/inc/camera_req.h"



// #define HTTP_TARGET "http://192.168.1.45:3000/api/enregistrements/ingest-batch-from-acap"
static aoa_scenario_t g_valid[20]; // mais on a que 10 scénarios max sur la caméra
static int g_valid_count = 0;

// Toujours commenter : callback GLib => refresh + logs.
static gboolean refresh_valid_scenarios_cb(gpointer user_data)
{
  (void)user_data;

  long http = 0;
  int n = 0;

  // Toujours commenter : on s'appuie sur les macros USER/PASS de env.h (secrets non commit).
  if (!aoa_refresh_valid_scenarios("127.0.0.1", USER, PASS, g_valid, 20, &n, &http))
  {
    syslog(LOG_WARNING, "Phase1: refresh_valid_scenarios FAILED (http=%ld)", http);
    return TRUE; // Toujours commenter : on garde le timer actif
  }

  g_valid_count = n;
  aoa_log_valid_scenarios_table(g_valid, g_valid_count, "Phase1");

  return TRUE; // Toujours commenter : TRUE => timer récurrent
}
// #define HTTP_TARGET "https://api.mycarcounter.fr/api/enregistrements/ingest-batch-from-acap"

// Auth0 token configuration
// #define AUTH0_URL "https://dev-6xrnn215zh26wxwo.us.auth0.com/oauth/token"
// #define AUTH0_CLIENT_ID "vtJWEop6rfpRz59PoFmfni27Fe7iwI4Z"
// #define AUTH0_CLIENT_SECRET "W04lUw4exH82IV4CWPd3i7ObsYAIAlWYINicuJHTlHZuRseoLZvhUl8f0YIQwcCn"
// #define AUTH0_AUDIENCE "https://api.mycarcounter.fr"
// #define AUTH0_GRANT_TYPE "client_credentials"

// #define USER "root"
// #define PASS "#Ch@mpIe1nMyCC"
// #define NUM_SCENARIO_DIR1_OU_DVG "1" // <- à adapter selon ton scénario de comptage SUR LA CAMERA
// #define NUM_SCENARIO_DIR2_OU_GVD "2" // <- à adapter selon ton scénario de comptage SUR LA CAMERA

/* NEW: numéro de série global, utilisé aux flush 15' et on SIGTERM */
static char g_camera_serial[64] = "camera_name"; // par défaut
static void verify_latest_json_on_all_disks_for_serial(const char *camera_serial);
static void write_block_json_to_sd(const char *serial);
static void day_string_local(GDateTime **now_local_out, char out_day[11])
{
  GTimeZone *tz = g_time_zone_new_local();
  GDateTime *now_local = g_date_time_new_now(tz);
  gchar *day = g_date_time_format(now_local, "%Y-%m-%d"); // ex: 2025-09-24
  g_strlcpy(out_day, day, 11);
  g_free(day);
  g_time_zone_unref(tz);
  *now_local_out = now_local; // le caller fera unref
}

/*** ---- main : init storage, s'abonner, timer pour poll ---- ***/
static gboolean get_camera_serial(char out[64]);
static void try_ship_file_to_node(const char *filepath, const char *node_url);
static gboolean http_post_json(const char *url, const char *json, long *http_code_out);
static char *strstr_between(const char *hay, const char *start, const char *end);
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);

// static void  curl_test_get_jsonplaceholder(void);
// static void list_all_files_with_parent(const char *root_dir);
static void resend_pending_today(const char *base_dir, const char *serial, const char *url);
// static void verify_latest_json_on_all_disks_for(const char *scenario_str); // debug
/*** ---- CURL buffer (reprend ton hello.c) ---- ***/
struct MemoryStruct
{
  char *memory;
  size_t size;
};
// petit buffer mémoire pour récupérer les réponses HTTP
struct mem
{
  char *p;
  size_t n;
};
// Auth0 token globals
static char g_access_token[2046] = {0};
static time_t g_token_expires_at = 0;

// Toujours commenter : table globale zone_id -> ZoneBlock*
static GHashTable *g_zone_blocks = NULL;

// Toujours commenter : début tranche 15 min "référence" (commune à toutes zones)
static GDateTime *g_zone_block_start_local = NULL;

// Function to fetch Auth0 access token
static int fetch_auth0_token(char err[256])
{
  CURL *curl = curl_easy_init();
  if (!curl)
  {
    snprintf(err, 256, "curl init fail");
    return 0;
  }

  char post_data[1024];
  snprintf(post_data, sizeof(post_data),
           "grant_type=%s&client_id=%s&client_secret=%s&audience=%s",
           AUTH0_GRANT_TYPE, AUTH0_CLIENT_ID, AUTH0_CLIENT_SECRET, AUTH0_AUDIENCE);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

  struct mem m = {0};

  curl_easy_setopt(curl, CURLOPT_URL, AUTH0_URL);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || http_code != 200)
  {
    snprintf(err, 256, "Auth0 request failed: %s HTTP %ld", curl_easy_strerror(rc), http_code);
    free(m.p);
    return 0;
  }

  // Parse JSON response
  char *access_token = strstr_between(m.p, "\"access_token\":\"", "\"");
  char *expires_in_str = strstr_between(m.p, "\"expires_in\":", ",");

  if (!access_token || !expires_in_str)
  {
    snprintf(err, 256, "Invalid Auth0 response");
    free(access_token);
    free(expires_in_str);
    free(m.p);
    return 0;
  }

  int expires_in = atoi(expires_in_str);
  time_t now = time(NULL);
  g_token_expires_at = now + expires_in - 60; // refresh 1 min early

  strncpy(g_access_token, access_token, sizeof(g_access_token) - 1);
  g_access_token[sizeof(g_access_token) - 1] = '\0';

  free(access_token);
  free(expires_in_str);
  free(m.p);
  return 1;
}

// Function to get valid access token
static const char *get_access_token(char err[256])
{
  time_t now = time(NULL);
  if (g_access_token[0] == '\0' || now >= g_token_expires_at)
  {
    if (!fetch_auth0_token(err))
    {
      syslog(LOG_WARNING, "token on get func  %s (HTTP )", g_access_token);
      return NULL;
    }
  }
  return g_access_token;
}
// always comment code
// Parsing léger: on cherche un asset .eap et on récupère son browser_download_url
static char *strstr_between(const char *hay, const char *start, const char *end)
{
  const char *s = strstr(hay, start);
  if (!s)
    return NULL;
  s += strlen(start);
  const char *e = strstr(s, end);
  if (!e)
    return NULL;
  size_t n = (size_t)(e - s);
  char *out = (char *)malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  struct mem *m = (struct mem *)userdata;
  size_t add = size * nmemb;
  char *np = (char *)realloc(m->p, m->n + add + 1);
  if (!np)
    return 0;
  m->p = np;
  memcpy(m->p + m->n, ptr, add);
  m->n += add;
  m->p[m->n] = '\0';
  return add;
}

// static char g_camera_serial[64] = "axis-b8a44f46ce99"; // par défaut
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if (!ptr)
  {
    syslog(LOG_ERR, "Out of memory");
    return 0;
  }
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
  return realsize;
}
/* ===== Aggregation 15 minutes ===== */

// cumulés précédents indépendants par direction
// static ClassTotals g_prev_dir1 = {0};
// static ClassTotals g_prev_dir2 = {0};
// static char g_prev_reset_time_dir1[40] = {0};
// static char g_prev_reset_time_dir2[40] = {0};
// static char g_prev_reset_time[40] = {0};

typedef enum
{
  DIR1 = 0,
  DIR2 = 1
} DirKind;

// typedef struct {
//   char cam_url[256], user[64], pass[64], scenario[32], api_version[8];
//   DirKind dir;            // <- quel sens on remplit
//   gint period_sec;
// } PollCfg;

static ClassTotals compute_delta(const ClassTotals *now, const ClassTotals *prev)
{
  ClassTotals d = {
      .car = now->car >= prev->car ? now->car - prev->car : 0,
      .bike = now->bike >= prev->bike ? now->bike - prev->bike : 0,
      .truck = now->truck >= prev->truck ? now->truck - prev->truck : 0,
      .bus = now->bus >= prev->bus ? now->bus - prev->bus : 0,
      .human = now->human >= prev->human ? now->human - prev->human : 0,
      .other = now->other >= prev->other ? now->other - prev->other : 0,
      .total = now->total >= prev->total ? now->total - prev->total : 0,
  };
  return d;
}


/* --- parse cumul depuis la réponse (sscanf naïf, suffisant pour ce JSON) --- */
static void parse_cumul_totals(const char *json, ClassTotals *out, char reset_time[40])
{
  memset(out, 0, sizeof(*out));
  if (!json)
    return;
  /* resetTime */
  const char *rt = strstr(json, "\"resetTime\"");
  if (rt)
    sscanf(rt, "\"resetTime\"%*[^\"]\"%39[^\"]", reset_time);

  const char *p;
  if ((p = strstr(json, "\"totalCar\"")))
    sscanf(p, "\"totalCar\"%*[^0-9]%d", &out->car);
  if ((p = strstr(json, "\"totalBike\"")))
    sscanf(p, "\"totalBike\"%*[^0-9]%d", &out->bike);
  if ((p = strstr(json, "\"totalTruck\"")))
    sscanf(p, "\"totalTruck\"%*[^0-9]%d", &out->truck);
  if ((p = strstr(json, "\"totalBus\"")))
    sscanf(p, "\"totalBus\"%*[^0-9]%d", &out->bus);
  if ((p = strstr(json, "\"totalHuman\"")))
    sscanf(p, "\"totalHuman\"%*[^0-9]%d", &out->human);
  if ((p = strstr(json, "\"totalOtherVehicle\"")))
    sscanf(p, "\"totalOtherVehicle\"%*[^0-9]%d", &out->other);
  if ((p = strstr(json, "\"total\"")))
    sscanf(p, "\"total\"%*[^0-9]%d", &out->total);
}

/*** ---- AXStorage state (inspiré du sample Axis) ---- ***/
typedef struct
{
  AXStorage *storage;
  gchar *storage_id;
  gchar *storage_path;
  guint subscription_id;
  gboolean setup;
  gboolean writable;
  gboolean available;
  gboolean full;
  gboolean exiting;
} disk_item_t;

static GList *disks_list = NULL;
static GMainLoop *loop = NULL;

/*** ---- Utils ---- ***/
static disk_item_t *find_disk_item(gchar *storage_id)
{
  for (GList *n = g_list_first(disks_list); n != NULL; n = g_list_next(n))
  {
    disk_item_t *item = n->data;
    if (g_strcmp0(storage_id, item->storage_id) == 0)
      return item;
  }
  return NULL;
}

static gboolean signal_handler(gpointer user_data)
{
  (void)user_data;
  if (loop)
  {
    if (g_zone_block_start_local && get_camera_serial(g_camera_serial))
      write_block_json_to_sd(g_camera_serial);
    g_main_loop_quit(loop);
  }
  syslog(LOG_INFO, "Stopping (signal).");
  return G_SOURCE_REMOVE;
}

/*** ---- AXStorage: setup/release (mêmes patterns que le sample) ---- ***/
static void release_disk_cb(gpointer user_data, GError *error)
{
  syslog(LOG_INFO, "Release of %s", (gchar *)user_data);
  if (error)
  {
    syslog(LOG_WARNING, "Release error: %s", error->message);
    g_error_free(error);
  }
}

static void setup_disk_cb(AXStorage *storage, gpointer user_data, GError *error)
{
  (void)user_data;
  if (!storage || error)
  {
    syslog(LOG_ERR, "Failed setup disk: %s", error ? error->message : "null");
    if (error)
      g_error_free(error);
    return;
  }
  GError *axerr = NULL;
  gchar *storage_id = ax_storage_get_storage_id(storage, &axerr);
  if (axerr)
  {
    syslog(LOG_WARNING, "get_storage_id: %s", axerr->message);
    g_error_free(axerr);
    return;
  }
  gchar *path = ax_storage_get_path(storage, &axerr);
  if (axerr)
  {
    syslog(LOG_WARNING, "get_path: %s", axerr->message);
    g_error_free(axerr);
    g_free(storage_id);
    return;
  }

  disk_item_t *d = find_disk_item(storage_id);
  if (d)
  {
    d->storage = storage;
    d->storage_path = g_strdup(path);
    d->setup = TRUE;
    syslog(LOG_INFO, "Disk %s setup at %s", storage_id, path);
  }
  g_free(storage_id);
  g_free(path);
}

/*** ---- AXStorage: subscribe events → garde writable/available/full/exiting ---- ***/
static void subscribe_cb(gchar *storage_id, gpointer user_data, GError *error)
{
  (void)user_data;
  if (error)
  {
    syslog(LOG_WARNING, "Subscribe %s error: %s", storage_id, error->message);
    g_error_free(error);
    return;
  }
  GError *axerr = NULL;
  disk_item_t *d = find_disk_item(storage_id);
  if (!d)
    return;

  gboolean exiting = ax_storage_get_status(storage_id, AX_STORAGE_EXITING_EVENT, &axerr);
  if (axerr)
  {
    syslog(LOG_WARNING, "EXITING err: %s", axerr->message);
    g_clear_error(&axerr);
    return;
  }
  gboolean available = ax_storage_get_status(storage_id, AX_STORAGE_AVAILABLE_EVENT, &axerr);
  if (axerr)
  {
    syslog(LOG_WARNING, "AVAILABLE err: %s", axerr->message);
    g_clear_error(&axerr);
    return;
  }
  gboolean writable = ax_storage_get_status(storage_id, AX_STORAGE_WRITABLE_EVENT, &axerr);
  if (axerr)
  {
    syslog(LOG_WARNING, "WRITABLE err: %s", axerr->message);
    g_clear_error(&axerr);
    return;
  }
  gboolean full = ax_storage_get_status(storage_id, AX_STORAGE_FULL_EVENT, &axerr);
  if (axerr)
  {
    syslog(LOG_WARNING, "FULL err: %s", axerr->message);
    g_clear_error(&axerr);
    return;
  }

  d->exiting = exiting;
  d->available = available;
  d->writable = writable;
  d->full = full;
  syslog(LOG_INFO, "Disk %s: %swritable %savailable %sfull %sexiting",
         storage_id, writable ? "" : "not ", available ? "" : "not ", full ? "" : "not ", exiting ? "" : "not ");

  if (exiting && d->setup)
  {
    ax_storage_release_async(d->storage, release_disk_cb, storage_id, &axerr);
    if (axerr)
    {
      syslog(LOG_WARNING, "release err: %s", axerr->message);
      g_clear_error(&axerr);
    }
    d->setup = FALSE;
  }
  else if (writable && !full && !exiting && !d->setup)
  {
    ax_storage_setup_async(storage_id, setup_disk_cb, NULL, &axerr);
    if (axerr)
    {
      syslog(LOG_WARNING, "setup err: %s", axerr->message);
      g_clear_error(&axerr);
    }
    char serial[64] = {0};
    if (!get_camera_serial(serial))
      strcpy(serial, "unknown_camera");
    syslog(LOG_INFO, "Resending pending files for serial %s", serial);
    resend_pending_today(d->storage_path, serial, HTTP_TARGET);

    // g_timeout_add_seconds(5, (GSourceFunc)curl_test_get_jsonplaceholder, NULL);
    // après setup AXStorage, par ex.:
    // const char *base = "/var/spool/storage/areas/SD_DISK/axstorage/aoa_counts";
    // list_all_files_with_parent(base);
  }
}
// Toujours commenter : crée la table si besoin + retourne le bloc de la zone.
static ZoneBlock *zone_bucket_get_or_create(int zone_id)
{
  // Paramètre :
  // - zone_id : ROI id extrait du nom de scénario (>= 1)
  if (zone_id <= 0)
    return NULL;

  if (!g_zone_blocks)
  {
    // key=int direct, value=ZoneBlock* (free auto)
    g_zone_blocks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  }

  ZoneBlock *zb = (ZoneBlock *)g_hash_table_lookup(g_zone_blocks, GINT_TO_POINTER(zone_id));
  if (!zb)
  {
    zb = (ZoneBlock *)g_malloc0(sizeof(*zb));
    zb->zone_id = zone_id;
    zb->block_start_local = NULL;
    g_hash_table_insert(g_zone_blocks, GINT_TO_POINTER(zone_id), zb);
  }

  return zb;
}
// Toujours commenter : calcule/assure la tranche 15' + met le delta dans la minute courante.
static void zone_bucket_add_delta(int zone_id, aoa_dir_t dir, const ClassTotals *delta, GDateTime *now_local)
{
  // Paramètres :
  // - zone_id : ROI dynamique
  // - dir : AOA_DIR_RTL (IN => dvg) ou AOA_DIR_LTR (OUT => gvd)
  // - delta : deltas minute calculés pour un scenario_id
  // - now_local : timestamp local (minute courante)
  if (!delta || !now_local)
    return;

  ZoneBlock *zb = zone_bucket_get_or_create(zone_id);
  if (!zb)
    return;

  // Toujours commenter : déterminer le "start 15 minutes" de la tranche en cours.
  GDateTime *start = g_date_time_new_local(
      g_date_time_get_year(now_local),
      g_date_time_get_month(now_local),
      g_date_time_get_day_of_month(now_local),
      g_date_time_get_hour(now_local),
      (g_date_time_get_minute(now_local) / 15) * 15,
      0);

  // Toujours commenter : gérer un changement de tranche global (commune).
  if (!g_zone_block_start_local ||
      g_date_time_difference(start, g_zone_block_start_local) != 0)
  {
    // On remplace la référence globale.
    if (g_zone_block_start_local)
      g_date_time_unref(g_zone_block_start_local);
    g_zone_block_start_local = start; // ownership
  }
  else
  {
    g_date_time_unref(start);
  }

  // Toujours commenter : s'assurer que la zone est sur la même tranche.
  if (!zb->block_start_local ||
      g_date_time_difference(zb->block_start_local, g_zone_block_start_local) != 0)
  {
    if (zb->block_start_local)
      g_date_time_unref(zb->block_start_local);
    memset(zb->m, 0, sizeof(zb->m));
    zb->block_start_local = g_date_time_ref(g_zone_block_start_local);
  }

  // Toujours commenter : index minute dans la tranche (0..14)
  gint64 secs = g_date_time_difference(now_local, zb->block_start_local) / G_TIME_SPAN_SECOND;
  if (secs < 0)
    secs = 0;
  int idx = (int)(secs / 60);
  if (idx < 0)
    idx = 0;
  if (idx > 14)
    idx = 14;

  // Toujours commenter : init from/to de la minute si pas encore fait.
  if (zb->m[idx].from_iso[0] == 0)
  {
    GDateTime *from = g_date_time_new_local(
        g_date_time_get_year(now_local),
        g_date_time_get_month(now_local),
        g_date_time_get_day_of_month(now_local),
        g_date_time_get_hour(now_local),
        g_date_time_get_minute(now_local),
        0);
    GDateTime *to = g_date_time_add_seconds(from, 60);

    gchar *f = g_date_time_format(from, "%Y-%m-%dT%H:%M:%S");
    gchar *t = g_date_time_format(to, "%Y-%m-%dT%H:%M:%S");
    g_strlcpy(zb->m[idx].from_iso, f, sizeof zb->m[idx].from_iso);
    g_strlcpy(zb->m[idx].to_iso, t, sizeof zb->m[idx].to_iso);
    g_free(f);
    g_free(t);
    g_date_time_unref(from);
    g_date_time_unref(to);
  }

  // Toujours commenter : loi sacrée appliquée via dir métier.
  if (dir == AOA_DIR_RTL)
  {
    // IN => dvg
    zb->m[idx].dvg.car += delta->car;
    zb->m[idx].dvg.bike += delta->bike;
    zb->m[idx].dvg.truck += delta->truck;
    zb->m[idx].dvg.bus += delta->bus;
    zb->m[idx].dvg.human += delta->human;
    zb->m[idx].dvg.other += delta->other;
    zb->m[idx].dvg.total += delta->total;
    zb->m[idx].has_dvg = TRUE;
  }
  else
  {
    // OUT => gvd
    zb->m[idx].gvd.car += delta->car;
    zb->m[idx].gvd.bike += delta->bike;
    zb->m[idx].gvd.truck += delta->truck;
    zb->m[idx].gvd.bus += delta->bus;
    zb->m[idx].gvd.human += delta->human;
    zb->m[idx].gvd.other += delta->other;
    zb->m[idx].gvd.total += delta->total;
    zb->m[idx].has_gvd = TRUE;
  }
}

// Toujours commenter : construit le JSON backend multi-ROI à partir des buckets par zone.
// Retour : string malloc() à free() par l'appelant.
static char *build_backend_json_from_zone_buckets(const char *serial)
{
  // Paramètre :
  // - serial : numéro de série caméra (peut être NULL)
  if (!g_zone_blocks || !g_zone_block_start_local)
    return NULL;

  GString *js = g_string_new(NULL);

  g_string_append_printf(js,
                         "{"
                         "\"cameraSerial\":\"%s\","
                         "\"sensor-time\":{\"timezone\":\"Europe/Paris\"},"
                         "\"content\":{\"element\":[",
                         serial ? serial : "unknown");

  gboolean first_elem = TRUE;

  // Toujours commenter : itérer sur toutes les zones.
  GHashTableIter it;
  gpointer k = NULL, v = NULL;
  g_hash_table_iter_init(&it, g_zone_blocks);

  while (g_hash_table_iter_next(&it, &k, &v))
  {
    ZoneBlock *zb = (ZoneBlock *)v;
    if (!zb || !zb->block_start_local)
      continue;

    // Toujours commenter : vérifier qu'il y a au moins une minute remplie.
    gboolean any_min = FALSE;
    for (int i = 0; i < 15; i++)
    {
      if (zb->m[i].from_iso[0] != 0 && (zb->m[i].has_dvg || zb->m[i].has_gvd))
      {
        any_min = TRUE;
        break;
      }
    }
    if (!any_min)
      continue;

    if (!first_elem)
      g_string_append(js, ",");
    first_elem = FALSE;

    // Toujours commenter : un element = une zone_id.
    g_string_append_printf(js,
                           "{"
                           "\"roi-id\":%d,"
                           "\"data-type\":\"LINE\","
                           "\"resolution\":\"ONE_MINUTE\","
                           "\"measurement\":[",
                           zb->zone_id);

    gboolean first_meas = TRUE;
    for (int i = 0; i < 15; i++)
    {
      if (zb->m[i].from_iso[0] == 0)
        continue;
      if (!(zb->m[i].has_dvg || zb->m[i].has_gvd))
        continue;

      if (!first_meas)
        g_string_append(js, ",");
      first_meas = FALSE;

      // Toujours commenter : total = dvg + gvd.
      ClassTotals tot = {
          .car = zb->m[i].dvg.car + zb->m[i].gvd.car,
          .bike = zb->m[i].dvg.bike + zb->m[i].gvd.bike,
          .truck = zb->m[i].dvg.truck + zb->m[i].gvd.truck,
          .bus = zb->m[i].dvg.bus + zb->m[i].gvd.bus,
          .human = zb->m[i].dvg.human + zb->m[i].gvd.human,
          .other = zb->m[i].dvg.other + zb->m[i].gvd.other,
          .total = zb->m[i].dvg.total + zb->m[i].gvd.total,
      };

      g_string_append_printf(js,
                             "{"
                             "\"from\":\"%s\",\"to\":\"%s\","
                             "\"counts\":{"
                             "\"dvg\":["
                             "{\"label\":\"car\",\"value\":%d},"
                             "{\"label\":\"bike\",\"value\":%d},"
                             "{\"label\":\"truck\",\"value\":%d},"
                             "{\"label\":\"bus\",\"value\":%d},"
                             "{\"label\":\"human\",\"value\":%d},"
                             "{\"label\":\"other\",\"value\":%d}"
                             "],"
                             "\"gvd\":["
                             "{\"label\":\"car\",\"value\":%d},"
                             "{\"label\":\"bike\",\"value\":%d},"
                             "{\"label\":\"truck\",\"value\":%d},"
                             "{\"label\":\"bus\",\"value\":%d},"
                             "{\"label\":\"human\",\"value\":%d},"
                             "{\"label\":\"other\",\"value\":%d}"
                             "],"
                             "\"total\":["
                             "{\"label\":\"car\",\"value\":%d},"
                             "{\"label\":\"bike\",\"value\":%d},"
                             "{\"label\":\"truck\",\"value\":%d},"
                             "{\"label\":\"bus\",\"value\":%d},"
                             "{\"label\":\"human\",\"value\":%d},"
                             "{\"label\":\"other\",\"value\":%d}"
                             "]"
                             "}"
                             "}",
                             zb->m[i].from_iso, zb->m[i].to_iso,
                             // dvg
                             zb->m[i].dvg.car, zb->m[i].dvg.bike, zb->m[i].dvg.truck,
                             zb->m[i].dvg.bus, zb->m[i].dvg.human, zb->m[i].dvg.other,
                             // gvd
                             zb->m[i].gvd.car, zb->m[i].gvd.bike, zb->m[i].gvd.truck,
                             zb->m[i].gvd.bus, zb->m[i].gvd.human, zb->m[i].gvd.other,
                             // total
                             tot.car, tot.bike, tot.truck, tot.bus, tot.human, tot.other);
    }

    g_string_append(js, "]}"); // end measurement + element
  }

  g_string_append(js, "]}}"); // end element + content + root

  // Toujours commenter : détacher la string malloc() depuis GString.
  return g_string_free(js, FALSE);
}


static void write_json_on_all_disks_serial(const char *basename,
                                           const char *json,
                                           const char *camera_serial)
{
  for (GList *n = g_list_first(disks_list); n; n = g_list_next(n))
  {
    disk_item_t *d = n->data;
    if (!d->available || !d->writable || d->full || !d->setup || !d->storage_path)
      continue;
    // gchar* outdir = g_strdup_printf("%s/aoa_counts/%s", d->storage_path, camera_serial);
    // g_mkdir_with_parents(outdir, 0775);
    // GDateTime* now = g_date_time_new_now_utc();
    // gchar* ts = g_date_time_format(now, "%Y%m%dT%H%M%SZ");
    // gchar* path = g_strdup_printf("%s/%s_%s.json", outdir, basename, ts);
    syslog(LOG_INFO, "write_json: storage_path=%s", d->storage_path ? d->storage_path : "(null)");
    // 1) dossier par jour (local)
    char ymd[11];
    GDateTime *now_local = NULL;
    day_string_local(&now_local, ymd); // "YYYY-MM-DD"
    gchar *outdir = g_strdup_printf("%s/aoa_counts/%s/%s",
                                    d->storage_path, camera_serial, ymd);
    g_mkdir_with_parents(outdir, 0775);
    syslog(LOG_INFO, "write_json: outdir=%s", outdir);
    // 2) nom horodaté (local pour l’unicité)
    gchar *ts = g_date_time_format(now_local, "%Y%m%dT%H%M%S");
    gchar *path = g_strdup_printf("%s/%s_%s.json", outdir, basename, ts);
    FILE *f = g_fopen(path, "w");
    if (f)
    {
      g_fprintf(f, "%s\n", json);
      fclose(f);
      syslog(LOG_INFO, "Wrote %s", path);
    }
    else
    {
      syslog(LOG_WARNING, "open %s failed", path);
      syslog(LOG_WARNING, "fopen(%s) failed: %s", path, g_strerror(errno));
    }

    g_free(path);
    g_free(ts);
    g_free(outdir);
    g_date_time_unref(now_local);
  }
}

static gboolean get_camera_serial(char out[64])
{
  // VAPIX: /axis-cgi/param.cgi?action=list&group=Properties.System.SerialNumber
  CURL *c = curl_easy_init();
  if (!c)
    return FALSE;
  struct MemoryStruct buf = {0};
  char url[256];
  snprintf(url, sizeof url, "http://127.0.0.1/axis-cgi/param.cgi?action=list&group=Properties.System.SerialNumber");
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERNAME, USER);
  curl_easy_setopt(c, CURLOPT_PASSWORD, PASS);
  curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK || !buf.memory)
  {
    free(buf.memory);
    curl_easy_cleanup(c);
    return FALSE;
  }
  // réponse contient: Properties.System.SerialNumber=ACCC8E123456
  char *p = strstr(buf.memory, "Properties.System.SerialNumber=");
  gboolean ok = FALSE;
  if (p)
  {
    p += strlen("Properties.System.SerialNumber=");
    // copie jusqu'à fin de ligne
    size_t i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i < 63)
    {
      out[i] = p[i];
      i++;
    }
    out[i] = '\0';
    ok = (i > 0);
    // on affiche le nom de la camera dans les log
    syslog(LOG_INFO, "\n\nle nom de la camera dans get serial est %s\n\n", out);
  }
  free(buf.memory);
  curl_easy_cleanup(c);
  return ok;
}

static void write_block_json_to_sd(const char *serial)
{
char *json = build_backend_json_from_zone_buckets(serial);
if (!json)
  return;

// nom de fichier basé sur début de tranche
gchar *start_str = g_date_time_format(g_zone_block_start_local, "%Y%m%dT%H%M");
gchar *fname = g_strdup_printf("counts_%s_%s_15min", serial ? serial : "unknown", start_str);

// réutilise ta fonction d’écriture (dossier unique aoa_counts/)
syslog(LOG_INFO, "ON ECRIT LE BLOCK DE 15 SUR EL DISK");
write_json_on_all_disks_serial(fname, json, serial); // même signature qu’actuelle sans “scenario”
syslog(LOG_INFO, "FIN ECRITURE LE BLOCK DE 15 SUR LE DISK \n");
syslog(LOG_INFO, "AVANT LAPPEL DE VERIF POUR SERIAL ");

g_free(start_str);
g_free(fname);
free(json);


  verify_latest_json_on_all_disks_for_serial(serial); // debug

  // g_string_free(js, TRUE);
}
static void maybe_flush_15min(void)
{
  // Toujours commenter : en multi-ROI, la tranche de référence est g_zone_block_start_local.
  // Si NULL => aucun delta n'a encore été déposé, donc rien à flusher.
  if (!g_zone_block_start_local)
    return;

  // si la minute courante est le début d’une nouvelle tranche 15’, on flush l’ancienne
  GDateTime *now = g_date_time_new_now_local();
  gint cur_min = g_date_time_get_minute(now);
  gint blk_min = g_date_time_get_minute(g_zone_block_start_local);

  // changement de tranche si cur_min/15 != blk_min/15 OU si heure/jour a bougé
  gboolean tranche_change =
      (cur_min / 15 != blk_min / 15) ||
      (g_date_time_get_hour(now) != g_date_time_get_hour(g_zone_block_start_local)) ||
      (g_date_time_get_day_of_month(now) != g_date_time_get_day_of_month(g_zone_block_start_local));

  syslog(LOG_INFO, "maybe_flush_15: now=%02d blk=%02d",
         g_date_time_get_minute(now),
         g_date_time_get_minute(g_zone_block_start_local));

  if (tranche_change)
  {
    syslog(LOG_INFO, "la tranche a changé on écrit le block de 15");

    char serial[64] = {0};
    if (!get_camera_serial(serial))
      strcpy(serial, "unknown");

    // Toujours commenter : écrit le JSON multi-ROI (content.element[]) depuis g_zone_blocks.
    write_block_json_to_sd(serial);

    // -----------------------------------------------------------------
    // Reset après flush (obligatoire pour éviter double compte)
    // -----------------------------------------------------------------

    if (g_zone_block_start_local)
    {
      g_date_time_unref(g_zone_block_start_local);
      g_zone_block_start_local = NULL;
    }

    if (g_zone_blocks)
    {
      // Libérer block_start_local de chaque ZoneBlock avant destroy
      GHashTableIter it;
      gpointer k = NULL, v = NULL;
      g_hash_table_iter_init(&it, g_zone_blocks);

      while (g_hash_table_iter_next(&it, &k, &v))
      {
        ZoneBlock *zb = (ZoneBlock *)v;
        if (zb && zb->block_start_local)
        {
          g_date_time_unref(zb->block_start_local);
          zb->block_start_local = NULL;
        }
      }

      g_hash_table_destroy(g_zone_blocks);
      g_zone_blocks = NULL;
    }
  }

  g_date_time_unref(now);
}


/* --- Trouve le dernier .json dans <storage_path>/aoa_counts --- */
static gchar *find_latest_json_in_dir(const gchar *dirpath)
{
  GDir *dir = g_dir_open(dirpath, 0, NULL);
  if (!dir)
    return NULL;

  gchar *latest_path = NULL;
  time_t latest_mtime = 0;

  const gchar *name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL)
  {
    if (!g_str_has_suffix(name, ".json"))
      continue;
    gchar *full = g_build_filename(dirpath, name, NULL);

    GStatBuf st;
    if (g_stat(full, &st) == 0 && S_ISREG(st.st_mode))
    {
      time_t mt = st.st_mtime;
      if (mt >= latest_mtime)
      {
        g_free(latest_path);
        latest_path = full;
        latest_mtime = mt;
        continue;
      }
    }
    g_free(full);
  }
  g_dir_close(dir);
  return latest_path; /* à g_free par l'appelant */
}

/* --- Lit un fichier et l'affiche en entier (JSON brut) --- */
static void read_and_print_file(const gchar *filepath)
{
  return;
  g_print("\n====================start_new=====================");
  // syslog(LOG_INFO, "Reading %s", filepath);
  if (!filepath)
  {
    g_printerr("No file to read.\n");
    return;
  }
  gchar *contents = NULL;
  gsize len = 0;
  if (g_file_get_contents(filepath, &contents, &len, NULL))
  {
    g_print("---- %s ----\n%.*s\n", filepath, (int)len, contents);
  }
  else
  {
    g_printerr("Failed to read: %s\n", filepath);
  }
  g_free(contents);
  g_print("\n====================end_new=====================");
}

/* --- Vérifie tous les disques connus et affiche le dernier JSON écrit --- */
/* nouvelle vérif “par serial” */
static void verify_latest_json_on_all_disks_for_serial(const char *camera_serial)
{
  // afficher un message de debug ici
  syslog(LOG_INFO, "ON ENTRE DANS LA FONCTION VERIFY");

  syslog(LOG_INFO, "Verifying latest JSON files for camera serial: %s", camera_serial);
  // g_print("\n=== Verifying latest JSON files for camera serial:  ===\n", camera_serial);
  for (GList *n = g_list_first(disks_list); n; n = g_list_next(n))
  {
    disk_item_t *d = n->data;
    if (!d || !d->storage_path)
      continue;
    // gchar* dir = g_strdup_printf("%s/aoa_counts/%s", d->storage_path, camera_serial);
    // cible le dossier du jour courant (local)
    char ymd[11];
    GDateTime *now_local = NULL;
    day_string_local(&now_local, ymd);
    gchar *dir = g_strdup_printf("%s/aoa_counts/%s/%s", d->storage_path, camera_serial, ymd);
    gchar *latest = find_latest_json_in_dir(dir);
    if (latest)
    {
      syslog(LOG_INFO, "lecture et affichage du dernier json trouve dans %s", dir);
      read_and_print_file(latest);
      syslog(LOG_INFO, "======================fin de lecture et affichage  %s\n ..............debut de lenvoi................................\n====================", dir);
      try_ship_file_to_node(latest, HTTP_TARGET);
      syslog(LOG_INFO, "======================fin de lenvoi  %s====================\n", dir);
    }
    else
    {
      syslog(LOG_INFO, "======================AUCUN FICHIER TROUVE  %s====================\n", dir);
      g_printerr("No JSON file found in %s\n", dir);
    }
    syslog(LOG_INFO, "======================AUCUN FICHIER TROUVE  %s V2====================\n", dir);
    g_free(latest);
    g_free(dir);
    g_date_time_unref(now_local);
  }
}

/*** ---- Tick: fait le POST JSON-RPC AOA et écrit sur SD ---- ***/
typedef struct
{
  char cam_url[256];
  char user[64];
  char pass[64];
  char scenario[64];   // UID ou index
  char api_version[8]; // "1.6"
  DirKind dir;         // <- quel sens on remplit
  gint period_sec;     // cadence
} PollCfg;
typedef struct
{
  ClassTotals prev;
  char reset_time[40];
  gboolean inited;
} ScenarioState;

static GHashTable *g_state_by_scenario_id = NULL;

static ScenarioState *get_or_create_state(int scenario_id)
{
    if (scenario_id <= 0)
        return NULL;

    if (!g_state_by_scenario_id)
    {
        g_state_by_scenario_id =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }

    ScenarioState *st =
        g_hash_table_lookup(g_state_by_scenario_id, GINT_TO_POINTER(scenario_id));

    if (!st)
    {
        st = g_malloc0(sizeof(*st));
        st->inited = FALSE;
        g_hash_table_insert(
            g_state_by_scenario_id,
            GINT_TO_POINTER(scenario_id),
            st);
    }

    return st;
}
// Toujours commenter : retrouve le scénario validé (Phase 1) à partir d'un scenario_id Axis.
static const aoa_scenario_t *find_valid_by_scenario_id(int scenario_id)
{
    for (int i = 0; i < g_valid_count; i++)
    {
        if (g_valid[i].scenario_id == scenario_id)
            return &g_valid[i];
    }
    return NULL;
}

static gboolean tick_poll_and_store(gpointer user_data)
{
  PollCfg *cfg = (PollCfg *)user_data;

  CURL *curl = curl_easy_init();
  if (!curl)
  {
    syslog(LOG_ERR, "curl init failed");
    return G_SOURCE_CONTINUE;
  }

  struct MemoryStruct chunk = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  // Corps JSON (scenario: nombre ou UID)
  char body[512];
  gboolean numeric = TRUE;
  for (const char *p = cfg->scenario; *p; ++p)
  {
    if (*p < '0' || *p > '9')
    {
      numeric = FALSE;
      break;
    }
  }
  if (numeric)
    snprintf(body, sizeof body,
             "{ \"apiVersion\":\"%s\",\"context\":\"acap-loop\",\"method\":\"getAccumulatedCounts\",\"params\":{\"scenario\":%s} }",
             cfg->api_version, cfg->scenario);
  else
    snprintf(body, sizeof body,
             "{ \"apiVersion\":\"%s\",\"context\":\"acap-loop\",\"method\":\"getAccumulatedCounts\",\"params\":{\"scenario\":\"%s\"} }",
             cfg->api_version, cfg->scenario);

  curl_easy_setopt(curl, CURLOPT_URL, cfg->cam_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
  curl_easy_setopt(curl, CURLOPT_USERNAME, cfg->user);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg->pass);
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK)
  {
    syslog(LOG_WARNING, "curl error: %s", curl_easy_strerror(rc));
  }
  else
  {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code == 200 && chunk.memory && chunk.size)
    {

      // 1) Parse cumul & resetTime
      char rt[40] = {0};
      ClassTotals now = {0};
      parse_cumul_totals(chunk.memory, &now, rt);
      // choisir les bons "prev"/"reset" selon la direction
      int scenario_id = atoi(cfg->scenario);
      ScenarioState *st = get_or_create_state(scenario_id);

      ClassTotals *prev = NULL;
      char *prev_rt = NULL;

      if (st)
      {
        prev = &st->prev;
        prev_rt = st->reset_time;
      }
      else
      {
        syslog(LOG_ERR, "Phase2: cannot alloc ScenarioState (scenario=%d)", scenario_id);
        // cleanup et continue
      }

      // reset si resetTime a changé
      if ((st && !st->inited) || prev_rt[0] == 0 || strcmp(rt, prev_rt) != 0)
      {
        g_strlcpy(prev_rt, rt, 40);
        *prev = now; // rebasing
        if (st) st->inited = TRUE;
        syslog(LOG_INFO,
              "Phase2: resetTime changed (scenario=%d) -> minute sent with zero counts",
              scenario_id);        
      }

      // delta et dépôt dans le bucket minute
      ClassTotals d = compute_delta(&now, prev);
      *prev = now;

      GDateTime *now_local = g_date_time_new_now_local();
      // Toujours commenter : dépôt multi-ROI (Phase 2.2) -> agrégation par zone_id.
      const aoa_scenario_t *sc = find_valid_by_scenario_id(scenario_id);
      if (sc)
      {
          zone_bucket_add_delta(sc->zone_id, sc->dir, &d, now_local);
      }
      else
      {
          syslog(LOG_WARNING, "Phase2: scenario_id=%d absent de g_valid[] (refresh Phase1 ?)", scenario_id);
      }
      g_date_time_unref(now_local);

      syslog(LOG_ERR, "TOTAL ET DELTA DU SCENARIO %d", scenario_id);
      syslog(LOG_INFO, "CUM now c=%d b=%d t=%d bus=%d h=%d o=%d tot=%d",
            now.car,now.bike,now.truck,now.bus,now.human,now.other,now.total);
      syslog(LOG_INFO, "DELTA   d c=%d b=%d t=%d bus=%d h=%d o=%d tot=%d",
            d.car,d.bike,d.truck,d.bus,d.human,d.other,d.total);
    }
    else
    {
      syslog(LOG_WARNING, "HTTP %ld (no payload)", http_code);
    }
  }

  curl_slist_free_all(headers);
  free(chunk.memory);
  curl_easy_cleanup(curl);
  return G_SOURCE_CONTINUE;
}
static PollCfg g_dir1, g_dir2;
/* --- Tick chaque minute: poll + store + flush si nouvelle tranche 15’ --- */
static gboolean minute_tick(gpointer unused)
{
  (void)unused;

  // 1) Poll : boucle sur tous les scénarios valides (Phase 1), sinon fallback legacy
  if (g_valid_count > 0)
  {
    // 2) Flush : si on vient d’entrer dans une nouvelle tranche 15’
    maybe_flush_15min();
    for (int i = 0; i < g_valid_count; i++)
    {
      PollCfg tmp = g_dir1; // reprend cam_url/user/pass/api_version
      snprintf(tmp.scenario, sizeof tmp.scenario, "%d", g_valid[i].scenario_id);
      // tmp.dir = (g_valid[i].dir == AOA_DIR_RTL) ? DIR1 : DIR2; // RTL=IN=dvg, LTR=OUT=gvd
      tick_poll_and_store(&tmp);
      // syslog(LOG_INFO, "ON EST DANS LE IF DE MINIUTE TICK" );
    }
  }
  else
  {
    // syslog(LOG_INFO, "ON EST DANS LE ELSE DE MINIUTE TICK" );
    tick_poll_and_store(&g_dir1);
    tick_poll_and_store(&g_dir2);
  }


  return G_SOURCE_CONTINUE;
}

// Envoie JSON -> Node
static gboolean http_post_json(const char *url, const char *json, long *http_code_out)
{
  CURL *curl = curl_easy_init();
  if (!curl)
    return FALSE;

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  // Add Auth0 Bearer token for authorization
  char err[256];
  const char *token = get_access_token(err);
  if (token)
  {
    char auth_header[2048 + 20];
    snprintf(auth_header, sizeof auth_header, "Authorization: Bearer %s", token);
    syslog(6, "Authorization: Bearer %s", token);
    headers = curl_slist_append(headers, auth_header);
  }
  else
  {
    syslog(LOG_WARNING, "Failed to get Auth0 token: %s", err);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  if (http_code_out)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code_out)
    *http_code_out = http_code;

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return (rc == CURLE_OK);
}

// Lit un fichier et l'envoie (puis, si succès 200/201, on peut le renommer ".sent")
static void try_ship_file_to_node(const char *filepath, const char *url)
{
  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(filepath, &contents, &len, NULL))
  {
    syslog(LOG_WARNING, "Cannot read %s to ship", filepath);
    return;
  }
  long code = 0;
  gboolean ok = http_post_json(url, contents, &code);
  if (ok && (code == 200 || code == 201))
  {
    syslog(LOG_INFO, "Shipped OK %s (HTTP %ld)", filepath, code);
    gchar *sent = g_strconcat(filepath, ".sent", NULL);
    g_rename(filepath, sent); // garde une trace, évite de supprimer brutalement
    g_free(sent);
  }
  else
  {
    syslog(LOG_WARNING, "Ship failed %s (HTTP %ld)", filepath, code);
  }
  g_free(contents);
}
static void resend_pending_today(const char *base_dir, const char *serial, const char *url)
{
  char ymd[11];
  GDateTime *now_local = NULL;
  day_string_local(&now_local, ymd);
  gchar *dir = g_strdup_printf("%s/aoa_counts/%s/%s", base_dir, serial, ymd);
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d)
  {
    g_free(dir);
    if (now_local)
      g_date_time_unref(now_local);
    return;
  }

  const gchar *name;
  while ((name = g_dir_read_name(d)) != NULL)
  {
    if (g_str_has_suffix(name, ".json") && !g_str_has_suffix(name, ".json.sent"))
    {
      gchar *fp = g_build_filename(dir, name, NULL);
      try_ship_file_to_node(fp, url);
      g_free(fp);
    }
  }
  g_dir_close(d);
  g_free(dir);
  if (now_local)
    g_date_time_unref(now_local);
}

/*** ---- main : init storage, s'abonner, timer pour poll ---- ***/
int main(void)
{
  syslog(LOG_INFO, "Start AOA→SD app");

    curl_global_init(CURL_GLOBAL_DEFAULT);

  // Phase 1 : refresh immédiat puis toutes les 10 minutes.
  refresh_valid_scenarios_cb(NULL);
  g_timeout_add_seconds(600, refresh_valid_scenarios_cb, NULL);

  aoa_log_configuration_scenarios("127.0.0.1", USER, PASS); // pour logs AOA
  test_camera_req_list_scenarios("127.0.0.1", USER, PASS); // pour logs AOA

  // test_camera_req_list_scenarios("127.0.0.1", user, pass);
  // 1) Lister les disques et s'abonner (reprend le pattern du sample Axis)
  GError *error = NULL;
  GList *disks = ax_storage_list(&error);
  if (error)
  {
    syslog(LOG_ERR, "ax_storage_list error: %s", error->message);
    g_error_free(error);
    return EXIT_FAILURE;
  }

  for (GList *n = g_list_first(disks); n != NULL; n = g_list_next(n))
  {
    gchar *storage_id = (gchar *)n->data;
    guint sub = ax_storage_subscribe(storage_id, subscribe_cb, NULL, &error);
    if (!sub || error)
    {
      syslog(LOG_WARNING, "subscribe %s failed: %s", storage_id, error ? error->message : "(null)");
      if (error)
        g_clear_error(&error);
      g_free(storage_id);
      continue;
    }
    disk_item_t *item = g_new0(disk_item_t, 1);
    item->subscription_id = sub;
    item->storage_id = g_strdup(storage_id);
    disks_list = g_list_append(disks_list, item);
    g_free(storage_id);
  }
  g_list_free(disks);

  // 2) Boucle GLib + timer
  loop = g_main_loop_new(NULL, FALSE);
  g_unix_signal_add(SIGTERM, signal_handler, NULL);
  g_unix_signal_add(SIGINT, signal_handler, NULL);
  // get_camera_serial(g_camera_serial, "champlein", "696969");
  // syslog(LOG_INFO, "Camera serial: %s", g_camera_serial);
  // Config du polling (à adapter)
  // static PollCfg cfg;
  // snprintf(cfg.cam_url, sizeof cfg.cam_url, "http://192.168.1.51/local/objectanalytics/control.cgi");
  // snprintf(cfg.user, sizeof cfg.user, "champlein");     // <-- remplace
  // snprintf(cfg.pass, sizeof cfg.pass, "696969");        // <-- remplace
  // snprintf(cfg.scenario, sizeof cfg.scenario, "2");     // ou "S1" (UID)
  // snprintf(cfg.api_version, sizeof cfg.api_version, "1.6");
  snprintf(g_dir1.cam_url, sizeof g_dir1.cam_url, "http://127.0.0.1/local/objectanalytics/control.cgi");
  strcpy(g_dir1.user, USER);
  strcpy(g_dir1.pass, PASS);
  // strcpy(g_dir1.scenario, NUM_SCENARIO_DIR1_OU_DVG);
  strcpy(g_dir1.api_version, "1.6");
  g_dir1.dir = DIR1;
  g_dir1.period_sec = 60;

  g_dir2 = g_dir1;
  // strcpy(g_dir2.scenario, NUM_SCENARIO_DIR2_OU_GVD);
  g_dir2.dir = DIR2;
  // cfg.period_sec = 60;
  // curl_test_get_jsonplaceholder();

  // char serial[64]={0};
  // if (!get_camera_serial(serial)) strcpy(serial,"unknown");
  // verify_latest_json_on_all_disks_for_serial(serial); // debug

  // Tick initial + toutes les period_sec
  // tick_poll_and_store(&cfg);
  // g_timeout_add_seconds(cfg.period_sec, tick_poll_and_store, &cfg);
  // premier tick immédiat + ensuite toutes les minutes
  minute_tick(NULL);
  g_timeout_add_seconds(60, minute_tick, NULL);

  // run
  g_main_loop_run(loop);

  // cleanup minimal (désabos / release si besoin)
  for (GList *n = g_list_first(disks_list); n != NULL; n = g_list_next(n))
  {
    disk_item_t *d = n->data;
    if (d->setup && d->storage)
    {
      ax_storage_release_async(d->storage, release_disk_cb, d->storage_id, &error);
      if (error)
      {
        syslog(LOG_WARNING, "release err: %s", error->message);
        g_clear_error(&error);
      }
      d->setup = FALSE;
    }
    if (d->subscription_id)
      ax_storage_unsubscribe(d->subscription_id, &error);
    if (error)
    {
      syslog(LOG_WARNING, "unsubscribe err: %s", error->message);
      g_clear_error(&error);
    }
    g_free(d->storage_id);
    g_free(d->storage_path);
  }
  g_list_free(disks_list);
  g_main_loop_unref(loop);

  syslog(LOG_INFO, "Finish AOA→SD app");
    // Toujours commenter : cleanup global libcurl.
  curl_global_cleanup();
  return EXIT_SUCCESS;
}
