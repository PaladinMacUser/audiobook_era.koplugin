/* spike_clock.c -- M0 experiment for the audioout daemon (dev/audioout/PLAN.md).
 *
 * Question: can we get a real playback clock out of mtkbtmwrpcaudiosink?
 *
 * The program dlopens the DEVICE's libgstreamer-1.0 (no link-time
 * dependency, same pattern as kindle/gst-play.c), builds the production
 * MTK pipeline against a raw PCM file, attaches a buffer probe to the
 * sink's sink pad, and prints a measurement line every --interval ms:
 *
 *   CLK t=<ms since PLAYING> bufs=<n> bytes=<n> pos=<ms> dpos=<ms/tick>
 *       dt=<actual tick ms> posq_pipe=<ns|-1> posq_sink=<ns|-1>
 *       lat=<live|-1>/<min ns>/<max ns>
 *
 * pos is the probe-derived clock: accepted_output_bytes / (2ch*2B*48000).
 * The run ends at EOS, ERROR or --max-ms, then prints a SUMMARY line:
 *
 *   SUMMARY input sink expected_ms wall_ms probe_bytes probe_pos_ms
 *           first_audio_ms clock_ratio=<pos/wall> delivery_ratio=<wall/expected>
 *           pause_frozen=<0|1>
 *
 * Controls: --sink fakesink blasts the file as fast as acceptance allows
 * (clock_ratio >> 1 proves the probe counts acceptance, not wall time);
 * --sink alsasink validates the mechanism on a stock sink; the real run is
 * --sink mtkbtmwrpcaudiosink. --pause-at MS pauses the pipeline for 2 s to
 * verify the clock freezes with it.
 *
 * Build: dev/audioout/cross-build-spike.nix (armv7l glibc dynamic, -ldl
 * only). Run on device through the bundled loader:
 *   <plugin>/espeak-ng/lib/ld-linux-armhf.so.3 \
 *     --library-path <plugin>/espeak-ng/lib /tmp/spike_clock ...
 */

#include <gst/gst.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    void *h;                    /* dlopen handle */
    void (*gst_init)(int *, char ***);
    void (*gst_version)(guint *, guint *, guint *, guint *);
    const gchar *(*gst_version_string)(void);
    GstElement *(*gst_parse_launch)(const gchar *, GError **);
    GstStateChangeReturn (*gst_element_set_state)(GstElement *, GstState);
    GstBus *(*gst_element_get_bus)(GstElement *);
    GstMessage *(*gst_bus_poll)(GstBus *, GstMessageType, GstClockTime);
    void (*gst_message_parse_error)(GstMessage *, GError **, gchar **);
    void (*gst_object_unref)(gpointer);
    GstElement *(*gst_bin_get_by_name)(GstBin *, const gchar *);
    GstPad *(*gst_element_get_static_pad)(GstElement *, const gchar *);
    gulong (*gst_pad_add_probe)(GstPad *, GstPadProbeType, GstPadProbeCallback,
                                gpointer, GDestroyNotify);
    gsize (*gst_buffer_get_size)(GstBuffer *);
    gboolean (*gst_element_query_position)(GstElement *, GstFormat, gint64 *);
    gboolean (*gst_element_query_latency)(GstElement *, gboolean *,
                                          GstClockTime *, GstClockTime *);
} Gst;

static Gst G;

/* Local instead of gst_state_get_name: the device's libgstreamer does not
 * export it (dlopen probe, 2026-10-14). */
static const char *
state_name(GstStateChangeReturn r) {
    switch (r) {
    case GST_STATE_CHANGE_FAILURE: return "failure";
    case GST_STATE_CHANGE_SUCCESS: return "success";
    case GST_STATE_CHANGE_ASYNC: return "async";
    case GST_STATE_CHANGE_NO_PREROLL: return "no-preroll";
    default: return "?";
    }
}

static long long
mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long
bytes_to_pos_ms(unsigned long long bytes) {
    /* Output caps are fixed S16LE stereo 48 kHz: 4 bytes per frame. */
    return (long long)((bytes * 1000ull) / (2ull * 2ull * 48000ull));
}

typedef struct {
    unsigned long long bufs;
    unsigned long long bytes;
} Counter;

static GstPadProbeReturn
count_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer user) {
    (void)pad;
    Counter *c = (Counter *)user;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf) {
        c->bufs++;
        c->bytes += (unsigned long long)G.gst_buffer_get_size(buf);
    }
    return GST_PAD_PROBE_OK;
}

static void *
bind(void *h, const char *name) {
    void *p = dlsym(h, name);
    if (!p) {
        fprintf(stderr, "SPIKE dlsym failed: %s (%s)\n", name, dlerror());
        exit(2);
    }
    return p;
}

/* Optional symbols: the device's libgstreamer is a stripped build and does
 * not export everything the headers declare (gst_state_get_name,
 * gst_element_query_latency both missing on fw 4.45). A missing optional
 * symbol only disables the corresponding measurement column. */
static void *
bind_soft(void *h, const char *name) {
    void *p = dlsym(h, name);
    if (!p) fprintf(stderr, "SPIKE note: %s not exported by device libgstreamer\n", name);
    return p;
}

