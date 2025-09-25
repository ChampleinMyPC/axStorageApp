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
static void verify_latest_json_on_all_disks_for_serial(const char* camera_serial);
static void write_block_json_to_sd(const char* serial);
// static void verify_latest_json_on_all_disks_for(const char *scenario_str); // debug
/*** ---- CURL buffer (reprend ton hello.c) ---- ***/
struct MemoryStruct
{
  char *memory;
  size_t size;
};

/* NEW: numéro de série global, utilisé aux flush 15' et on SIGTERM */
static char g_camera_serial[64] = "ma_camra_b8...ce99"; // par défaut
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
typedef struct {
  int car, bike, truck, bus, human, other, total;
} ClassTotals;

typedef struct {
  char from_iso[32], to_iso[32];   // bords minute (UTC)
  gboolean has_dir1, has_dir2;     // flags d'arrivée partielle
  ClassTotals dir1, dir2;          // deltas/minute
} MinuteEntry2;

typedef struct {
  GDateTime* block_start_utc;     /* début fenêtre 15 min (UTC) */
  MinuteEntry2 m[15];              /* 15 buckets */
  int filled;                     /* combien de minutes remplies */
} Block15x2;

static Block15x2 g_block = {0};

// cumulés précédents indépendants par direction
static ClassTotals g_prev_dir1 = {0};
static ClassTotals g_prev_dir2 = {0};
static char g_prev_reset_time_dir1[40] = {0};
static char g_prev_reset_time_dir2[40] = {0};
// static char g_prev_reset_time[40] = {0};

typedef enum { DIR1=0, DIR2=1 } DirKind;

// typedef struct {
//   char cam_url[256], user[64], pass[64], scenario[32], api_version[8];
//   DirKind dir;            // <- quel sens on remplit
//   gint period_sec;
// } PollCfg;

static ClassTotals compute_delta(const ClassTotals* now, const ClassTotals* prev) {
  ClassTotals d = {
    .car   = now->car   >= prev->car   ? now->car   - prev->car   : 0,
    .bike  = now->bike  >= prev->bike  ? now->bike  - prev->bike  : 0,
    .truck = now->truck >= prev->truck ? now->truck - prev->truck : 0,
    .bus   = now->bus   >= prev->bus   ? now->bus   - prev->bus   : 0,
    .human = now->human >= prev->human ? now->human - prev->human : 0,
    .other = now->other >= prev->other ? now->other - prev->other : 0,
    .total = now->total >= prev->total ? now->total - prev->total : 0,
  };
  return d;
}

static void ensure_block_for_now(GDateTime* now_utc) {
  // si pas de bloc ou changement de tranche 15’
  GDateTime* start = g_date_time_new_utc(
    g_date_time_get_year(now_utc), g_date_time_get_month(now_utc), g_date_time_get_day_of_month(now_utc),
    g_date_time_get_hour(now_utc), (g_date_time_get_minute(now_utc)/15)*15, 0);
  if (!g_block.block_start_utc ||
      g_date_time_difference(now_utc, g_block.block_start_utc) >= 15*60*G_TIME_SPAN_SECOND ||
      g_date_time_difference(now_utc, g_block.block_start_utc) < 0) {
    if (g_block.block_start_utc) g_date_time_unref(g_block.block_start_utc);
    memset(&g_block, 0, sizeof(g_block));
    g_block.block_start_utc = start; // ownership
  } else {
    g_date_time_unref(start);
  }
}
static int minute_index(GDateTime* now, GDateTime* start){
  gint64 secs = g_date_time_difference(now, start) / G_TIME_SPAN_SECOND;
  if (secs < 0) secs = 0; 
  int idx = (int)(secs / 60);
  if (idx < 0) idx = 0; 
  if (idx > 14) idx = 14;
  return idx;
}

