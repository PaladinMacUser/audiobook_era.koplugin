/* Paradee-8M benchmark runner: onnxruntime C API, model -> 24 kHz WAV.
 *
 * Graph I/O (per the model card): input_ids int64 [1,T] (0-pad at both
 * ends, max 512), speed float32 [1]; output waveform float32 [1,N] at
 * 24 kHz. Single thread, int8 model preferred.
 *
 * Build (x86 sanity):
 *   cc -O2 paradee_runner.c -lonnxruntime -o paradee_runner
 * Build (armv7hf): link the nix cross-built libonnxruntime.so, see PLAN.md.
 *
 * Usage: paradee_runner <model.onnx> <sent.ids> [sent.ids ...] [out.wav]
 *   ids files are raw little-endian int64; WAV path applies to the last
 *   sentence only (RTF/paths come from device-side benchmark scripts).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <onnxruntime_c_api.h>

#define SR 24000

static const OrtApi *api;

static void die(const char *what, OrtStatus *s) {
    if (s) {
        fprintf(stderr, "%s: %s\n", what, api->GetErrorMessage(s));
        api->ReleaseStatus(s);
    } else {
        fprintf(stderr, "%s\n", what);
    }
    exit(1);
}

#define ORC(x) do { OrtStatus *_s = (x); if (_s) die(#x, _s); } while (0)

/* Minimal 16-bit WAV writer (24 kHz mono). */
static void write_wav(const char *path, const float *pcm, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) die("fopen out.wav", NULL);
    uint32_t datalen = n * 2, riff = datalen + 36, rate = SR, byte_rate = SR * 2, fmtsz = 16;
    uint16_t audio_format = 1, channels = 1, block_align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&audio_format, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datalen, 4, 1, f);
    for (size_t i = 0; i < n; i++) {
        int16_t v = pcm[i] >= 1 ? 32767 : (pcm[i] <= -1 ? -32768
                     : (int16_t)(pcm[i] * 32767));
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

static int64_t *read_ids(const char *path, size_t *t_out) {
    FILE *f = fopen(path, "rb");
    if (!f) die("fopen ids", NULL);
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes % 8 || bytes / 8 < 3) die("ids file: not int64 LE / too short", NULL);
    int64_t *ids = malloc(bytes);
    if (fread(ids, 1, bytes, f) != (size_t)bytes) die("fread ids", NULL);
    fclose(f);
    *t_out = bytes / 8;
    return ids;
}

int main(int argc, char **argv) {
    if (argc < 3) die("usage: paradee_runner model.onnx a.ids [b.ids ...] [out.wav]", NULL);
    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtEnv *env;
    ORC(api->CreateEnv(ORT_LOGGING_LEVEL_VERBOSE, "paradee", &env));
    OrtSessionOptions *opts;
    ORC(api->CreateSessionOptions(&opts));
    ORC(api->SetIntraOpNumThreads(opts, 1));
    ORC(api->SetInterOpNumThreads(opts, 1));
    ORC(api->SetSessionGraphOptimizationLevel(opts, ORT_DISABLE_ALL));
    OrtSession *session;
    ORC(api->CreateSession(env, argv[1], opts, &session));
    fprintf(stderr, "[dbg] session created\n");

    /* I/O names via the default allocator (freed with AllocatorFree; the
     * process is short, so the small leaks are left in place). */
    OrtAllocator *al;
    ORC(api->GetAllocatorWithDefaultOptions(&al));
    char *in_names[2], *out_name;
    ORC(api->SessionGetInputName(session, 0, al, &in_names[0]));
    ORC(api->SessionGetInputName(session, 1, al, &in_names[1]));
    ORC(api->SessionGetOutputName(session, 0, al, &out_name));
    fprintf(stderr, "[dbg] in0=%s in1=%s out=%s\n", in_names[0], in_names[1], out_name);

    int n_ids = argc - 2; /* every arg after the model is an ids file; the
                             WAV is written next to each ids file */

    for (int a = 0; a < n_ids; a++) {
        size_t T;
        int64_t *ids = read_ids(argv[a + 2], &T);
        float speed = 1.0f;
        fprintf(stderr, "[dbg] ids loaded T=%zu\n", T);
        OrtMemoryInfo *mi;
        const int64_t shape_ids[2] = {1, (int64_t)T};
        const int64_t shape_sp[1] = {1};
        OrtValue *t_ids, *t_sp;
        ORC(api->CreateMemoryInfo("Cpu", OrtArenaAllocator, 0, OrtMemTypeDefault, &mi));
        ORC(api->CreateTensorWithDataAsOrtValue(mi, ids, T * 8, shape_ids, 2,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &t_ids));
        ORC(api->CreateTensorWithDataAsOrtValue(mi, &speed, 4, shape_sp, 1,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &t_sp));

        const OrtValue *inputs[2] = {t_ids, t_sp};
        OrtValue *outputs[1];
        outputs[0] = NULL;
        struct timespec t0, t1;
        fprintf(stderr, "[dbg] tensors ready, running\n");
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ORC(api->Run(session, NULL, (const char *const *)in_names, inputs, 2,
                     (const char *const *)&out_name, 1, outputs));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        fprintf(stderr, "[dbg] run done\n");

        float *pcm = NULL;
        fprintf(stderr, "[dbg] outputs[0]=%p\n", (void *)outputs[0]);
        OrtTensorTypeAndShapeInfo *tsi = NULL;
        fprintf(stderr, "[dbg] calling GetTensorTypeAndShape\n");
        ORC(api->GetTensorTypeAndShape(outputs[0], &tsi));
        fprintf(stderr, "[dbg] type/shape ok tsi=%p\n", (void *)tsi);
        size_t ndim = 0;
        ORC(api->GetDimensionsCount(tsi, &ndim));
        int64_t dims[8] = {0};
        ORC(api->GetDimensions(tsi, dims, ndim));
        api->ReleaseTensorTypeAndShapeInfo(tsi);
        fprintf(stderr, "[dbg] shape ok ndim=%zu dims0=%lld dims1=%lld\n",
                ndim, (long long)dims[0], ndim > 1 ? (long long)dims[1] : -1LL);
        ORC(api->GetTensorMutableData(outputs[0], (void **)&pcm));
        fprintf(stderr, "[dbg] mutable data ok pcm=%p\n", (void *)pcm);
        size_t nsamp = 1;
        for (size_t d = 0; d < ndim; d++) nsamp *= (size_t)dims[d];
        double synth = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
        double audio = nsamp / (double)SR;
        printf("%s: T=%zu samples=%zu synth=%.3fs audio=%.2fs RTF=%.3f\n",
               argv[a + 2], T, nsamp, synth, audio, audio > 0 ? synth / audio : 0);
        {
            char wav[512];
            snprintf(wav, sizeof wav, "%s", argv[a + 2]);
            char *dot = strrchr(wav, '.');
            if (dot) *dot = 0;
            strncat(wav, ".wav", sizeof wav - strlen(wav) - 1);
            write_wav(wav, pcm, nsamp);
        }
        fprintf(stderr, "[dbg] wav written\n");
        api->ReleaseValue(outputs[0]);
        api->ReleaseValue(t_ids);
        api->ReleaseValue(t_sp);
        api->ReleaseMemoryInfo(mi);
        free(ids);
    }
    return 0;
}
