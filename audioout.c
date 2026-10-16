/* audioout.c -- owned audio-output daemon for audiobook.koplugin (Tier 2).
 *
 * One long-lived process drives the device's GStreamer pipeline (Kobo MTK
 * vendor sink for now), accepts clip commands over an AF_UNIX socket, and
 * reports a REAL rendered-position clock derived from a sink-pad probe.
 * Replaces the duplicated shell-wrapper FIFO pipelines in ttsengine.lua and
 * mediaengine.lua. Design and on-device measurements:
 * dev/audioout/PLAN.md (M0, 2026-10-14).
 *
 * Pipeline (topology unchanged from the proven production one, fed by us):
 *   filesrc location=FIFO ! rawaudioparse ... ! audioconvert ! audioresample
 *     ! audio/x-raw,format=S16LE,rate=48000,channels=2
 *     ! queue max-size-time=500000000 max-size-bytes=131072
 *     ! [volume name=vol] ! mtkbtmwrpcaudiosink name=sink sync=false
 *
 * Units: the FIFO carries INPUT-rate PCM (the clip's rate); the sink gets
 * 48 kHz stereo. The clock is tracked in MILLISECONDS on both sides, never
 * raw bytes across the boundary:
 *   feed side    ms = input_bytes * 1000 / (rate * channels * 2)
 *   clock side   ms = output_bytes / 192        (48000 * 4 B/frame)
 *
 * Clock model (M0-verified on the vendor sink): buffers reach the sink
 * ~1.5 s at a time, one per render period, real-time paced. When buffer i
 * (spanning output bytes S_i..S_i+b_i) is accepted at wall time W_i, the
 * sink begins rendering S_i at W_i, so
 *     rendered(T) = boundary_ms + (T - arrival_ms)      capped at accepted
 * gst_element_query_position is available on this sink and logged as a
 * cross-check only.
 *
 * Protocol: one JSON object per line in, one JSON line per response.
 * Events are not pushed in M1; the client polls "status" and compares
 * done_count. Commands:
 *   {"cmd":"play","path":"...","gap_ms":300,"rate":22050,"channels":1}
 *       Queue a WAV clip (append). A rate/channels change vs the running
 *       pipeline rebuilds it internally and drops any queued clips.
 *   {"cmd":"pause"}    stop feeding clips, feed silence (in-flight buffered
 *                      audio still plays out; same semantics as the old
 *                      feeder pause)
 *   {"cmd":"resume"}   resume feeding
 *   {"cmd":"stop"}     drop the queue, keep the A2DP link with silence
 *   {"cmd":"flush"}    drop the queue AND restart the pipeline (instant
 *                      silence, drops buffered audio too)
 *   {"cmd":"status"}   state/pos_ms/accepted_ms/fed_ms/done_count/queue/...
 *   {"cmd":"volume","value_x1000":800}
 *   {"cmd":"ping"} / {"cmd":"shutdown"}
 * Queue: 8 clips. When it runs dry the daemon feeds silence to hold the
 * A2DP link (replaces the 120 s keepalive WAV).
 *
 * Build: dev/audioout/build-daemon.sh (Jessie 2.19 sysroot, bare run under
 * the device loader, dlopens the device's libgstreamer-1.0).
 */

#include <gst/gst.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_PATH_DEFAULT "/tmp/audiobook_audioout.sock"
#define FIFO_PATH_DEFAULT "/tmp/audiobook_audioout_fifo"
#define MAX_QUEUE 8
#define MAX_PATH 1024
#define LINE_MAX_LEN 2048
/* Bounded writes keep the poll loop responsive to commands. */
#define FEED_CHUNK (32 * 1024)
#define SILENCE_CHUNK (8 * 1024)
#define F_SETPIPE_SZ 1031
/* Output caps: S16LE stereo 48 kHz. */
#define OUT_BYTES_PER_MS 192

/* ------------------------------------------------------------------ */
/* GStreamer via dlopen, soft-bound (the device build strips optional   */
/* symbols; M0 listed exactly which survive).                           */
/* ------------------------------------------------------------------ */

typedef struct {
    void *gst;
    void *gobj;                 /* libgobject, optional (live volume) */
    void (*gst_init)(int *, char ***);
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
    gboolean (*gst_element_factory_find)(const gchar *);
    void (*g_object_set)(gpointer, const gchar *, ...);
} GstApi;

