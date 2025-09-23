// /**
//  * Copyright (C) 2023 Axis Communications AB, Lund, Sweden
//  *
//  * Licensed under the Apache License, Version 2.0 (the "License");
//  * you may not use this file except in compliance with the License.
//  * You may obtain a copy of the License at
//  *
//  *     <http://www.apache.org/licenses/LICENSE-2.0>
//  *
//  * Unless required by applicable law or agreed to in writing, software
//  * distributed under the License is distributed on an "AS IS" BASIS,
//  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  * See the License for the specific language governing permissions and
//  * limitations under the License.
//  */

// #include <errno.h>
// #include <glib-unix.h>
// #include <glib.h>
// #include <glib/gstdio.h>
// #include <signal.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <syslog.h>

// /* AX Storage library. */
// #include <axsdk/axstorage.h>

// /**
//  * disk_item_t represents one storage device and its values.
//  */
// typedef struct {
//     AXStorage* storage;         /** AXStorage reference. */
//     AXStorageType storage_type; /** Storage type */
//     gchar* storage_id;          /** Storage device name. */
//     gchar* storage_path;        /** Storage path. */
//     guint subscription_id;      /** Subscription ID for storage events. */
//     gboolean setup;             /** TRUE: storage was set up async, FALSE otherwise. */
//     gboolean writable;          /** Storage is writable or not. */
//     gboolean available;         /** Storage is available or not. */
//     gboolean full;              /** Storage device is full or not. */
//     gboolean exiting;           /** Storage is exiting (going to disappear) or not. */
// } disk_item_t;

// static GList* disks_list = NULL;

// /**
//  * @brief Handles the signals.
//  *
//  * @param loop Loop to quit
//  */
// static gboolean signal_handler(gpointer loop) {
//     g_main_loop_quit((GMainLoop*)loop);
//     syslog(LOG_INFO, "Application was stopped by SIGTERM or SIGINT.");
//     return G_SOURCE_REMOVE;
// }

// /**
//  * @brief Callback function registered by g_timeout_add_seconds(),
//  *        which is triggered every 10th second and writes data to disk
//  *
//  * @param data The storage to subscribe to its events
//  *
//  * @return Result
//  */
// static gboolean write_data(const gchar* data) {
//     static guint counter = 0;
//     GList* node          = NULL;
//     gboolean ret         = TRUE;
//     for (node = g_list_first(disks_list); node != NULL; node = g_list_next(node)) {
//         disk_item_t* item = node->data;

//         /* Write data to disk when it is available, writable and has disk space
//            and the setup has been done. */
//         if (item->available && item->writable && !item->full && item->setup) {
//             gchar* filename = g_strdup_printf("%s/%s.log", item->storage_path, data);

//             FILE* file = g_fopen(filename, "a");
//             if (file == NULL) {
//                 syslog(LOG_WARNING, "Failed to open %s. Error %s.", filename, g_strerror(errno));
//                 ret = FALSE;
//             } else {
//                 g_fprintf(file, "counter: %d\n", ++counter);
//                 fclose(file);
//                 syslog(LOG_INFO, "Writing to %s", filename);
//             }
//             g_free(filename);
//         }
//     }
//     return ret;
// }

// /**
//  * @brief Find disk item in disks_list
//  *
//  * @param storage_id The storage to subscribe to its events
//  *
//  * @return Disk item
//  */
// static disk_item_t* find_disk_item_t(gchar* storage_id) {
//     GList* node       = NULL;
//     disk_item_t* item = NULL;

//     for (node = g_list_first(disks_list); node != NULL; node = g_list_next(node)) {
//         item = node->data;

//         if (g_strcmp0(storage_id, item->storage_id) == 0) {
//             return item;
//         }
//     }
//     return NULL;
// }