static void put_delta_in_minute(GDateTime* now_utc, DirKind dir, const ClassTotals* delta) {
  ensure_block_for_now(now_utc);
  int idx = minute_index(now_utc, g_block.block_start_utc);

  if (g_block.m[idx].from_iso[0] == 0) {
    // borne exacte de la minute (from inclusive, to exclusive)
    GDateTime* from = g_date_time_new_utc(
      g_date_time_get_year(now_utc), g_date_time_get_month(now_utc), g_date_time_get_day_of_month(now_utc),
      g_date_time_get_hour(now_utc), g_date_time_get_minute(now_utc), 0);
    GDateTime* to   = g_date_time_add_seconds(from, 60);
    gchar* f = g_date_time_format(from, "%Y-%m-%dT%H:%M:%SZ");
    gchar* t = g_date_time_format(to,   "%Y-%m-%dT%H:%M:%SZ");
    g_strlcpy(g_block.m[idx].from_iso, f, sizeof g_block.m[idx].from_iso);
    g_strlcpy(g_block.m[idx].to_iso,   t, sizeof g_block.m[idx].to_iso);
    g_free(f); g_free(t); g_date_time_unref(from); g_date_time_unref(to);
  }

  if (dir == DIR1) { g_block.m[idx].dir1 = *delta; g_block.m[idx].has_dir1 = TRUE; }
  else             { g_block.m[idx].dir2 = *delta; g_block.m[idx].has_dir2 = TRUE; }
}

/* tronque un timestamp UTC à un multiple de 15 min */
// static GDateTime* floor_to_15min(GDateTime* t) {
//   int min = g_date_time_get_minute(t);
//   int flo = (min/15)*15;
//   return g_date_time_new_utc(
//     g_date_time_get_year(t), g_date_time_get_month(t), g_date_time_get_day_of_month(t),
//     g_date_time_get_hour(t), flo, 0);
// }

/* retourne l’index 0..14 pour la minute courante dans la fenêtre */
// static int minute_index_in_block(GDateTime* now_utc) {
//   int m = g_date_time_get_minute(now_utc);
//   return m % 15;
// }

/* ISO helpers */
// static void iso_min_bounds(GDateTime* t_utc, char out_from[32], char out_to[32]) {
//   GDateTime* from = g_date_time_new_utc(
//     g_date_time_get_year(t_utc), g_date_time_get_month(t_utc), g_date_time_get_day_of_month(t_utc),
//     g_date_time_get_hour(t_utc), g_date_time_get_minute(t_utc), 0);
//   GDateTime* to = g_date_time_add_seconds(from, 60);
//   gchar* f = g_date_time_format(from, "%Y-%m-%dT%H:%M:%SZ");
//   gchar* t = g_date_time_format(to,   "%Y-%m-%dT%H:%M:%SZ");
//   g_strlcpy(out_from, f, 32);
//   g_strlcpy(out_to,   t, 32);
//   g_free(f); g_free(t);
//   g_date_time_unref(from); g_date_time_unref(to);
// }
/* --- parse cumul depuis la réponse (sscanf naïf, suffisant pour ce JSON) --- */
static void parse_cumul_totals(const char* json, ClassTotals* out, char reset_time[40]) {
  memset(out, 0, sizeof(*out));
  if (!json) return;
  /* resetTime */
  const char* rt = strstr(json, "\"resetTime\"");
  if (rt) sscanf(rt, "\"resetTime\"%*[^\"]\"%39[^\"]", reset_time);

  const char* p;
  if ((p=strstr(json,"\"totalCar\"")))        sscanf(p, "\"totalCar\"%*[^0-9]%d", &out->car);
  if ((p=strstr(json,"\"totalBike\"")))       sscanf(p, "\"totalBike\"%*[^0-9]%d", &out->bike);
  if ((p=strstr(json,"\"totalTruck\"")))      sscanf(p, "\"totalTruck\"%*[^0-9]%d", &out->truck);
  if ((p=strstr(json,"\"totalBus\"")))        sscanf(p, "\"totalBus\"%*[^0-9]%d", &out->bus);
  if ((p=strstr(json,"\"totalHuman\"")))      sscanf(p, "\"totalHuman\"%*[^0-9]%d", &out->human);
  if ((p=strstr(json,"\"totalOtherVehicle\""))) sscanf(p, "\"totalOtherVehicle\"%*[^0-9]%d", &out->other);
  if ((p=strstr(json,"\"total\"")))           sscanf(p, "\"total\"%*[^0-9]%d", &out->total);
}