static GstApi G;

/* ------------------------------------------------------------------ */
/* Clock                                                                */
/* ------------------------------------------------------------------ */

static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long probe_bytes;      /* total accepted output bytes */
static unsigned long long probe_last_buf;   /* size of the newest buffer */

/* Main-thread copies refreshed each tick. */
static unsigned long long acc_bytes;
static long long acc_boundary_ms;           /* stream ms where the newest
                                               buffer STARTS rendering */
static long long acc_arrival_ms;            /* wall ms of that arrival */
static long long wall_now;
static int probe_seen;

static long long
mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static GstPadProbeReturn
probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user) {
    (void)pad;
    (void)user;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    gsize sz = G.gst_buffer_get_size(buf);
    pthread_mutex_lock(&probe_lock);
    probe_last_buf = (unsigned long long)sz;
    probe_bytes += (unsigned long long)sz;
    pthread_mutex_unlock(&probe_lock);
    return GST_PAD_PROBE_OK;
}

static void
refresh_clock(void) {
    unsigned long long b, last;
    pthread_mutex_lock(&probe_lock);
    b = probe_bytes;
    last = probe_last_buf;
    pthread_mutex_unlock(&probe_lock);
    if (b != acc_bytes || (b > 0 && !probe_seen)) {
        probe_seen = 1;
        acc_bytes = b;
        acc_boundary_ms = (long long)((b > last ? b - last : 0) / OUT_BYTES_PER_MS);
        acc_arrival_ms = wall_now;
    }
}

/* Rendered stream position in ms (M0 model), capped at accepted. */
static long long
rendered_ms(void) {
    if (!probe_seen || acc_bytes == 0) return 0;
    long long est = acc_boundary_ms + (wall_now - acc_arrival_ms);
    long long acc = (long long)(acc_bytes / OUT_BYTES_PER_MS);
    if (est < 0) est = 0;
    if (est > acc) est = acc;
    return est;
}

/* ------------------------------------------------------------------ */
/* Daemon state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char path[MAX_PATH];
    int fd;
    unsigned long long data_offset;
    unsigned long long data_size;   /* PCM bytes */
    unsigned long long file_pos;    /* next byte to feed */
    int in_bps;                     /* input bytes per second */
    long long duration_ms;
    long long gap_ms_after;
    long long start_ms;             /* stream position of first PCM */
    long long end_ms;               /* start + duration + gap */
} Clip;

static struct {
    GstElement *pipe;
    GstElement *sink;
    GstBus *bus;
    int pipe_rate, pipe_channels;
    double volume;
    int pipeline_retries;
    long long pipeline_last_fail_ms;

    char fifo_path[MAX_PATH];
    int fifo_fd;
    unsigned long long fifo_written;    /* input bytes handed to the fifo */

    Clip queue[MAX_QUEUE];
    int queue_len;
    int feed_idx;                       /* clip being fed, -1 = none active */
    int all_fed;                        /* every queued clip written out */
    long long gap_left_ms;              /* silence owed before the next clip */
    int paused;

    long long fed_end_ms;               /* end of everything queued so far */
    unsigned long long done_count;
    int announce_idle;

    int listen_fd;
    int client_fd;
} S;

static const char *g_socket_path = SOCKET_PATH_DEFAULT;
static const char *g_sink_name = "mtkbtmwrpcaudiosink";
static int g_rate = 22050, g_channels = 1;
static long long g_started_ms;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void
log_line(const char *fmt, ...) {
    va_list ap;
    long long t = mono_ms() - g_started_ms;
    fprintf(stderr, "[%lld.%03lld] ", t / 1000, t % 1000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* Flat-object JSON field extraction, snt_server style: enough for
 * machine-written command lines, no escaping (paths have no quotes). */
static int
json_find(const char *json, const char *key, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t n = 0;
        /* Unescape the standard JSON escapes: KOReader's encoder writes
         * "/" as "\/", and an unescaped path fails to open. */
        while (*p && *p != '"') {
            char c = *p++;
            if (c == '\\' && *p) {
                char e = *p++;
                if (e == '/' || e == '"' || e == '\\') c = e;
                else if (e == 'n') c = '\n';
                else if (e == 't') c = '\t';
                else if (e == 'r') c = '\r';
                else c = e;
            }
            if (n + 1 < outsz) out[n++] = c;
        }
        out[n] = 0;
        return 0;
    }
    size_t n = 0;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' '
           && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n + 1 < outsz) out[n++] = *p++;
        else p++;
    }
    out[n] = 0;
    return 1;
}