// /**
//  * @brief Callback function registered by ax_storage_release_async(),
//  *        which is triggered to release the disk
//  *
//  * @param user_data storage_id of a disk
//  * @param error Returned errors
//  */
// static void release_disk_cb(gpointer user_data, GError* error) {
//     syslog(LOG_INFO, "Release of %s", (gchar*)user_data);
//     if (error != NULL) {
//         syslog(LOG_WARNING, "Error while releasing %s: %s", (gchar*)user_data, error->message);
//         g_error_free(error);
//     }
// }

// /**
//  * @brief Free disk items from disks_list
//  */
// static void free_disk_item_t(void) {
//     GList* node = NULL;

//     for (node = g_list_first(disks_list); node != NULL; node = g_list_next(node)) {
//         GError* error     = NULL;
//         disk_item_t* item = node->data;

//         if (item->setup) {
//             /* NOTE: It is advised to finish all your reading/writing operations
//                before releasing the storage device. */
//             ax_storage_release_async(item->storage, release_disk_cb, item->storage_id, &error);
//             if (error != NULL) {
//                 syslog(LOG_WARNING,
//                        "Failed to release %s. Error: %s",
//                        item->storage_id,
//                        error->message);
//                 g_clear_error(&error);
//             } else {
//                 syslog(LOG_INFO, "Release of %s was successful", item->storage_id);
//                 item->setup = FALSE;
//             }
//         }

//         ax_storage_unsubscribe(item->subscription_id, &error);
//         if (error != NULL) {
//             syslog(LOG_WARNING,
//                    "Failed to unsubscribe event of %s. Error: %s",
//                    item->storage_id,
//                    error->message);
//             g_clear_error(&error);
//         } else {
//             syslog(LOG_INFO, "Unsubscribed events of %s", item->storage_id);
//         }
//         g_free(item->storage_id);
//         g_free(item->storage_path);
//     }
//     g_list_free(disks_list);
// }

// /**
//  * @brief Callback function registered by ax_storage_setup_async(),
//  *        which is triggered to setup a disk
//  *
//  * @param storage storage_id of a disk
//  * @param user_data
//  * @param error Returned errors
//  */
// static void setup_disk_cb(AXStorage* storage, gpointer user_data, GError* error) {
//     GError* ax_error  = NULL;
//     gchar* storage_id = NULL;
//     gchar* path       = NULL;
//     AXStorageType storage_type;
//     (void)user_data;

//     if (storage == NULL || error != NULL) {
//         syslog(LOG_ERR, "Failed to setup disk. Error: %s", error->message);
//         g_error_free(error);
//         goto free_variables;
//     }

//     storage_id = ax_storage_get_storage_id(storage, &ax_error);
//     if (ax_error != NULL) {
//         syslog(LOG_WARNING, "Failed to get storage_id. Error: %s", ax_error->message);
//         g_error_free(ax_error);
//         goto free_variables;
//     }

//     path = ax_storage_get_path(storage, &ax_error);
//     if (ax_error != NULL) {
//         syslog(LOG_WARNING, "Failed to get storage path. Error: %s", ax_error->message);
//         g_error_free(ax_error);
//         goto free_variables;
//     }

//     storage_type = ax_storage_get_type(storage, &ax_error);
//     if (ax_error != NULL) {
//         syslog(LOG_WARNING, "Failed to get storage type. Error: %s", ax_error->message);
//         g_error_free(ax_error);
//         goto free_variables;
//     }

//     disk_item_t* disk = find_disk_item_t(storage_id);
//     /* The storage pointer is created in this callback, assign it to
//        disk_item_t instance. */
//     disk->storage      = storage;
//     disk->storage_type = storage_type;
//     disk->storage_path = g_strdup(path);
//     disk->setup        = TRUE;

//     syslog(LOG_INFO, "Disk: %s has been setup in %s", storage_id, path);
// free_variables:
//     g_free(storage_id);
//     g_free(path);
// }