/* delta = cumul_now - g_prev (>=0) */
// static ClassTotals compute_delta(const ClassTotals* now) {
//   ClassTotals d = {
//     .car   = (now->car   >= g_prev.car   ? now->car   - g_prev.car   : 0),
//     .bike  = (now->bike  >= g_prev.bike  ? now->bike  - g_prev.bike  : 0),
//     .truck = (now->truck >= g_prev.truck ? now->truck - g_prev.truck : 0),
//     .bus   = (now->bus   >= g_prev.bus   ? now->bus   - g_prev.bus   : 0),
//     .human = (now->human >= g_prev.human ? now->human - g_prev.human : 0),
//     .other = (now->other >= g_prev.other ? now->other - g_prev.other : 0),
//     .total = (now->total >= g_prev.total ? now->total - g_prev.total : 0),
//   };
//   return d;
// }

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
  if (loop){
    if (g_block.block_start_utc) write_block_json_to_sd(g_camera_serial);
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
  }
}

/*** ---- Helper: écriture JSON sur tous les disques prêts ---- ***/
// static void write_json_on_all_disks(const char *basename, const char *json, const char *scenario_str)
// {
//   for (GList *n = g_list_first(disks_list); n != NULL; n = g_list_next(n))
//   {
//     disk_item_t *d = n->data;
//     if (!d->available || !d->writable || d->full || !d->setup || !d->storage_path)
//       continue;

//     // Dossier dédié au scénario
//     gchar *outdir = g_strdup_printf("%s/aoa_counts/scenario_%s", d->storage_path, scenario_str);
//     g_mkdir_with_parents(outdir, 0775);

//     // fichier horodaté
//     GDateTime *now = g_date_time_new_now_utc();
//     gchar *ts = g_date_time_format(now, "%Y%m%dT%H%M%SZ");
//     gchar *path = g_strdup_printf("%s/%s_%s.json", outdir, basename, ts);

//     FILE *f = g_fopen(path, "w");
//     if (!f)
//     {
//       syslog(LOG_WARNING, "open %s failed", path);
//     }
//     else
//     {
//       g_fprintf(f, "%s\n", json);
//       fclose(f);
//       syslog(LOG_INFO, "Wrote %s", path);
//     }
//     g_free(path);
//     g_free(ts);
//     g_free(outdir);
//     g_date_time_unref(now);
//   }
// }
static void write_json_on_all_disks_serial(const char* basename,
                                           const char* json,
                                           const char* camera_serial) {
  for (GList* n = g_list_first(disks_list); n; n = g_list_next(n)) {
    disk_item_t* d = n->data;
    if (!d->available || !d->writable || d->full || !d->setup || !d->storage_path) continue;
    gchar* outdir = g_strdup_printf("%s/aoa_counts/%s", d->storage_path, camera_serial);
    g_mkdir_with_parents(outdir, 0775);
    GDateTime* now = g_date_time_new_now_utc();
    gchar* ts = g_date_time_format(now, "%Y%m%dT%H%M%SZ");
    gchar* path = g_strdup_printf("%s/%s_%s.json", outdir, basename, ts);
    FILE* f = g_fopen(path, "w");
    if (f) { g_fprintf(f, "%s\n", json); fclose(f); syslog(LOG_INFO, "Wrote %s", path); }
    else   { syslog(LOG_WARNING, "open %s failed", path); }
    g_free(path); g_free(ts); g_free(outdir); g_date_time_unref(now);
  }
}

