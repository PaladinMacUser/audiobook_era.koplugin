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
#include <csignal>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

struct Output {
    FILE *file;
    uint32_t rate = 0, samples = 0;
    uint32_t lead_ms = 0, lead_samples = 0;
    std::string cancel;
};
static volatile sig_atomic_t running = 1;
static void stop_server(int) { running = 0; }
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
    if (!running || (!out->cancel.empty() && access(out->cancel.c_str(), F_OK) == 0)) return 0;
    if (rate < 8000 || rate > 48000 || (out->samples && out->rate != (unsigned)rate)) return 0;
    out->rate = rate;
    if (!out->samples && out->lead_ms) {
        static const short silence[1024] = {};
        out->lead_samples = rate * out->lead_ms / 1000;
        while (out->samples < out->lead_samples) {
            unsigned n = out->lead_samples - out->samples;
            if (n > 1024) n = 1024;
            if (fwrite(silence, 2, n, out->file) != n) return 0;
            out->samples += n;
        }
    }
    return 1;
}
static int speech(const short *samples, unsigned count, void *userdata) {
    Output *out = static_cast<Output *>(userdata);
    if (!running || (!out->cancel.empty() && access(out->cancel.c_str(), F_OK) == 0)) return 0;
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
static bool render(RHVoice_tts_engine engine, const std::string &input,
                   const std::string &output, double speed, double volume,
                   double lead_ms, const std::string &cancel = "",
                   const std::string &voice = "Slt", double pitch = 1, bool ssml = false) {
    bool installed=false;
    const RHVoice_voice_info *voices=RHVoice_get_voices(engine);
    for(unsigned i=0;i<RHVoice_get_number_of_voices(engine);i++)
        if(voices[i].name && voice==voices[i].name)installed=true;
    if(!installed){fprintf(stderr,"RHVoice: selected voice is not installed\n");return false;}
    std::ifstream source(input.c_str(), std::ios::binary | std::ios::ate);
    if (!source || source.tellg() <= 0 || source.tellg() > 1048576) return false;
    source.seekg(0);
    std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    if ((!source.eof() && source.fail()) || (!cancel.empty() && access(cancel.c_str(), F_OK) == 0)) return false;
    Output out;
    out.lead_ms = static_cast<uint32_t>(lead_ms);
    out.cancel = cancel;
    std::string temporary = output + ".rhvoice.XXXXXX";
    int fd = mkstemp(&temporary[0]);
    if (fd < 0) return false;
    out.file = fdopen(fd, "w+b");
    if (!out.file) { close(fd); unlink(temporary.c_str()); return false; }
    header(out.file, 0, 0);
    RHVoice_synth_params params = {};
    params.voice_profile = voice.c_str();
    params.relative_rate = speed;
    params.relative_pitch = pitch;
    params.relative_volume = volume;
    params.punctuation_mode = RHVoice_punctuation_none;
    RHVoice_message message = engine ? RHVoice_new_message(engine, text.data(), text.size(),
        ssml ? RHVoice_message_ssml : RHVoice_message_text, &params, &out) : NULL;
    bool ok = message && RHVoice_speak(message) && out.samples > out.lead_samples && out.rate && !ferror(out.file);
    if (message) RHVoice_delete_message(message);
    if (ok) header(out.file, out.rate, out.samples);
    if (ferror(out.file)) ok = false;
    if (fclose(out.file)) ok = false;
    if (!running || (!cancel.empty() && access(cancel.c_str(), F_OK) == 0)) ok = false;
    if (ok && rename(temporary.c_str(), output.c_str())) ok = false;
    unlink(temporary.c_str());
    if (!ok) { fprintf(stderr, "RHVoice/SLT could not generate audio\n"); return false; }
    fprintf(stderr, "RHVoice/%s: %u Hz, %u samples, rate %.3f, pitch %.3f\n", voice.c_str(), out.rate, out.samples, speed, pitch);
    return true;
}

static bool marker(const std::string &path, const std::string &text) {
    std::string temporary = path + ".new";
    FILE *f = fopen(temporary.c_str(), "w");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f)) ok = false;
    if (ok && rename(temporary.c_str(), path.c_str())) ok = false;
    unlink(temporary.c_str());
    return ok;
}
static bool local_path(const std::string &path, const std::string &directory) {
    const std::string prefix = directory + "/";
    return path.compare(0, prefix.size(), prefix) == 0 && path.size() > prefix.size()
        && path.find('/', prefix.size()) == std::string::npos
        && path.find("..", prefix.size()) == std::string::npos
        && path.find_first_of("\r\n") == std::string::npos;
}
static int serve(RHVoice_tts_engine engine, const std::string &socket_path, unsigned idle_seconds) {
    sockaddr_un address = {};
    if (socket_path.size() >= sizeof address.sun_path) return 2;
    const std::string directory = socket_path.substr(0, socket_path.find_last_of('/'));
    const std::string ready = socket_path + ".ready";
    int lock = open((socket_path + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (lock < 0) return 1;
    if (flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return 1; }
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { close(lock); return 1; }
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    unlink(socket_path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address)) { close(fd); close(lock); return 1; }
    chmod(socket_path.c_str(), 0600);
    signal(SIGTERM, stop_server); signal(SIGINT, stop_server);
    marker(ready, std::to_string(getpid()) + "\n");
    fprintf(stderr, "RHVoice resident engine ready\n");
    auto last = std::chrono::steady_clock::now();
    while (running) {
        pollfd event = {fd, POLLIN, 0};
        int status = poll(&event, 1, 1000);
        if (status < 0) { if (errno == EINTR) continue; break; }
        if (!status) {
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last).count() >= idle_seconds) break;
            continue;
        }
        char packet[4096];
        ssize_t n = recv(fd, packet, sizeof packet, 0);
        if (n <= 0 || n >= (ssize_t)sizeof packet) continue;
        std::string job(packet, static_cast<size_t>(n));
        if (!local_path(job, directory) || job.size() < 4 || job.substr(job.size()-4) != ".req") continue;
        std::ifstream request(job.c_str());
        std::string input, output, rate, volume, lead;
        std::getline(request, input); std::getline(request, output); std::getline(request, rate);
        std::getline(request, volume); std::getline(request, lead);
        double r, v, l;
        bool valid = request && local_path(input, directory) && local_path(output, directory)
            && number(rate.c_str(), 0.25, 2, r) && number(volume.c_str(), 0, 1, v)
            && number(lead.c_str(), 0, 2000, l);
        std::string voice="Slt", pitch_text="1", mode="text";
        if (request.peek()!=std::char_traits<char>::eof()) {
            std::getline(request, voice);std::getline(request, pitch_text);std::getline(request, mode);
        }
        double pitch=1;
        valid = valid && (voice=="Slt" || voice=="Bdl" || voice=="Clb" || voice=="Alan")
            && number(pitch_text.c_str(), .85, 1.15, pitch) && (mode=="text" || mode=="ssml");
        bool ok = valid && render(engine, input, output, r, v, l, job + ".cancel", voice, pitch, mode=="ssml");
        bool cancelled = access((job + ".cancel").c_str(), F_OK) == 0;
        marker(job + ".done", ok ? "OK\n" : cancelled ? "CANCELLED\n" : "ERR synthesis failed\n");
        last = std::chrono::steady_clock::now();
    }
    close(fd); unlink(ready.c_str()); unlink(socket_path.c_str()); close(lock);
    return 0;
}
int main(int argc, char **argv) {
    std::string input, output, data, daemon, voice="Slt", mode="text";
    double speed = 1, volume = 1, lead = 0, idle = 120, pitch = 1;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) return 2;
        std::string flag = argv[i];
        if (flag == "--input") input = argv[i+1];
        else if (flag == "--output") output = argv[i+1];
        else if (flag == "--data") data = argv[i+1];
        else if (flag == "--daemon") daemon = argv[i+1];
        else if (flag == "--voice") { voice=argv[i+1]; if (voice!="Slt" && voice!="Bdl" && voice!="Clb" && voice!="Alan") return 2; }
        else if (flag == "--pitch") { if (!number(argv[i+1], .85, 1.15, pitch)) return 2; }
        else if (flag == "--format") { mode=argv[i+1]; if (mode!="text" && mode!="ssml") return 2; }
        else if (flag == "--speed") { if (!number(argv[i+1], .25, 2, speed)) return 2; }
        else if (flag == "--volume") { if (!number(argv[i+1], 0, 1, volume)) return 2; }
        else if (flag == "--lead-in-ms") { if (!number(argv[i+1], 0, 2000, lead)) return 2; }
        else if (flag == "--idle-seconds") { if (!number(argv[i+1], 1, 600, idle)) return 2; }
        else return 2;
    }
    if (daemon.empty() && (input.empty() || output.empty())) return 2;
    if (data.empty()) {
        char executable[4096]; ssize_t n = readlink("/proc/self/exe", executable, sizeof executable-1);
        if (n <= 0 || n >= (ssize_t)sizeof executable-1) return 2;
        executable[n] = 0; data = executable;
        data = data.substr(0, data.find_last_of('/')) + "/data";
    }
    RHVoice_init_params init = {};
    init.data_path = data.c_str();
    std::string config = data + "/config"; init.config_path = config.c_str();
    init.callbacks.set_sample_rate = set_rate; init.callbacks.play_speech = speech;
    RHVoice_tts_engine engine = RHVoice_new_tts_engine(&init);
    if (!engine) { fprintf(stderr, "RHVoice: engine initialization failed\n"); return 1; }
    int result = daemon.empty() ? (render(engine, input, output, speed, volume, lead, "", voice, pitch, mode=="ssml") ? 0 : 1)
        : serve(engine, daemon, static_cast<unsigned>(idle));
    RHVoice_delete_tts_engine(engine);
    return result;
}