// /**
//  * @brief Subscribe to the events of the storage
//  *
//  * @param storage_id The storage to subscribe to its events
//  * @param user_data User data to be processed
//  * @param error Returned errors
//  */
// static void subscribe_cb(gchar* storage_id, gpointer user_data, GError* error) {
//     GError* ax_error = NULL;
//     gboolean available;
//     gboolean writable;
//     gboolean full;
//     gboolean exiting;
//     (void)user_data;

//     if (error != NULL) {
//         syslog(LOG_WARNING, "Failed to subscribe to %s. Error: %s", storage_id, error->message);
//         g_error_free(error);
//         return;
//     }

//     syslog(LOG_INFO, "Subscribe for the events of %s", storage_id);
//     disk_item_t* disk = find_disk_item_t(storage_id);

//     /* Get the status of the events. */
//     exiting = ax_storage_get_status(storage_id, AX_STORAGE_EXITING_EVENT, &ax_error);
//     if (ax_error != NULL) {
//         syslog(LOG_WARNING,
//                "Failed to get EXITING event for %s. Error: %s",
//                storage_id,
//                ax_error->message);
//         g_error_free(ax_error);
//         return;
//     }

//     available = ax_storage_get_status(storage_id, AX_STORAGE_AVAILABLE_EVENT, &ax_error);
//     if (ax_error != NULL) {
//         syslog(LOG_WARNING,
//                "Failed to get AVAILABLE event for %s. Error: %s",
//                storage_id,
//                ax_error->message);
//         g_error_free(ax_error);
//         return;
//     }

//     writable = ax_storage_get_status(storage_id, AX_STORAGE_WRITABLE_EVENT, &ax_error);
//     if (ax_error != NULL) {
//         syslog(LOG_WARNING,
//                "Failed to get WRITABLE event for %s. Error: %s",
//                storage_id,
//                ax_error->message);
//         g_error_free(ax_error);
//         return;
//     }

//     full = ax_storage_get_status(storage_id, AX_STORAGE_FULL_EVENT, &ax_error);
//     if (ax_error != NULL) {
//         syslog(LOG_WARNING,
//                "Failed to get FULL event for %s. Error: %s",
//                storage_id,
//                ax_error->message);
//         g_error_free(ax_error);
//         return;
//     }

//     disk->writable  = writable;
//     disk->available = available;
//     disk->exiting   = exiting;
//     disk->full      = full;

//     syslog(LOG_INFO,
//            "Status of events for %s: %swritable, %savailable, %sexiting, %sfull",
//            storage_id,
//            writable ? "" : "not ",
//            available ? "" : "not ",
//            exiting ? "" : "not ",
//            full ? "" : "not ");

//     /* If exiting, and the disk was set up before, release it. */
//     if (exiting && disk->setup) {
//         /* NOTE: It is advised to finish all your reading/writing operations before
//            releasing the storage device. */
//         ax_storage_release_async(disk->storage, release_disk_cb, storage_id, &ax_error);

//         if (ax_error != NULL) {
//             syslog(LOG_WARNING, "Failed to release %s. Error %s.", storage_id, ax_error->message);
//             g_error_free(ax_error);
//         } else {
//             syslog(LOG_INFO, "Release of %s was successful", storage_id);
//             disk->setup = FALSE;
//         }

//         /* Writable implies that the disk is available. */
//     } else if (writable && !full && !exiting && !disk->setup) {
//         syslog(LOG_INFO, "Setup %s", storage_id);
//         ax_storage_setup_async(storage_id, setup_disk_cb, NULL, &ax_error);

//         if (ax_error != NULL) {
//             /* NOTE: It is advised to try to setup again in case of failure. */
//             syslog(LOG_WARNING, "Failed to setup %s, reason: %s", storage_id, ax_error->message);
//             g_error_free(ax_error);
//         } else {
//             syslog(LOG_INFO, "Setup of %s was successful", storage_id);
//         }
//     }
// }