static void
load_gstreamer(void) {
    /* Preload the dependency chain first, so a failure names the exact
     * library: a bare dlopen of libgstreamer reports "No such file" for a
     * missing DEPENDENCY without saying which one. */
    const char *deps[] = {
        "libglib-2.0.so.0",
        "libgobject-2.0.so.0",
        "libffi.so.6",
        "libffi.so.8",
        "libgstreamer-1.0.so.0",
        "/lib/libgstreamer-1.0.so.0",
        "/usr/lib/libgstreamer-1.0.so.0",
        NULL,
    };
    for (int i = 0; deps[i]; i++) {
        void *h = dlopen(deps[i], RTLD_NOW | RTLD_GLOBAL);
        fprintf(stderr, "SPIKE dlopen %s -> %s%s%s\n", deps[i],
                h ? "ok" : "FAIL (",
                h ? "" : dlerror(),
                h ? "" : ")");
        if (h && !G.h && strstr(deps[i], "libgstreamer")) G.h = h;
    }
    if (!G.h) {
        fprintf(stderr, "SPIKE FATAL: no libgstreamer-1.0\n");
        exit(2);
    }
    G.gst_init = bind(G.h, "gst_init");
    G.gst_version = bind(G.h, "gst_version");
    G.gst_version_string = bind(G.h, "gst_version_string");
    G.gst_parse_launch = bind(G.h, "gst_parse_launch");
    G.gst_element_set_state = bind(G.h, "gst_element_set_state");
    G.gst_element_get_bus = bind(G.h, "gst_element_get_bus");
    G.gst_bus_poll = bind(G.h, "gst_bus_poll");
    G.gst_message_parse_error = bind(G.h, "gst_message_parse_error");
    G.gst_object_unref = bind(G.h, "gst_object_unref");
    G.gst_bin_get_by_name = bind(G.h, "gst_bin_get_by_name");
    G.gst_element_get_static_pad = bind(G.h, "gst_element_get_static_pad");
    G.gst_pad_add_probe = bind(G.h, "gst_pad_add_probe");
    G.gst_buffer_get_size = bind(G.h, "gst_buffer_get_size");
    G.gst_element_query_position = bind_soft(G.h, "gst_element_query_position");
    G.gst_element_query_latency = bind_soft(G.h, "gst_element_query_latency");
}