static long long
json_num(const char *json, const char *key, long long dflt) {
    char v[64];
    if (json_find(json, key, v, sizeof v) < 0) return dflt;
    return strtoll(v, NULL, 10);
}

/* ------------------------------------------------------------------ */
/* WAV parsing (chunk walk; TTS WAVs are plain s16 PCM)                 */
/* ------------------------------------------------------------------ */

static int
clip_open(Clip *c, const char *path, int rate, int channels) {
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s", path);
    c->fd = open(path, O_RDONLY);
    if (c->fd < 0) {
        log_line("clip open failed: %s (%s)", path, strerror(errno));
        return -1;
    }
    unsigned char hdr[12];
    if (pread(c->fd, hdr, 12, 0) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        log_line("clip not RIFF/WAVE: %s", path);
        goto fail;
    }
    unsigned long long off = 12;
    int fmt_bits = 0;
    for (;;) {
        unsigned char ch[8];
        if (pread(c->fd, ch, 8, off) != 8) break;
        unsigned long long sz = ch[4] | (ch[5] << 8) |
                                ((unsigned long long)ch[6] << 16) |
                                ((unsigned long long)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4) && sz >= 16) {
            unsigned char fmt[16];
            if (pread(c->fd, fmt, 16, off + 8) == 16)
                fmt_bits = fmt[14] | (fmt[15] << 8);
        } else if (!memcmp(ch, "data", 4)) {
            c->data_offset = off + 8;
            c->data_size = sz;
            c->file_pos = off + 8;
            break;
        }
        off += 8 + sz + (sz & 1);
    }
    if (c->data_size == 0 || (fmt_bits != 0 && fmt_bits != 16)) {
        log_line("clip unusable (data=%llu bits=%d): %s",
                 c->data_size, fmt_bits, path);
        goto fail;
    }
    c->in_bps = rate * channels * 2;
    if (c->in_bps <= 0) c->in_bps = 44100;
    c->duration_ms = (long long)((c->data_size * 1000ull) /
                                 (unsigned long long)c->in_bps);
    return 0;
fail:
    close(c->fd);
    c->fd = -1;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Pipeline lifecycle                                                   */
/* ------------------------------------------------------------------ */

static int
pipeline_build(int rate, int channels) {
    char launch[1600];
    char vol[64] = "";
    if (G.gst_element_factory_find && G.gst_element_factory_find("volume"))
        snprintf(vol, sizeof vol, " ! volume name=vol volume=%.3f", S.volume);
    snprintf(launch, sizeof launch,
        "filesrc location=\"%s\""
        " ! rawaudioparse use-sink-caps=false format=pcm pcm-format=s16le"
        " sample-rate=%d num-channels=%d"
        " ! audioconvert ! audioresample"
        " ! audio/x-raw,format=S16LE,rate=48000,channels=2"
        " ! queue max-size-time=500000000 max-size-bytes=131072"
        "%s"
        " ! %s name=sink sync=false",
        S.fifo_path, rate, channels, vol, g_sink_name);
    log_line("pipeline: %s", launch);

    GError *err = NULL;
    GstElement *pipe = G.gst_parse_launch(launch, &err);
    if (!pipe) {
        log_line("parse_launch failed: %s",
                 err && err->message ? err->message : "?");
        return -1;
    }
    S.pipe = pipe;
    S.sink = G.gst_bin_get_by_name(GST_BIN(pipe), "sink");
    if (S.sink) {
        GstPad *pad = G.gst_element_get_static_pad(S.sink, "sink");
        if (pad)
            G.gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, probe_cb,
                                NULL, NULL);
        else
            log_line("WARNING: sink has no static pad; no clock");
        S.bus = G.gst_element_get_bus(pipe);
    }
    if (G.gst_element_set_state(pipe, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        /* Pull the bus error so failures are diagnosable off-device. */
        char errbuf[256] = "";
        if (S.bus) {
            GstMessage *msg = G.gst_bus_poll(S.bus, GST_MESSAGE_ERROR, 500);
            if (msg) {
                GError *e = NULL;
                gchar *dbg = NULL;
                G.gst_message_parse_error(msg, &e, &dbg);
                snprintf(errbuf, sizeof errbuf, "%s",
                         e && e->message ? e->message : "?");
                log_line("bus error: %s (%s)", errbuf, dbg ? dbg : "-");
            }
        }
        log_line("pipeline refused PLAYING%s%s",
                 errbuf[0] ? ": " : " (no bus error)", errbuf);
        return -1;
    }
    S.pipe_rate = rate;
    S.pipe_channels = channels;
    return 0;
}

