#include <glib.h>

// Contourne le macro g_string_free qui peut appeler g_string_free_and_steal
#ifdef g_string_free
#undef g_string_free
#endif

// Déclare la vraie fonction (symbole libglib), après undef macro
char *g_string_free(GString *string, gboolean free_segment);

// Fournit le symbole manquant pour les runtimes GLib anciens
char *g_string_free_and_steal(GString *str)
{
    // FALSE => on "vole" le buffer interne et on libère l'objet GString
    return g_string_free(str, FALSE);
}