// static void write_block_json_to_sd_with_scenario(const char* scenario_str) {
//   if (!g_block.block_start_utc || g_block.filled==0) return;
  //   // JSON: on inclut "scenario"
  //   GString* js = g_string_new(NULL);
  //   g_string_append_printf(js,
  //     "{"
  //       "\"scenario\":\"%s\","
  //       "\"sensor-time\":{\"timezone\":\"Europe/Paris\"},"
  //       "\"content\":{\"element\":[{"
  //         "\"element-id\":0,"
  //         "\"data-type\":\"LINE\","
  //         "\"resolution\":\"ONE_MINUTE\","
  //         "\"measurement\":[",
  //     scenario_str
  //   );

  //   for (int i=0;i<15;i++){
  //     if (!g_block.m[i].filled) continue;
  //     if (i>0) g_string_append(js, ",");
  //     g_string_append_printf(js,
  //       "{"
  //         "\"from\":\"%s\",\"to\":\"%s\","
  //         "\"value\":["
  //           "{\"label\":\"car\",\"value\":%d},"
  //           "{\"label\":\"bike\",\"value\":%d},"
  //           "{\"label\":\"truck\",\"value\":%d},"
  //           "{\"label\":\"bus\",\"value\":%d},"
  //           "{\"label\":\"human\",\"value\":%d},"
  //           "{\"label\":\"other\",\"value\":%d},"
  //           "{\"label\":\"total\",\"value\":%d}"
  //         "]"
  //       "}",
  //       g_block.m[i].from_iso, g_block.m[i].to_iso,
  //       g_block.m[i].delta.car, g_block.m[i].delta.bike, g_block.m[i].delta.truck,
  //       g_block.m[i].delta.bus, g_block.m[i].delta.human, g_block.m[i].delta.other,
  //       g_block.m[i].delta.total
  //     );
  //   }
  //   g_string_append(js, "]}}]}");

  //   // Nom de fichier: counts_<SCENARIO>_<startUTC>_15min.json
  //   gchar* start_str = g_date_time_format(g_block.block_start_utc, "%Y%m%dT%H%MZ");
  //   gchar* fname = g_strdup_printf("counts_%s_%s_15min", scenario_str, start_str);
  //   write_json_on_all_disks(fname, js->str, scenario_str);
  //   g_free(start_str); g_free(fname);
  //   g_string_free(js, TRUE);
  // }

/* démarre une nouvelle fenêtre 15 min et vide le buffer */
// static void start_new_block(GDateTime* start_utc) {
//   if (g_block.block_start_utc) g_date_time_unref(g_block.block_start_utc);
//   memset(&g_block, 0, sizeof(g_block));
//   g_block.block_start_utc = g_date_time_ref(start_utc);
// }
static gboolean get_camera_serial(char out[64], const char* user, const char* pass) {
  // VAPIX: /axis-cgi/param.cgi?action=list&group=Properties.System.SerialNumber
  CURL* c = curl_easy_init(); if(!c) return FALSE;
  struct MemoryStruct buf = {0};
  char url[256]; snprintf(url,sizeof url,"http://127.0.0.1/axis-cgi/param.cgi?action=list&group=Properties.System.SerialNumber");
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERNAME, user);
  curl_easy_setopt(c, CURLOPT_PASSWORD, pass);
  curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
  CURLcode rc = curl_easy_perform(c);
  if (rc!=CURLE_OK || !buf.memory) { free(buf.memory); curl_easy_cleanup(c); return FALSE; }
  // réponse contient: Properties.System.SerialNumber=ACCC8E123456
  char* p = strstr(buf.memory, "Properties.System.SerialNumber=");
  gboolean ok = FALSE;
  if (p) {
    p += strlen("Properties.System.SerialNumber=");
    // copie jusqu'à fin de ligne
    size_t i=0; while (p[i] && p[i]!='\r' && p[i]!='\n' && i<63) { out[i]=p[i]; i++; }
    out[i]='\0'; ok = (i>0);
  }
  free(buf.memory); curl_easy_cleanup(c);
  return ok;
}

