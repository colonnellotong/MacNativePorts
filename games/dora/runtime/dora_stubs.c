/* The two host services this build leaves out.
 *
 * The shared runtime carries a hardware Opus decoder (opus_codec.c, libopus) and a VP9 movie
 * decoder (movie_host.c, libvpx). Both exist for Tears of the Kingdom: its voice streams are
 * hwopus packets and its cutscenes are VP9. This game has neither — its audio is CRIWARE ADX2
 * decoded inside the guest, and its video is Sofdec2 in .usm containers — so those two files are
 * not compiled here and the libraries stay out of the link.
 *
 * The first attempt therefore shows up as a missing-symbol abort naming the call, instead of a
 * decoder that answers wrongly.
 */
#include <stddef.h>

void *runtime_codec_override(const char *name) {
    (void)name;
    return NULL;
}

void *runtime_movie_override(const char *name) {
    (void)name;
    return NULL;
}

/* Called once per present by the shared graphics layer. With no host video decoder there is never a
   frame waiting to be handed over, so there is nothing to do. */
void movie_host_tick(void) {}