// /**
//  * @brief Subscribes to disk events and creates new disk item
//  *
//  * @param storage_id storage_id of a disk
//  *
//  * @return The item
//  */
// static disk_item_t* new_disk_item_t(gchar* storage_id) {
//     GError* error     = NULL;
//     disk_item_t* item = NULL;
//     guint subscription_id;

//     /* Subscribe to disks events. */
//     subscription_id = ax_storage_subscribe(storage_id, subscribe_cb, NULL, &error);
//     if (subscription_id == 0 || error != NULL) {
//         syslog(LOG_ERR,
//                "Failed to subscribe to events of %s. Error: %s",
//                storage_id,
//                error->message);
//         g_clear_error(&error);
//         return NULL;
//     }

//     item                  = g_new0(disk_item_t, 1);
//     item->subscription_id = subscription_id;
//     item->storage_id      = g_strdup(storage_id);
//     item->setup           = FALSE;

//     return item;
// }

// /**
//  * @brief Main function
//  *
//  * @return Result
//  */
// gint main(void) {
//     GList* disks    = NULL;
//     GList* node     = NULL;
//     GError* error   = NULL;
//     GMainLoop* loop = NULL;
//     gint ret        = EXIT_SUCCESS;

//     syslog(LOG_INFO, "Start AXStorage application");

//     disks = ax_storage_list(&error);
//     if (error != NULL) {
//         syslog(LOG_WARNING, "Failed to list storage devices. Error: (%s)", error->message);
//         g_error_free(error);
//         ret = EXIT_FAILURE;
//         /* Note: It is advised to get the list more than once, in case of failure.*/
//         goto out;
//     }

//     loop = g_main_loop_new(NULL, FALSE);
//     g_unix_signal_add(SIGTERM, signal_handler, loop);
//     g_unix_signal_add(SIGINT, signal_handler, loop);

//     /* Loop through the retrieved disks and subscribe to their events. */
//     for (node = g_list_first(disks); node != NULL; node = g_list_next(node)) {
//         gchar* disk_name  = (gchar*)node->data;
//         disk_item_t* item = new_disk_item_t(disk_name);
//         if (item == NULL) {
//             syslog(LOG_WARNING, "%s is skipped", disk_name);
//             g_free(node->data);
//             continue;
//         }
//         disks_list = g_list_append(disks_list, item);
//         g_free(node->data);
//     }
//     g_list_free(disks);

//     /* Write contents to two files. */
//     gchar* file1 = g_strdup("file1");
//     gchar* file2 = g_strdup("file2");
//     g_timeout_add_seconds(10, (GSourceFunc)write_data, file1);
//     g_timeout_add_seconds(10, (GSourceFunc)write_data, file2);

//     /* start the main loop */
//     g_main_loop_run(loop);

//     free_disk_item_t();
//     g_free(file1);
//     g_free(file2);
//     /* unref the main loop when the main loop has been quit */
//     g_main_loop_unref(loop);

// out:
//     syslog(LOG_INFO, "Finish AXStorage application");
//     return ret;
// }

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

/*** ---- CURL buffer (reprend ton hello.c) ---- ***/
struct MemoryStruct {
  char *memory;
  size_t size;
};
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(!ptr) { syslog(LOG_ERR, "Out of memory"); return 0; }
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
  return realsize;
}

/*** ---- AXStorage state (inspiré du sample Axis) ---- ***/
typedef struct {
  AXStorage* storage;
  gchar* storage_id;
  gchar* storage_path;
  guint subscription_id;
  gboolean setup;
  gboolean writable;
  gboolean available;
  gboolean full;
  gboolean exiting;
} disk_item_t;

static GList* disks_list = NULL;
static GMainLoop* loop = NULL;

/*** ---- Utils ---- ***/
static disk_item_t* find_disk_item(gchar* storage_id) {
  for (GList* n = g_list_first(disks_list); n != NULL; n = g_list_next(n)) {
    disk_item_t* item = n->data;
    if (g_strcmp0(storage_id, item->storage_id) == 0) return item;
  }
  return NULL;
}