static void write_block_json_to_sd(const char* serial) {
  if (!g_block.block_start_utc) return;

  GString* js = g_string_new(NULL);
  g_string_append_printf(js,
    "{"
      "\"cameraSerial\":\"%s\","
      "\"sensor-time\":{\"timezone\":\"Europe/Paris\"},"
      "\"content\":{\"element\":[{"
        "\"element-id\":0,"
        "\"data-type\":\"LINE\","
        "\"resolution\":\"ONE_MINUTE\","
        "\"measurement\":[",
    serial ? serial : ""
  );

  gboolean first = TRUE;
  for (int i=0;i<15;i++) {
    if (g_block.m[i].from_iso[0]==0) continue;
    if (!first) g_string_append(js, ",");
    first = FALSE;

    // total = dir1 + dir2, classe par classe
    ClassTotals tot = {
      .car   = g_block.m[i].dir1.car   + g_block.m[i].dir2.car,
      .bike  = g_block.m[i].dir1.bike  + g_block.m[i].dir2.bike,
      .truck = g_block.m[i].dir1.truck + g_block.m[i].dir2.truck,
      .bus   = g_block.m[i].dir1.bus   + g_block.m[i].dir2.bus,
      .human = g_block.m[i].dir1.human + g_block.m[i].dir2.human,
      .other = g_block.m[i].dir1.other + g_block.m[i].dir2.other,
      .total = g_block.m[i].dir1.total + g_block.m[i].dir2.total
    };

    g_string_append_printf(js,
      "{"
        "\"from\":\"%s\",\"to\":\"%s\","
        "\"counts\":{"
          "\"dir1\":["
            "{\"label\":\"car\",\"value\":%d},"
            "{\"label\":\"bike\",\"value\":%d},"
            "{\"label\":\"truck\",\"value\":%d},"
            "{\"label\":\"bus\",\"value\":%d},"
            "{\"label\":\"human\",\"value\":%d},"
            "{\"label\":\"other\",\"value\":%d}"
          "],"
          "\"dir2\":["
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
      g_block.m[i].from_iso, g_block.m[i].to_iso,
      // dir1
      g_block.m[i].dir1.car, g_block.m[i].dir1.bike, g_block.m[i].dir1.truck,
      g_block.m[i].dir1.bus, g_block.m[i].dir1.human, g_block.m[i].dir1.other,
      // dir2
      g_block.m[i].dir2.car, g_block.m[i].dir2.bike, g_block.m[i].dir2.truck,
      g_block.m[i].dir2.bus, g_block.m[i].dir2.human, g_block.m[i].dir2.other,
      // total
      tot.car, tot.bike, tot.truck, tot.bus, tot.human, tot.other
    );
  }
  g_string_append(js, "]}}]}");

  // nom de fichier basé sur le début de tranche 15’, sans “scenario”
  gchar* start_str = g_date_time_format(g_block.block_start_utc, "%Y%m%dT%H%MZ");
  gchar* fname = g_strdup_printf("counts_%s_%s_15min", serial?serial:"unknown", start_str);

  // réutilise ta fonction d’écriture (dossier unique aoa_counts/)
  write_json_on_all_disks_serial(fname, js->str,serial);  // même signature qu’actuelle sans “scenario”
  syslog(LOG_INFO, "AVANT LAPPEL DE VERIF POUR SERIAL ");

  verify_latest_json_on_all_disks_for_serial(serial); // debug
  g_free(start_str); g_free(fname);
  g_string_free(js, TRUE);

  // reset le bloc après écriture
  g_date_time_unref(g_block.block_start_utc);
  memset(&g_block, 0, sizeof(g_block));
}
static void maybe_flush_15min(const char* user, const char* pass) {
  if (!g_block.block_start_utc) return;
  
  // si la minute courante est le début d’une nouvelle tranche 15’, on flush l’ancienne
  GDateTime* now = g_date_time_new_now_utc();
  gint cur_min = g_date_time_get_minute(now);
  gint blk_min = g_date_time_get_minute(g_block.block_start_utc);
  // changement de tranche si cur_min/15 != blk_min/15 OU si heure/jour a bougé
  gboolean tranche_change =
  (cur_min/15 != blk_min/15) ||
  (g_date_time_get_hour(now) != g_date_time_get_hour(g_block.block_start_utc)) ||
  (g_date_time_get_day_of_month(now) != g_date_time_get_day_of_month(g_block.block_start_utc));
  
  syslog(LOG_INFO, "maybe_flush_15: now=%02d blk=%02d",
       g_date_time_get_minute(now),
       g_date_time_get_minute(g_block.block_start_utc));
  if (tranche_change) {
    char serial[64]={0};
    if (!get_camera_serial(serial, user, pass)) strcpy(serial,"unknown");
    write_block_json_to_sd(serial);
    // la prochaine insertion recréera un nouveau bloc dans ensure_block_for_now()
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
// static void verify_latest_json_on_all_disks_for(const char *scenario_str)
// {
//   for (GList *n = g_list_first(disks_list); n != NULL; n = g_list_next(n))
//   {
//     disk_item_t *d = n->data;
//     if (!d || !d->storage_path)
//       continue;

//     gchar *dir = g_strdup_printf("%s/aoa_counts/scenario_%s", d->storage_path, scenario_str);
//     gchar *latest = find_latest_json_in_dir(dir);
//     if (latest)
//       read_and_print_file(latest);
//     else
//       g_printerr("No JSON file found in %s\n", dir);
//     g_free(latest);
//     g_free(dir);
//   }
// }
/* nouvelle vérif “par serial” */
static void verify_latest_json_on_all_disks_for_serial(const char* camera_serial) {
  // afficher un message de debug ici 
  syslog(LOG_INFO, "Verifying latest JSON files for camera serial: %s", camera_serial);
  // g_print("\n=== Verifying latest JSON files for camera serial:  ===\n", camera_serial);
  for (GList* n = g_list_first(disks_list); n; n = g_list_next(n)) {
    disk_item_t* d = n->data;
    if (!d || !d->storage_path) continue;
    gchar* dir = g_strdup_printf("%s/aoa_counts/%s", d->storage_path, camera_serial);
    gchar* latest = find_latest_json_in_dir(dir);
    if (latest) read_and_print_file(latest);
    else g_printerr("No JSON file found in %s\n", dir);
    g_free(latest); g_free(dir);
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
  DirKind dir;            // <- quel sens on remplit
  gint period_sec;     // cadence
} PollCfg;

static gboolean tick_poll_and_store(gpointer user_data)
{
  PollCfg *cfg = (PollCfg *)user_data;

  CURL *curl = curl_easy_init();
  if (!curl) { syslog(LOG_ERR, "curl init failed"); return G_SOURCE_CONTINUE; }

  struct MemoryStruct chunk = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  // Corps JSON (scenario: nombre ou UID)
  char body[512];
  gboolean numeric = TRUE;
  for (const char *p = cfg->scenario; *p; ++p) { if (*p<'0'||*p>'9'){ numeric=FALSE; break; } }
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
  if (rc != CURLE_OK) {
    syslog(LOG_WARNING, "curl error: %s", curl_easy_strerror(rc));
  } else {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code == 200 && chunk.memory && chunk.size) {

      // 1) Parse cumul & resetTime
      char rt[40]={0};
      ClassTotals now={0};
      parse_cumul_totals(chunk.memory, &now, rt);
// choisir les bons "prev"/"reset" selon la direction
ClassTotals* prev = (cfg->dir==DIR1) ? &g_prev_dir1 : &g_prev_dir2;
char* prev_rt     = (cfg->dir==DIR1) ? g_prev_reset_time_dir1 : g_prev_reset_time_dir2;

// reset si resetTime a changé
if (prev_rt[0]==0 || strcmp(rt, prev_rt)!=0) {
  g_strlcpy(prev_rt, rt, 40);
  *prev = now;  // rebasing
}

// delta et dépôt dans le bucket minute
ClassTotals d = compute_delta(&now, prev);
*prev = now;

GDateTime* now_utc = g_date_time_new_now_utc();
put_delta_in_minute(now_utc, cfg->dir, &d);
g_date_time_unref(now_utc);

      // 2) Gérer reset (rebase des cumulés)
      // if (g_prev_reset_time[0]==0 || strcmp(rt, g_prev_reset_time)!=0) {
      //   memcpy(g_prev_reset_time, rt, sizeof(g_prev_reset_time));
      //   g_prev = now; // base = état actuel
      // }

      // 3) Calculer le delta/minute
      // ClassTotals d = compute_delta(&now);
      // g_prev = now;

      // 4) Fenêtre 15 min
      // GDateTime* now_utc = g_date_time_new_now_utc();
      // GDateTime* blk = floor_to_15min(now_utc);
      // int idx = minute_index_in_block(now_utc);

      // Changement de fenêtre -> flush l’ancienne
      // if (!g_block.block_start_utc || !g_date_time_equal(blk, g_block.block_start_utc)) {
      //   if (g_block.filled > 0) write_block_json_to_sd_with_scenario(cfg->scenario);
      //   start_new_block(blk);
      // }

      // Remplir la minute idx
      // iso_min_bounds(now_utc, g_block.m[idx].from_iso, g_block.m[idx].to_iso);
      // g_block.m[idx].delta   = d;
      // g_block.m[idx].filled  = TRUE;
      // if (idx+1 > g_block.filled) g_block.filled = idx+1;
      syslog(LOG_INFO, "CUM now c=%d b=%d t=%d bus=%d h=%d o=%d tot=%d",
            now.car,now.bike,now.truck,now.bus,now.human,now.other,now.total);
      syslog(LOG_INFO, "DELTA   d c=%d b=%d t=%d bus=%d h=%d o=%d tot=%d",
            d.car,d.bike,d.truck,d.bus,d.human,d.other,d.total);

      // Si la fenêtre est complète (15/15) -> écrire fichier 15 min
      // if (g_block.filled == 15) {
      //   write_block_json_to_sd_with_scenario(cfg->scenario);
      //   GDateTime* next_blk = g_date_time_add_minutes(g_block.block_start_utc, 15);
      //   start_new_block(next_blk);
      //   g_date_time_unref(next_blk);
      // }

      // Option: vérifier le dernier fichier du bon scénario
      // verify_latest_json_on_all_disks_for(cfg->scenario);

      // g_date_time_unref(blk);
      // g_date_time_unref(now_utc);
    } else {
      syslog(LOG_WARNING, "HTTP %ld (no payload)", http_code);
    }
  }

  curl_slist_free_all(headers);
  free(chunk.memory);
  curl_easy_cleanup(curl);
  return G_SOURCE_CONTINUE;
}
// calcule le début de fenêtre (UTC) : 00/15/30/45
static GDateTime* floor15(GDateTime* now){
  int m = g_date_time_get_minute(now);
  int q = (m/15)*15;
  return g_date_time_new_utc(g_date_time_get_year(now),
                             g_date_time_get_month(now),
                             g_date_time_get_day_of_month(now),
                             g_date_time_get_hour(now), q, 0);
}

static PollCfg g_dir1, g_dir2;
/* --- Tick chaque minute: poll + store + flush si nouvelle tranche 15’ --- */
static gboolean minute_tick(gpointer unused) {
  (void)unused;
  GDateTime* now = g_date_time_new_now_utc();
GDateTime* win = floor15(now);

if (!g_block.block_start_utc) {
  g_block.block_start_utc = g_date_time_ref(win);
  // (réinitialise g_block.m[...] si besoin)
} else if (!g_date_time_equal(g_block.block_start_utc, win)) {
  // ↙️ on vient de franchir un multiple de 15 → flush l’ancienne fenêtre
  write_block_json_to_sd(g_camera_serial);
  // réinitialise la fenêtre
  g_date_time_unref(g_block.block_start_utc);
  g_block.block_start_utc = g_date_time_ref(win);
  // clear g_block.m[...] (flags dir1/dir2 à FALSE)
}
g_date_time_unref(win);

  tick_poll_and_store(&g_dir1);
  tick_poll_and_store(&g_dir2);
  // potentiellement flush si on vient d’entrer dans une nouvelle tranche 15’
  maybe_flush_15min(g_dir1.user, g_dir1.pass);
  return G_SOURCE_CONTINUE;
}

/*** ---- main : init storage, s'abonner, timer pour poll ---- ***/
int main(void)
{
  syslog(LOG_INFO, "Start AOA→SD app");

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

  // Config du polling (à adapter)
  // static PollCfg cfg;
  // snprintf(cfg.cam_url, sizeof cfg.cam_url, "http://192.168.1.51/local/objectanalytics/control.cgi");
  // snprintf(cfg.user, sizeof cfg.user, "champlein");     // <-- remplace
  // snprintf(cfg.pass, sizeof cfg.pass, "696969");        // <-- remplace
  // snprintf(cfg.scenario, sizeof cfg.scenario, "2");     // ou "S1" (UID)
  // snprintf(cfg.api_version, sizeof cfg.api_version, "1.6");
  snprintf(g_dir1.cam_url,sizeof g_dir1.cam_url,"http://192.168.1.51/local/objectanalytics/control.cgi");
  strcpy(g_dir1.user,"champlein"); strcpy(g_dir1.pass,"696969");
  strcpy(g_dir1.scenario,"4"); strcpy(g_dir1.api_version,"1.6");
  g_dir1.dir = DIR1; g_dir1.period_sec = 60;

  g_dir2 = g_dir1; strcpy(g_dir2.scenario,"2"); g_dir2.dir = DIR2;
  // cfg.period_sec = 60;

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
  return EXIT_SUCCESS;
}
