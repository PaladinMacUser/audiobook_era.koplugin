// Private datagram worker protocol, shared with the plugin speech client.
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
static int serve(Ito &engine, const std::string &socket_path, unsigned idle_seconds) {
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
    fprintf(stderr, "Ito resident engine ready\n");
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
        std::string voice="ItoFemale", pitch_text="1", mode="text";
        if (request.peek()!=std::char_traits<char>::eof()) {
            std::getline(request, voice);std::getline(request, pitch_text);std::getline(request, mode);
        }
        double pitch=1;
        valid = valid && (voice=="ItoFemale")
            && number(pitch_text.c_str(), .85, 1.15, pitch) && mode=="text";
        bool ok = valid && render(engine, input, output, r, v, l, job + ".cancel", voice, pitch, false);
        bool cancelled = access((job + ".cancel").c_str(), F_OK) == 0;
        marker(job + ".done", ok ? "OK\n" : cancelled ? "CANCELLED\n" : "ERR synthesis failed\n");
        last = std::chrono::steady_clock::now();
    }
    close(fd); unlink(ready.c_str()); unlink(socket_path.c_str()); close(lock);
    return 0;
}