static gboolean signal_handler(gpointer user_data) {
  (void)user_data;
  if (loop) g_main_loop_quit(loop);
  syslog(LOG_INFO, "Stopping (signal).");
  return G_SOURCE_REMOVE;
}

/*** ---- AXStorage: setup/release (mêmes patterns que le sample) ---- ***/
static void release_disk_cb(gpointer user_data, GError* error) {
  syslog(LOG_INFO, "Release of %s", (gchar*)user_data);
  if (error) { syslog(LOG_WARNING, "Release error: %s", error->message); g_error_free(error); }
}
static void setup_disk_cb(AXStorage* storage, gpointer user_data, GError* error) {
  (void)user_data;
  if (!storage || error) {
    syslog(LOG_ERR, "Failed setup disk: %s", error ? error->message : "null");
    if (error) g_error_free(error);
    return;
  }
  GError* axerr = NULL;
  gchar* storage_id = ax_storage_get_storage_id(storage, &axerr);
  if (axerr) { syslog(LOG_WARNING, "get_storage_id: %s", axerr->message); g_error_free(axerr); return; }
  gchar* path = ax_storage_get_path(storage, &axerr);
  if (axerr) { syslog(LOG_WARNING, "get_path: %s", axerr->message); g_error_free(axerr); g_free(storage_id); return; }

  disk_item_t* d = find_disk_item(storage_id);
  if (d) {
    d->storage = storage;
    d->storage_path = g_strdup(path);
    d->setup = TRUE;
    syslog(LOG_INFO, "Disk %s setup at %s", storage_id, path);
  }
  g_free(storage_id);
  g_free(path);
}

/*** ---- AXStorage: subscribe events → garde writable/available/full/exiting ---- ***/
static void subscribe_cb(gchar* storage_id, gpointer user_data, GError* error) {
  (void)user_data;
  if (error) { syslog(LOG_WARNING, "Subscribe %s error: %s", storage_id, error->message); g_error_free(error); return; }
  GError* axerr = NULL;
  disk_item_t* d = find_disk_item(storage_id);
  if (!d) return;

  gboolean exiting   = ax_storage_get_status(storage_id, AX_STORAGE_EXITING_EVENT, &axerr); if (axerr){syslog(LOG_WARNING,"EXITING err: %s",axerr->message); g_clear_error(&axerr); return;}
  gboolean available = ax_storage_get_status(storage_id, AX_STORAGE_AVAILABLE_EVENT, &axerr); if (axerr){syslog(LOG_WARNING,"AVAILABLE err: %s",axerr->message); g_clear_error(&axerr); return;}
  gboolean writable  = ax_storage_get_status(storage_id, AX_STORAGE_WRITABLE_EVENT, &axerr); if (axerr){syslog(LOG_WARNING,"WRITABLE err: %s",axerr->message); g_clear_error(&axerr); return;}
  gboolean full      = ax_storage_get_status(storage_id, AX_STORAGE_FULL_EVENT, &axerr); if (axerr){syslog(LOG_WARNING,"FULL err: %s",axerr->message); g_clear_error(&axerr); return;}

  d->exiting = exiting; d->available = available; d->writable = writable; d->full = full;
  syslog(LOG_INFO, "Disk %s: %swritable %savailable %sfull %sexiting",
         storage_id, writable?"":"not ", available?"":"not ", full?"":"not ", exiting?"":"not ");

  if (exiting && d->setup) {
    ax_storage_release_async(d->storage, release_disk_cb, storage_id, &axerr);
    if (axerr) { syslog(LOG_WARNING,"release err: %s", axerr->message); g_clear_error(&axerr); }
    d->setup = FALSE;
  } else if (writable && !full && !exiting && !d->setup) {
    ax_storage_setup_async(storage_id, setup_disk_cb, NULL, &axerr);
    if (axerr) { syslog(LOG_WARNING,"setup err: %s", axerr->message); g_clear_error(&axerr); }
  }
}

