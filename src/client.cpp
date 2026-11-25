// Client de news : se connecte, s'authentifie, s'abonne depuis le dernier id
// recu (persiste sur disque) et se reconnecte automatiquement (backoff exponentiel).
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "net.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Config {
    std::string host = "127.0.0.1", port = "5555", user, password, state = "last_id.txt";
};

void log(const std::string& msg) { std::cerr << "[client] " << msg << std::endl; }

// ---- Persistance du dernier id recu --------------------------------------
uint64_t load_state(const std::string& path) {
    std::ifstream in(path);
    uint64_t id = 0;
    if (in >> id) return id;
    return 0;
}

// Ecriture atomique (fichier temporaire + rename) : un crash en pleine ecriture
// ne laisse jamais un fichier d'etat a moitie ecrit.
void save_state(const std::string& path, uint64_t id) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << id << '\n';
    }
    std::rename(tmp.c_str(), path.c_str());
}

// ---- Reseau ----------------------------------------------------------------
Fd connect_to(const Config& cfg) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(cfg.host.c_str(), cfg.port.c_str(), &hints, &res) != 0) return Fd();
    Fd result;
    for (addrinfo* p = res; p; p = p->ai_next) {
        Fd fd(::socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (!fd.valid()) continue;
        if (::connect(fd.get(), p->ai_addr, p->ai_addrlen) == 0) {
            result = std::move(fd);
            break;
        }
    }
    ::freeaddrinfo(res);
    return result;
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) sent += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) { if (g_stop) return false; }
        else return false;
    }
    return true;
}

enum class SessionEnd { Disconnected, Fatal, Stopped };

// Une session = une connexion. Retourne la raison de sa fin ;
// `handshake_done` indique si on a ete abonne avec succes (pour remettre le backoff a zero).
SessionEnd run_session(const Config& cfg, uint64_t& last_id, bool& handshake_done) {
    Fd fd = connect_to(cfg);
    if (!fd.valid()) return SessionEnd::Disconnected;
    int yes = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);  // detecte les liens morts
    log("connecte, reprise apres id=" + std::to_string(last_id));

    // On envoie AUTH puis SUBSCRIBE d'un coup : le serveur les traite dans l'ordre.
    if (!send_all(fd.get(), "AUTH " + cfg.user + " " + cfg.password + "\nSUBSCRIBE " + std::to_string(last_id) + "\n"))
        return SessionEnd::Disconnected;

    std::string buf;
    int oks_expected = 2;
    char chunk[4096];
    while (!g_stop) {
        ssize_t n = ::recv(fd.get(), chunk, sizeof chunk, 0);
        if (n == 0) return SessionEnd::Disconnected;
        if (n < 0) {
            if (errno == EINTR) continue;
            return SessionEnd::Disconnected;
        }
        buf.append(chunk, static_cast<size_t>(n));

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.rfind("NEWS ", 0) == 0) {
                std::istringstream is(line.substr(5));
                uint64_t id = 0;
                if (!(is >> id)) { log("ligne NEWS invalide ignoree"); continue; }
                std::string text;
                std::getline(is >> std::ws, text);
                if (id <= last_id) continue;  // doublon : ignore (idempotence)
                if (id != last_id + 1)
                    log("attention: trou detecte (" + std::to_string(last_id) + " -> " + std::to_string(id) + ")");
                // 1) traiter (ici : afficher), 2) puis memoriser. Un crash entre les
                // deux provoque un doublon, jamais une perte (at-least-once).
                std::cout << id << '\t' << text << std::endl;
                save_state(cfg.state, id);
                last_id = id;
            } else if (line == "OK") {
                if (--oks_expected == 0) handshake_done = true;
            } else if (line.rfind("ERR", 0) == 0) {
                log("erreur serveur: " + line);
                return SessionEnd::Fatal;  // auth refusee, since invalide... : inutile de reessayer
            }
        }
    }
    return SessionEnd::Stopped;
}

void interruptible_sleep(std::chrono::milliseconds total) {
    const auto step = std::chrono::milliseconds(100);
    for (auto waited = std::chrono::milliseconds(0); waited < total && !g_stop; waited += step)
        std::this_thread::sleep_for(step);
}

void usage() {
    std::cerr << "Usage: news_client --user <nom> --pass <mot_de_passe> [--host 127.0.0.1] [--port 5555]\n"
                 "                   [--state last_id.txt]\n";
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    try {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("valeur manquante pour " + a);
            return argv[++i];
        };
        if (a == "--host") cfg.host = next();
        else if (a == "--port") cfg.port = next();
        else if (a == "--user") cfg.user = next();
        else if (a == "--pass") cfg.password = next();
        else if (a == "--state") cfg.state = next();
        else { usage(); return 2; }
    }
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        usage();
        return 2;
    }
    if (cfg.user.empty()) { usage(); return 2; }

    struct sigaction sa{};
    sa.sa_handler = on_signal;  // sans SA_RESTART : recv() est interrompu par Ctrl-C
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    uint64_t last_id = load_state(cfg.state);
    const auto kMaxBackoff = std::chrono::milliseconds(30000);
    auto backoff = std::chrono::milliseconds(500);

    while (!g_stop) {
        bool handshake_done = false;
        SessionEnd end = run_session(cfg, last_id, handshake_done);
        if (end == SessionEnd::Fatal) return 1;
        if (end == SessionEnd::Stopped) break;
        if (handshake_done) backoff = std::chrono::milliseconds(500);  // connexion qui a marche
        log("deconnecte, nouvelle tentative dans " + std::to_string(backoff.count()) + " ms");
        interruptible_sleep(backoff);
        backoff = std::min(backoff * 2, kMaxBackoff);  // backoff exponentiel plafonne
    }
    log("arret (dernier id recu: " + std::to_string(last_id) + ")");
    return 0;
}
