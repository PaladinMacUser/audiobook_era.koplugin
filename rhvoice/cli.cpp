// Offline RHVoice/SLT WAV adapter for KOReader. GPL-3.0-or-later.
#include "RHVoice.h"
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

struct Output {
    FILE *file;
    uint32_t rate = 0, samples = 0;
};
static void le16(FILE *f, uint16_t n) { fputc(n & 255, f); fputc(n >> 8, f); }
static void le32(FILE *f, uint32_t n) { le16(f, n & 65535); le16(f, n >> 16); }
static void header(FILE *f, uint32_t rate, uint32_t samples) {
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); le32(f, samples * 2 + 36);
    fwrite("WAVEfmt ", 1, 8, f); le32(f, 16); le16(f, 1); le16(f, 1);
    le32(f, rate); le32(f, rate * 2); le16(f, 2); le16(f, 16);
    fwrite("data", 1, 4, f); le32(f, samples * 2);
}
static int set_rate(int rate, void *userdata) {
    Output *out = static_cast<Output *>(userdata);
    if (rate < 8000 || rate > 48000 || (out->samples && out->rate != (unsigned)rate)) return 0;
    out->rate = rate;
    return 1;
}
static int speech(const short *samples, unsigned count, void *userdata) {
    Output *out = static_cast<Output *>(userdata);
    if (count > (UINT32_MAX - 36) / 2 - out->samples) return 0;
    if (fwrite(samples, 2, count, out->file) != count) return 0;
    out->samples += count;
    return 1;
}
static bool number(const char *text, double min, double max, double &value) {
    char *end;
    errno = 0;
    value = strtod(text, &end);
    return end != text && !*end && !errno && std::isfinite(value) && value >= min && value <= max;
}
int main(int argc, char **argv) {
    std::string input, output, data;
    double speed = 1, volume = 1;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) return 2;
        std::string flag = argv[i];
        if (flag == "--input") input = argv[i + 1];
        else if (flag == "--output") output = argv[i + 1];
        else if (flag == "--data") data = argv[i + 1];
        else if (flag == "--speed") { if (!number(argv[i + 1], 0.25, 2, speed)) return 2; }
        else if (flag == "--volume") { if (!number(argv[i + 1], 0, 1, volume)) return 2; }
        else return 2;
    }
    if (input.empty() || output.empty()) return 2;
    if (data.empty()) {
        char executable[4096];
        ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
        if (n <= 0 || n >= (ssize_t)sizeof executable - 1) return 2;
        executable[n] = 0;
        data = executable;
        data = data.substr(0, data.find_last_of('/')) + "/data";
    }
    std::ifstream source(input.c_str(), std::ios::binary | std::ios::ate);
    if (!source || source.tellg() <= 0 || source.tellg() > 1048576) return 2;
    source.seekg(0);
    std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    if (!source.eof() && source.fail()) return 2;
    Output out;
    std::string temporary = output + ".rhvoice.XXXXXX";
    int fd = mkstemp(&temporary[0]);
    if (fd < 0) return 1;
    out.file = fdopen(fd, "w+b");
    if (!out.file) { close(fd); unlink(temporary.c_str()); return 1; }
    header(out.file, 0, 0);
    RHVoice_init_params init = {};
    init.data_path = data.c_str();
    std::string config = data + "/config";
    init.config_path = config.c_str();
    init.callbacks.set_sample_rate = set_rate;
    init.callbacks.play_speech = speech;
    RHVoice_tts_engine engine = RHVoice_new_tts_engine(&init);
    RHVoice_synth_params params = {};
    params.voice_profile = "Slt";
    params.relative_rate = speed;
    params.relative_pitch = 1;
    params.relative_volume = volume;
    params.punctuation_mode = RHVoice_punctuation_none;
    RHVoice_message message = engine ? RHVoice_new_message(engine, text.data(), text.size(),
        RHVoice_message_text, &params, &out) : NULL;
    bool ok = message && RHVoice_speak(message) && out.samples && out.rate && !ferror(out.file);
    if (message) RHVoice_delete_message(message);
    if (engine) RHVoice_delete_tts_engine(engine);
    if (ok) header(out.file, out.rate, out.samples);
    if (ferror(out.file)) ok = false;
    if (fclose(out.file)) ok = false;
    if (ok && rename(temporary.c_str(), output.c_str())) ok = false;
    unlink(temporary.c_str());
    if (!ok) { fprintf(stderr, "RHVoice/SLT could not generate audio\n"); return 1; }
    fprintf(stderr, "RHVoice/SLT: %u Hz, %u samples, rate %.3f\n", out.rate, out.samples, speed);
    return 0;
}