/*** ---- Helper: écriture JSON sur tous les disques prêts ---- ***/
static void write_json_on_all_disks(const char* basename, const char* json) {
  for (GList* n = g_list_first(disks_list); n != NULL; n = g_list_next(n)) {
    disk_item_t* d = n->data;
    if (!d->available || !d->writable || d->full || !d->setup || !d->storage_path) continue;

    // dossier dédié
    gchar* outdir = g_strdup_printf("%s/aoa_counts", d->storage_path);
    g_mkdir_with_parents(outdir, 0775);

    // fichier horodaté
    GDateTime* now = g_date_time_new_now_utc();
    gchar* ts = g_date_time_format(now, "%Y%m%dT%H%M%SZ");
    gchar* path = g_strdup_printf("%s/%s_%s.json", outdir, basename, ts);

    FILE* f = g_fopen(path, "w");
    if (!f) {
      syslog(LOG_WARNING, "open %s failed", path);
    } else {
      g_fprintf(f, "%s\n", json);
      fclose(f);
      syslog(LOG_INFO, "Wrote %s", path);
    }
    g_free(path); g_free(ts); g_free(outdir);
    g_date_time_unref(now);
  }
}

/* --- Trouve le dernier .json dans <storage_path>/aoa_counts --- */
static gchar* find_latest_json_in_dir(const gchar* dirpath) {
  GDir* dir = g_dir_open(dirpath, 0, NULL);
  if (!dir) return NULL;

  gchar* latest_path = NULL;
  time_t latest_mtime = 0;

  const gchar* name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (!g_str_has_suffix(name, ".json")) continue;
    gchar* full = g_build_filename(dirpath, name, NULL);

    GStatBuf st;
    if (g_stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
      time_t mt = st.st_mtime;
      if (mt >= latest_mtime) {
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
static void read_and_print_file(const gchar* filepath) {
  g_print( "==========================================");
  // SYSLOG(LOG_INFO, "Reading %s", filepath);
  if (!filepath) { g_printerr("No file to read.\n"); return; }
  gchar* contents = NULL; gsize len = 0;
  if (g_file_get_contents(filepath, &contents, &len, NULL)) {
    g_print("---- %s ----\n%.*s\n", filepath, (int)len, contents);
  } else {
    g_printerr("Failed to read: %s\n", filepath);
  }
  g_free(contents);
}

/* --- Vérifie tous les disques connus et affiche le dernier JSON écrit --- */
static void verify_latest_json_on_all_disks(void) {

  for (GList* n = g_list_first(disks_list); n != NULL; n = g_list_next(n)) {
    disk_item_t* d = n->data;
    if (!d || !d->storage_path) continue;

    /* On peut lire même si pas writable; on préfère available && setup */
    if (!d->available || !d->setup) {
      g_printerr("Storage %s not ready for read (available=%d, setup=%d)\n",
                 d->storage_id ? d->storage_id : "(null)",
                 (int)d->available, (int)d->setup);
    }

    gchar* dir = g_strdup_printf("%s/aoa_counts", d->storage_path);
    gchar* latest = find_latest_json_in_dir(dir);
    if (latest) {
      read_and_print_file(latest);
    } else {
      g_printerr("No JSON file found in %s\n", dir);
    }
    g_free(latest);
    g_free(dir);
  }
}


/*** ---- Tick: fait le POST JSON-RPC AOA et écrit sur SD ---- ***/
typedef struct {
  char cam_url[256];
  char user[64];
  char pass[64];
  char scenario[64];   // UID ou index
  char api_version[8]; // "1.6"
  gint period_sec;     // cadence
} PollCfg;

static gboolean tick_poll_and_store(gpointer user_data) {
  PollCfg* cfg = (PollCfg*)user_data;

  CURL* curl = curl_easy_init();
  if (!curl) { syslog(LOG_ERR, "curl init failed"); return G_SOURCE_CONTINUE; }

  struct MemoryStruct chunk = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  // corps JSON
  char body[512];
  // si scenario est numérique -> sans guillemets, sinon guillemets
  gboolean numeric = TRUE;
  for (const char* p = cfg->scenario; *p; ++p) { if (*p < '0' || *p > '9') { numeric = FALSE; break; } }
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
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);

  // Auth (Digest/Basic auto)
  curl_easy_setopt(curl, CURLOPT_USERNAME, cfg->user);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg->pass);
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    syslog(LOG_WARNING, "curl error: %s", curl_easy_strerror(rc));
  } else {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    syslog(LOG_INFO, "HTTP %ld payload=%zu bytes", http_code, chunk.size);
    if (http_code == 200 && chunk.memory && chunk.size) {
      // écriture brute JSON (tu peux parser et reformater si tu veux)
      write_json_on_all_disks("counts", chunk.memory);
      verify_latest_json_on_all_disks();   // affiche immédiatement le dernier fichier
    }
  }

  curl_slist_free_all(headers);
  free(chunk.memory);
  curl_easy_cleanup(curl);

  // reprogrammer le prochain tick
  return G_SOURCE_CONTINUE;
}

