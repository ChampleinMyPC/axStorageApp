// Toujours commenter : copie sûre type strlcpy (garantit toujours le '\0')
#include <cstddef>
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