int
main(int argc, char **argv) {
    const char *pcm_path = NULL, *sink_name = "mtkbtmwrpcaudiosink";
    int rate = 22050, channels = 1, interval_ms = 250, pause_at = 0;
    long long max_ms = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pcm") && i + 1 < argc) pcm_path = argv[++i];
        else if (!strcmp(argv[i], "--sink") && i + 1 < argc) sink_name = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pause-at") && i + 1 < argc) pause_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-ms") && i + 1 < argc) max_ms = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: spike_clock --pcm <raw s16le file> [--sink NAME]"
                   " [--rate HZ] [--channels N] [--interval MS] [--pause-at MS]"
                   " [--max-ms MS]\n");
            return 0;
        }
    }
    if (!pcm_path) {
        fprintf(stderr, "SPIKE FATAL: --pcm is required\n");
        return 2;
    }

    load_gstreamer();

    guint vmaj, vmin, vmic, vnano;
    G.gst_version(&vmaj, &vmin, &vmic, &vnano);
    fprintf(stderr, "SPIKE device gstreamer: %s (%u.%u.%u)\n",
            G.gst_version_string(), vmaj, vmin, vmic);

    G.gst_init(NULL, NULL);

    gchar launch[1024];
    snprintf(launch, sizeof launch,
        "filesrc location=\"%s\""
        " ! rawaudioparse use-sink-caps=false format=pcm pcm-format=s16le"
        " sample-rate=%d num-channels=%d"
        " ! audioconvert ! audioresample"
        " ! audio/x-raw,format=S16LE,rate=48000,channels=2"
        " ! queue max-size-time=500000000 max-size-bytes=131072"
        " ! %s name=sink sync=false",
        pcm_path, rate, channels, sink_name);
    fprintf(stderr, "SPIKE pipeline: %s\n", launch);

    GError *err = NULL;
    GstElement *pipe = G.gst_parse_launch(launch, &err);
    if (!pipe) {
        fprintf(stderr, "SPIKE FATAL: parse_launch failed: %s\n",
                err && err->message ? err->message : "(no message)");
        return 2;
    }

    GstElement *sink = G.gst_bin_get_by_name(GST_BIN(pipe), "sink");
    if (!sink) {
        fprintf(stderr, "SPIKE FATAL: sink element not found\n");
        return 2;
    }
    GstPad *sinkpad = G.gst_element_get_static_pad(sink, "sink");
    if (!sinkpad) {
        fprintf(stderr, "SPIKE FATAL: sink has no static sink pad\n");
        return 2;
    }
    Counter counter = {0, 0};
    G.gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer,
                        &counter, NULL);

    GstBus *bus = G.gst_element_get_bus(pipe);
    GstStateChangeReturn scr = G.gst_element_set_state(pipe, GST_STATE_PLAYING);
    fprintf(stderr, "SPIKE set_state(PLAYING) -> %s\n", state_name(scr));
    if (scr == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "SPIKE FATAL: pipeline refused PLAYING\n");
        return 2;
    }

    long long t0 = mono_ms();
    long long first_audio_ms = -1;
    long long last_pos_ms = 0, last_t = 0;
    int pause_done = 0, pause_frozen = 0;
    unsigned long long hold_first_bytes = 0, hold_last_bytes = 0;

    printf("# t bufs bytes pos dpos dt posq_pipe posq_sink lat_live lat_min lat_max\n");

    for (;;) {
        usleep(interval_ms * 1000);

        GstMessage *msg = G.gst_bus_poll(bus, GST_MESSAGE_EOS | GST_MESSAGE_ERROR, 0);
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError *e = NULL; gchar *dbg = NULL;
                G.gst_message_parse_error(msg, &e, &dbg);
                printf("ERROR %s\n", e && e->message ? e->message : "?");
                fprintf(stderr, "SPIKE pipeline ERROR: %s (dbg %s)\n",
                        e && e->message ? e->message : "?",
                        dbg ? dbg : "-");
            } else {
                printf("EOS\n");
                fprintf(stderr, "SPIKE EOS\n");
            }
            break;
        }

        long long now = mono_ms();
        long long t = now - t0;

        /* Query-based alternatives, reported but not relied on. */
        gint64 pq = -1, sq = -1;
        if (G.gst_element_query_position) {
            G.gst_element_query_position(pipe, GST_FORMAT_TIME, &pq);
            G.gst_element_query_position(sink, GST_FORMAT_TIME, &sq);
        }
        gboolean live = FALSE;
        GstClockTime lmin = 0, lmax = 0;
        gboolean lat_ok = FALSE;
        if (G.gst_element_query_latency)
            lat_ok = G.gst_element_query_latency(sink, &live, &lmin, &lmax);

        long long pos_ms = bytes_to_pos_ms(counter.bytes);
        if (first_audio_ms < 0 && counter.bufs > 0) first_audio_ms = t;

        long long dpos = pos_ms - last_pos_ms;
        long long dt = last_t ? (t - last_t) : interval_ms;

        /* The pause test: freeze near --pause-at of probe position, hold
         * 2 s, resume. While PAUSED nothing is accepted, so bytes must not
         * move across the hold. */
        if (pause_at && !pause_done && pos_ms >= pause_at) {
            G.gst_element_set_state(pipe, GST_STATE_PAUSED);
            fprintf(stderr, "SPIKE pause at pos=%lld t=%lld\n", pos_ms, t);
            hold_first_bytes = counter.bytes;
            long long until = mono_ms() + 2000;
            while (mono_ms() < until) {
                usleep(200000);
                hold_last_bytes = counter.bytes;
                printf("PAUSE t=%lld bufs=%llu bytes=%llu pos=%lld\n",
                       mono_ms() - t0, counter.bufs, counter.bytes,
                       bytes_to_pos_ms(counter.bytes));
            }
            pause_frozen = (hold_last_bytes == hold_first_bytes);
            G.gst_element_set_state(pipe, GST_STATE_PLAYING);
            fprintf(stderr, "SPIKE resume (frozen=%d)\n", pause_frozen);
            pause_done = 1;
            t0 += 2000;             /* keep t comparable to playback time */
            continue;
        }

        printf("CLK t=%lld bufs=%llu bytes=%llu pos=%lld dpos=%lld dt=%lld"
               " posq_pipe=%lld posq_sink=%lld lat_live=%d lat_min=%llu lat_max=%llu\n",
               t, counter.bufs, counter.bytes, pos_ms, dpos, dt,
               pq, sq,
               lat_ok ? (int)live : -1,
               lat_ok ? (unsigned long long)lmin : 0ull,
               lat_ok ? (unsigned long long)lmax : 0ull);
        last_pos_ms = pos_ms;
        last_t = t;

        if (max_ms && t > max_ms) {
            printf("TIMEOUT\n");
            fprintf(stderr, "SPIKE max-ms reached\n");
            break;
        }
    }

    long long wall = mono_ms() - t0;
    long long expected_ms = 0;
    {
        FILE *f = fopen(pcm_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            expected_ms = (long long)((sz * 1000ll) / (long long)(rate * channels * 2));
        }
    }
    long long final_pos = bytes_to_pos_ms(counter.bytes);
    printf("SUMMARY input=%s sink=%s expected_ms=%lld wall_ms=%lld probe_bytes=%llu"
           " probe_pos_ms=%lld first_audio_ms=%lld clock_ratio=%.3f delivery_ratio=%.3f"
           " pause_frozen=%d\n",
           pcm_path, sink_name, expected_ms, wall, counter.bytes, final_pos,
           first_audio_ms,
           wall > 0 ? (double)final_pos / (double)wall : 0.0,
           expected_ms > 0 ? (double)wall / (double)expected_ms : 0.0,
           pause_frozen);
    return 0;
}