static void
pipeline_teardown(void) {
    if (S.pipe) {
        G.gst_element_set_state(S.pipe, GST_STATE_NULL);
        if (S.bus) G.gst_object_unref(S.bus);
        G.gst_object_unref(S.pipe);
        S.pipe = NULL;
        S.sink = NULL;
        S.bus = NULL;
    }
    if (S.fifo_fd >= 0) {
        close(S.fifo_fd);
        S.fifo_fd = -1;
    }
}

static void
reset_stream(void) {
    /* Call right after a pipeline rebuild: the old backlog died with it. */
    S.fifo_written = 0;
    S.fed_end_ms = 0;
    S.queue_len = 0;
    S.feed_idx = -1;
    S.all_fed = 1;
    S.gap_left_ms = 0;
    acc_bytes = 0;
    acc_boundary_ms = 0;
    acc_arrival_ms = 0;
    probe_seen = 0;
}

static int
fifo_open(void) {
    if (S.fifo_fd >= 0) return 0;
    if (mkfifo(S.fifo_path, 0600) != 0 && errno != EEXIST) {
        log_line("mkfifo %s failed: %s", S.fifo_path, strerror(errno));
        return -1;
    }
    /* O_RDWR: we hold both ends, so filesrc never sees EOF across pipeline
     * rebuilds. 16 KB pipe like the old feeder, to bound stale audio. */
    int fd = open(S.fifo_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        log_line("fifo open failed: %s (%s)", S.fifo_path, strerror(errno));
        return -1;
    }
    log_line("fifo open ok on fd %d", fd);
    fcntl(fd, F_SETPIPE_SZ, 16384);
    S.fifo_fd = fd;
    return 0;
}

/* Wait for the exclusive MTK abstract socket to be free, killing legacy
 * gst-launch orphans: the old startup dance, done once at daemon start. */
static void
claim_exclusive_socket(void) {
    for (int attempt = 0; attempt < 5; attempt++) {
        FILE *f = fopen("/proc/net/unix", "r");
        int held = 0;
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "@kobo:mtkbtmwrpc")) {
                    held = 1;
                    break;
                }
            }
            fclose(f);
        }
        if (!held) return;
        log_line("MTK socket held; killing legacy gst-launch (attempt %d)",
                 attempt + 1);
        system("killall -9 gst-launch-1.0 2>/dev/null");
        usleep(200000);
    }
    log_line("WARNING: MTK socket still held after retries");
}

/* ------------------------------------------------------------------ */
/* Queue + feeding                                                      */
/* ------------------------------------------------------------------ */

static const unsigned char silence_chunk[SILENCE_CHUNK];

static int
queue_clip(const char *path, long long gap_ms) {
    Clip c;
    if (clip_open(&c, path, g_rate, g_channels) != 0) return -1;
    c.gap_ms_after = gap_ms;
    /* A clip queued while keepalive silence flows starts after every byte
     * already written. */
    long long now_written_ms =
        (long long)((S.fifo_written * 1000ull) /
                    (unsigned long long)(g_rate * g_channels * 2));
    c.start_ms = (now_written_ms > S.fed_end_ms) ? now_written_ms : S.fed_end_ms;
    c.end_ms = c.start_ms + c.duration_ms + gap_ms;
    S.fed_end_ms = c.end_ms;
    S.all_fed = 0;
    S.announce_idle = 0;
    S.queue[S.queue_len++] = c;
    log_line("queued %s dur=%lldms gap=%lldms span=[%lld..%lld]ms",
             path, c.duration_ms, gap_ms, c.start_ms, c.end_ms);
    return 0;
}