/*** ---- main : init storage, s'abonner, timer pour poll ---- ***/
int main(void) {
  syslog(LOG_INFO, "Start AOA→SD app");

  // 1) Lister les disques et s'abonner (reprend le pattern du sample Axis)
  GError* error = NULL;
  GList* disks = ax_storage_list(&error);
  if (error) { syslog(LOG_ERR, "ax_storage_list error: %s", error->message); g_error_free(error); return EXIT_FAILURE; }

  for (GList* n = g_list_first(disks); n != NULL; n = g_list_next(n)) {
    gchar* storage_id = (gchar*)n->data;
    guint sub = ax_storage_subscribe(storage_id, subscribe_cb, NULL, &error);
    if (!sub || error) {
      syslog(LOG_WARNING, "subscribe %s failed: %s", storage_id, error ? error->message : "(null)");
      if (error) g_clear_error(&error);
      g_free(storage_id);
      continue;
    }
    disk_item_t* item = g_new0(disk_item_t, 1);
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
  static PollCfg cfg;
  snprintf(cfg.cam_url, sizeof cfg.cam_url, "http://192.168.1.51/local/objectanalytics/control.cgi");
  snprintf(cfg.user, sizeof cfg.user, "champlein");     // <-- remplace
  snprintf(cfg.pass, sizeof cfg.pass, "696969");        // <-- remplace
  snprintf(cfg.scenario, sizeof cfg.scenario, "2");     // ou "S1" (UID)
  snprintf(cfg.api_version, sizeof cfg.api_version, "1.6");
  cfg.period_sec = 60;

  // Tick initial + toutes les period_sec
  tick_poll_and_store(&cfg);
  g_timeout_add_seconds(cfg.period_sec, tick_poll_and_store, &cfg);

  // run
  g_main_loop_run(loop);

  // cleanup minimal (désabos / release si besoin)
  for (GList* n = g_list_first(disks_list); n != NULL; n = g_list_next(n)) {
    disk_item_t* d = n->data;
    if (d->setup && d->storage) {
      ax_storage_release_async(d->storage, release_disk_cb, d->storage_id, &error);
      if (error) { syslog(LOG_WARNING, "release err: %s", error->message); g_clear_error(&error); }
      d->setup = FALSE;
    }
    if (d->subscription_id) ax_storage_unsubscribe(d->subscription_id, &error);
    if (error) { syslog(LOG_WARNING, "unsubscribe err: %s", error->message); g_clear_error(&error); }
    g_free(d->storage_id);
    g_free(d->storage_path);
  }
  g_list_free(disks_list);
  g_main_loop_unref(loop);

  syslog(LOG_INFO, "Finish AOA→SD app");
  return EXIT_SUCCESS;
}