static unsigned long long
fifo_write(const unsigned char *buf, unsigned long long len) {
    unsigned long long total = 0;
    while (total < len) {
        ssize_t n = write(S.fifo_fd, buf + total, len - total);
        if (n > 0) {
            total += (unsigned long long)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        log_line("fifo write error: %s", strerror(errno));
        break;
    }
    S.fifo_written += total;
    return total;
}

/* One feed tick: active clip, then inter-clip gap, then keepalive. */
static void
feed_tick(void) {
    if (S.paused || S.fifo_fd < 0) return;
    if (S.feed_idx < 0 && S.queue_len == 0) {
        fifo_write(silence_chunk, SILENCE_CHUNK);   /* idle keepalive */
        return;
    }
    if (S.all_fed) {
        /* Queue fully written (possibly still rendering): keepalive. */
        fifo_write(silence_chunk, SILENCE_CHUNK);
        return;
    }
    if (S.feed_idx < 0) S.feed_idx = 0;

    unsigned long long budget = FEED_CHUNK;
    unsigned char buf[FEED_CHUNK];
    while (budget > 0) {
        if (S.gap_left_ms > 0) {
            /* Gap silence at the INPUT byte rate of the pipeline. */
            long long gap_bytes = (long long)((double)S.gap_left_ms *
                                              (g_rate * g_channels * 2) / 1000.0);
            unsigned long long want = (unsigned long long)gap_bytes;
            if (want > budget) want = budget;
            if (want > SILENCE_CHUNK) want = SILENCE_CHUNK;
            unsigned long long w = fifo_write(silence_chunk, want);
            double wrote_ms = (double)w * 1000.0 / (g_rate * g_channels * 2);
            S.gap_left_ms -= (long long)(wrote_ms + 0.5);
            if (S.gap_left_ms < 0) S.gap_left_ms = 0;
            budget -= w;
            if (w < want) return;   /* fifo full */
            continue;
        }
        if (S.feed_idx >= S.queue_len) {
            S.all_fed = 1;
            return;
        }
        Clip *c = &S.queue[S.feed_idx];
        unsigned long long remaining =
            (c->data_offset + c->data_size) - c->file_pos;
        if (remaining == 0) {
            if (c->fd >= 0) {
                close(c->fd);
                c->fd = -1;
            }
            S.gap_left_ms = c->gap_ms_after;
            S.feed_idx++;
            continue;
        }
        unsigned long long want = remaining < budget ? remaining : budget;
        ssize_t n = pread(c->fd, buf, want, c->file_pos);
        if (n <= 0) {
            log_line("clip read failed at %llu: %s", c->file_pos, c->path);
            if (c->fd >= 0) {
                close(c->fd);
                c->fd = -1;
            }
            S.gap_left_ms = c->gap_ms_after;
            S.feed_idx++;
            continue;
        }
        unsigned long long w = fifo_write(buf, (unsigned long long)n);
        c->file_pos += w;
        budget -= w;
        if (w < (unsigned long long)n) return;   /* fifo full, next tick */
    }
}

/* ------------------------------------------------------------------ */
/* Socket                                                               */
/* ------------------------------------------------------------------ */

static void
send_line(const char *line) {
    if (S.client_fd < 0) return;
    size_t len = strlen(line);
    send(S.client_fd, line, len, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void
respond(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t l = strlen(buf);
    if (l < sizeof buf - 2) {
        buf[l] = '\n';
        buf[l + 1] = 0;
    }
    send_line(buf);
}

static const char *
state_str(void) {
    if (S.paused) return "paused";
    if (S.feed_idx >= 0 && !S.all_fed) return "playing";
    if (S.queue_len > 0) return "buffered";
    return "idle";
}

static void
drop_queue(void) {
    for (int i = 0; i < S.queue_len; i++)
        if (S.queue[i].fd >= 0) close(S.queue[i].fd);
    S.queue_len = 0;
    S.feed_idx = -1;
    S.all_fed = 1;
    S.gap_left_ms = 0;
}

static void
handle_command(char *line) {
    char cmd[32] = "";
    json_find(line, "cmd", cmd, sizeof cmd);

    if (!strcmp(cmd, "ping")) {
        respond("{\"ok\":true,\"uptime_ms\":%lld,\"bufs\":%llu}",
                mono_ms() - g_started_ms, acc_bytes / OUT_BYTES_PER_MS);
    } else if (!strcmp(cmd, "play")) {
        char path[MAX_PATH] = "";
        json_find(line, "path", path, sizeof path);
        if (!path[0]) {
            respond("{\"ok\":false,\"error\":\"missing path\"}");
            return;
        }
        if (S.queue_len >= MAX_QUEUE) {
            respond("{\"ok\":false,\"error\":\"queue full\"}");
            return;
        }
        long long rate = json_num(line, "rate", g_rate);
        long long channels = json_num(line, "channels", g_channels);
        /* Lazy pipeline build: the vendor sink can stall tens of seconds
         * at init when no A2DP peer is connected (M0/M1 measured ~26 s),
         * and a stalled build at daemon start would hang the client. */
        if (!S.pipe) {
            if (fifo_open() != 0 || pipeline_build((int)rate, (int)channels) != 0) {
                respond("{\"ok\":false,\"error\":\"pipeline build failed\"}");
                return;
            }
        }
        if ((int)rate != S.pipe_rate || (int)channels != S.pipe_channels) {
            log_line("rate change %dx%d -> %ldx%ld; rebuilding (queue dropped)",
                     S.pipe_rate, S.pipe_channels, rate, channels);
            drop_queue();
            pipeline_teardown();
            g_rate = (int)rate;
            g_channels = (int)channels;
            reset_stream();
            if (fifo_open() != 0 || pipeline_build(g_rate, g_channels) != 0) {
                respond("{\"ok\":false,\"error\":\"pipeline rebuild failed\"}");
                return;
            }
        }
        if (fifo_open() != 0) {
            respond("{\"ok\":false,\"error\":\"fifo open failed\"}");
            return;
        }
        if (queue_clip(path, json_num(line, "gap_ms", 0)) != 0) {
            respond("{\"ok\":false,\"error\":\"clip open failed\"}");
            return;
        }
        respond("{\"ok\":true,\"queue\":%d,\"fed_end_ms\":%lld,"
                "\"start_ms\":%lld,\"duration_ms\":%lld}",
                S.queue_len, S.fed_end_ms,
                S.queue[S.queue_len - 1].start_ms,
                S.queue[S.queue_len - 1].duration_ms);
    } else if (!strcmp(cmd, "pause")) {
        S.paused = 1;
        respond("{\"ok\":true,\"state\":\"paused\"}");
    } else if (!strcmp(cmd, "resume")) {
        S.paused = 0;
        respond("{\"ok\":true,\"state\":\"%s\"}", state_str());
    } else if (!strcmp(cmd, "stop")) {
        drop_queue();
        respond("{\"ok\":true,\"state\":\"idle\"}");
    } else if (!strcmp(cmd, "flush")) {
        drop_queue();
        if (S.pipe) {
            pipeline_teardown();
            reset_stream();
            if (fifo_open() != 0 || pipeline_build(g_rate, g_channels) != 0) {
                respond("{\"ok\":false,\"error\":\"flush rebuild failed\"}");
                return;
            }
        }
        respond("{\"ok\":true,\"state\":\"idle\"}");
    } else if (!strcmp(cmd, "release")) {
        drop_queue();
        if (S.pipe) {
            pipeline_teardown();
            reset_stream();
        }
        respond("{\"ok\":true,\"state\":\"released\"}");
    } else if (!strcmp(cmd, "status")) {
        /* Cap at the end of REAL audio: idle keepalive silence advances
         * the probe clock but is not part of any clip, and the client's
         * sync math must never see a position past the last word. */
        long long rend = rendered_ms();
        long long fed = (long long)((S.fifo_written * 1000ull) /
                                    (unsigned long long)(g_rate * g_channels * 2));
        if (rend > S.fed_end_ms) rend = S.fed_end_ms;
        respond("{\"ok\":true,\"state\":\"%s\",\"pos_ms\":%lld,"
                "\"accepted_ms\":%lld,\"fed_ms\":%lld,\"fed_end_ms\":%lld,"
                "\"done_count\":%llu,\"queue\":%d,\"rate\":%d,\"channels\":%d,"
                "\"volume\":%.2f,\"uptime_ms\":%lld}",
                state_str(), rend, (long long)(acc_bytes / OUT_BYTES_PER_MS),
                fed, S.fed_end_ms, S.done_count, S.queue_len,
                S.pipe_rate, S.pipe_channels, S.volume,
                mono_ms() - g_started_ms);
    } else if (!strcmp(cmd, "volume")) {
        long long v = json_num(line, "value_x1000", 1000);
        double vol = (double)v / 1000.0;
        if (vol < 0) vol = 0;
        if (vol > 4) vol = 4;
        S.volume = vol;
        if (G.g_object_set && S.pipe) {
            GstElement *el = G.gst_bin_get_by_name(GST_BIN(S.pipe), "vol");
            if (el) {
                G.g_object_set(el, "volume", vol, (char *)NULL);
                G.gst_object_unref(el);
                respond("{\"ok\":true,\"volume\":%.2f,\"live\":true}", vol);
                return;
            }
        }
        respond("{\"ok\":true,\"volume\":%.2f,\"live\":false}", vol);
    } else if (!strcmp(cmd, "shutdown")) {
        respond("{\"ok\":true,\"bye\":true}");
        usleep(50000);
        exit(0);
    } else {
        respond("{\"ok\":false,\"error\":\"unknown cmd\"}");
    }
}

/* ------------------------------------------------------------------ */
/* Per-tick bookkeeping                                                 */
/* ------------------------------------------------------------------ */

static void
tick_bookkeeping(void) {
    wall_now = mono_ms();
    refresh_clock();

    /* Clip completion: the rendered estimate crossed the clip's end. */
    long long rend = rendered_ms();
    while (S.queue_len > 0 && rend >= S.queue[0].end_ms) {
        log_line("clip done: %s (end_ms=%lld)", S.queue[0].path,
                 S.queue[0].end_ms);
        if (S.queue[0].fd >= 0) {
            close(S.queue[0].fd);
            S.queue[0].fd = -1;
        }
        for (int i = 1; i < S.queue_len; i++) S.queue[i - 1] = S.queue[i];
        S.queue_len--;
        S.done_count++;
        if (S.feed_idx > 0) S.feed_idx--;
    }

    if (!S.announce_idle && S.queue_len == 0 && S.fed_end_ms > 0 &&
        rend >= S.fed_end_ms) {
        S.announce_idle = 1;
        log_line("queue drained; idle keepalive (pos=%lldms)", rend);
        respond("{\"event\":\"idle\",\"pos_ms\":%lld,\"done_count\":%llu}",
                rend, S.done_count);
    }

    /* Bus: errors only. Our O_RDWR fifo never produces a natural EOS. */
    if (S.bus) {
        GstMessage *msg = G.gst_bus_poll(S.bus, GST_MESSAGE_ERROR, 0);
        if (msg) {
            GError *e = NULL;
            gchar *dbg = NULL;
            G.gst_message_parse_error(msg, &e, &dbg);
            log_line("pipeline ERROR: %s (%s)",
                     e && e->message ? e->message : "?", dbg ? dbg : "-");
            respond("{\"event\":\"error\",\"message\":\"pipeline\"}");
            long long now = mono_ms();
            if (S.pipeline_retries < 3 &&
                (S.pipeline_last_fail_ms == 0 ||
                 now - S.pipeline_last_fail_ms > 30000)) {
                S.pipeline_retries++;
                S.pipeline_last_fail_ms = now;
                pipeline_teardown();
                reset_stream();
                if (fifo_open() == 0) pipeline_build(g_rate, g_channels);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_quit;

static void
on_term(int sig) {
    (void)sig;
    g_quit = 1;
}

int
main(int argc, char **argv) {
    g_started_ms = mono_ms();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) g_socket_path = argv[++i];
        else if (!strcmp(argv[i], "--fifo") && i + 1 < argc)
            snprintf(S.fifo_path, sizeof S.fifo_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) g_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc)
            g_channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc)
            S.volume = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sink") && i + 1 < argc)
            g_sink_name = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: audioout [--socket PATH] [--fifo PATH]"
                   " [--rate HZ] [--channels N] [--volume 0-4]"
                   " [--sink ELEMENT]\n");
            return 0;
        }
    }
    if (!S.fifo_path[0])
        snprintf(S.fifo_path, sizeof S.fifo_path, "%s", FIFO_PATH_DEFAULT);
    /* Static struct: fd sentinels must be -1, not 0 (fd 0 is stdin and
     * would make fifo_open think it is already open). */
    S.fifo_fd = -1;
    S.listen_fd = -1;
    S.client_fd = -1;
    if (S.volume <= 0) S.volume = 1.0;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    const char *names[] = {
        "libgstreamer-1.0.so.0", "/lib/libgstreamer-1.0.so.0", NULL,
    };
    for (int i = 0; names[i] && !G.gst; i++) {
        G.gst = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (G.gst) log_line("dlopened %s", names[i]);
    }
    if (!G.gst) {
        log_line("FATAL: no libgstreamer-1.0 (%s)", dlerror());
        return 2;
    }
    G.gobj = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);

#define BIND(field, name)                                   \
    G.field = dlsym(G.gst, name);                           \
    if (!G.field) {                                         \
        log_line("FATAL: %s not exported", name);           \
        return 2;                                           \
    }
    BIND(gst_init, "gst_init");
    BIND(gst_parse_launch, "gst_parse_launch");
    BIND(gst_element_set_state, "gst_element_set_state");
    BIND(gst_element_get_bus, "gst_element_get_bus");
    BIND(gst_bus_poll, "gst_bus_poll");
    BIND(gst_message_parse_error, "gst_message_parse_error");
    BIND(gst_object_unref, "gst_object_unref");
    BIND(gst_bin_get_by_name, "gst_bin_get_by_name");
    BIND(gst_element_get_static_pad, "gst_element_get_static_pad");
    BIND(gst_pad_add_probe, "gst_pad_add_probe");
    BIND(gst_buffer_get_size, "gst_buffer_get_size");
#undef BIND
    G.gst_element_query_position = dlsym(G.gst, "gst_element_query_position");
    G.gst_element_factory_find = dlsym(G.gst, "gst_element_factory_find");
    if (G.gobj)
        G.g_object_set = dlsym(G.gobj, "g_object_set");
    if (!G.gst_element_query_position)
        log_line("note: query_position unavailable; probe clock only");
    if (!G.g_object_set)
        log_line("note: g_object_set unavailable; volume applies on rebuild");

    G.gst_init(NULL, NULL);

    unlink(g_socket_path);
    S.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (S.listen_fd < 0) {
        log_line("FATAL: socket: %s", strerror(errno));
        return 2;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_socket_path);
    if (bind(S.listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(S.listen_fd, 2) < 0) {
        log_line("FATAL: bind/listen: %s", strerror(errno));
        return 2;
    }
    fcntl(S.listen_fd, F_SETFL, O_NONBLOCK);
    log_line("listening on %s (fifo %s, %d Hz x %d ch)", g_socket_path,
             S.fifo_path, g_rate, g_channels);

    claim_exclusive_socket();
    /* The pipeline builds lazily on the first play command: the vendor
     * sink's init can block for many seconds with no A2DP peer. */
    log_line("ready (pipeline builds on first play)");

    S.client_fd = -1;
    S.feed_idx = -1;
    S.all_fed = 1;

    char rbuf[LINE_MAX_LEN];
    size_t rlen = 0;

    while (!g_quit) {
        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds].fd = S.listen_fd;
        fds[nfds].events = POLLIN;
        nfds++;
        int cli = S.client_fd;
        if (cli >= 0) {
            fds[nfds].fd = cli;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        int rc = poll(fds, nfds, 10);
        if (rc < 0 && errno != EINTR) break;

        int c = accept(S.listen_fd, NULL, NULL);
        if (c >= 0) {
            if (S.client_fd >= 0) {
                log_line("replacing client connection");
                close(S.client_fd);
            }
            S.client_fd = c;
            fcntl(c, F_SETFL, O_NONBLOCK);
            rlen = 0;
            log_line("client connected");
        }

        if (S.client_fd >= 0) {
            for (;;) {
                ssize_t n = recv(S.client_fd, rbuf + rlen,
                                 sizeof rbuf - rlen - 1, MSG_DONTWAIT);
                if (n > 0) {
                    rlen += (size_t)n;
                    rbuf[rlen] = 0;
                    char *nl;
                    while ((nl = memchr(rbuf, '\n', rlen)) != NULL) {
                        *nl = 0;
                        if (nl > rbuf) handle_command(rbuf);
                        size_t consumed = (size_t)(nl - rbuf) + 1;
                        memmove(rbuf, nl + 1, rlen - consumed);
                        rlen -= consumed;
                        rbuf[rlen] = 0;
                    }
                    if (rlen >= sizeof rbuf - 1) rlen = 0;
                    continue;
                }
                if (n == 0 ||
                    (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    log_line("client gone");
                    close(S.client_fd);
                    S.client_fd = -1;
                }
                break;
            }
        }

        feed_tick();
        tick_bookkeeping();
    }

    log_line("shutting down");
    pipeline_teardown();
    if (S.client_fd >= 0) close(S.client_fd);
    close(S.listen_fd);
    unlink(g_socket_path);
    return 0;
}
