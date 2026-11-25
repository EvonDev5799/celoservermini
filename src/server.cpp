// Serveur TCP de news : mono-thread, boucle poll(), authentification,
// historique persistant et rejeu depuis un identifiant donne.
//
// Protocole (texte, une commande par ligne terminee par '\n') :
//   client -> serveur : AUTH <user> <password>
//                       SUBSCRIBE <last_id>      (0 = depuis le debut)
//                       QUIT
//   serveur -> client : OK
//                       ERR <raison>
//                       NEWS <id> <texte>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "history.hpp"
#include "net.hpp"
#include "users.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr size_t kMaxLine = 4096;             // taille max d'une commande client
constexpr size_t kOutHighWater = 64 * 1024;   // on ne remplit pas plus le tampon d'envoi
constexpr int kMaxAuthFailures = 3;

struct Client {
    Fd fd;
    std::string peer;
    std::string in, out;       // tampons de lecture / d'ecriture
    bool authed = false;
    bool subscribed = false;
    bool close_after_flush = false;
    bool dead = false;
    int auth_failures = 0;
    std::string user;
    uint64_t next_id = 0;      // prochaine news a envoyer a ce client
};

void log(const std::string& msg) { std::cerr << "[server] " << msg << std::endl; }

void handle_line(Client& c, const std::string& line, const UserDb& users, const NewsStore& store) {
    std::istringstream is(line);
    std::string cmd;
    is >> cmd;

    if (cmd == "AUTH") {
        std::string user, password;
        is >> user;
        std::getline(is >> std::ws, password);  // le mot de passe peut contenir des espaces
        if (c.authed) {
            c.out += "ERR already_authenticated\n";
        } else if (users.check(user, password)) {
            c.authed = true;
            c.user = user;
            c.out += "OK\n";
            log(c.peer + " authentifie en tant que '" + user + "'");
        } else {
            c.out += "ERR auth_failed\n";
            log(c.peer + " echec d'authentification");
            if (++c.auth_failures >= kMaxAuthFailures) c.close_after_flush = true;
        }
    } else if (cmd == "SUBSCRIBE") {
        long long since = -1;
        if (!c.authed) {
            c.out += "ERR not_authenticated\n";
        } else if (c.subscribed) {
            c.out += "ERR already_subscribed\n";
        } else if (!(is >> since) || since < 0) {
            c.out += "ERR bad_request\n";
        } else if (static_cast<uint64_t>(since) > store.last_id()) {
            // Le client pretend avoir recu plus que ce que le serveur connait
            // (ex. historique serveur perdu) : on refuse plutot que de mentir.
            c.out += "ERR bad_since last_id=" + std::to_string(store.last_id()) + "\n";
            c.close_after_flush = true;
        } else {
            c.subscribed = true;
            c.next_id = static_cast<uint64_t>(since) + 1;
            c.out += "OK\n";
            log(c.peer + " abonne, reprise apres id=" + std::to_string(since));
        }
    } else if (cmd == "QUIT") {
        c.close_after_flush = true;
    } else if (!cmd.empty()) {
        c.out += "ERR unknown_command\n";
    }
}

// Extrait les lignes completes du tampon d'entree (TCP = flux d'octets : une
// lecture peut contenir une demi-ligne ou plusieurs lignes).
void process_input(Client& c, const UserDb& users, const NewsStore& store) {
    size_t pos;
    while (!c.close_after_flush && (pos = c.in.find('\n')) != std::string::npos) {
        std::string line = c.in.substr(0, pos);
        c.in.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        handle_line(c, line, users, store);
    }
    if (c.in.size() > kMaxLine) {
        c.out += "ERR line_too_long\n";
        c.close_after_flush = true;
    }
}

// Ajoute au tampon d'envoi les news manquantes, par paquets (contre-pression :
// un client lent ne fait pas gonfler la memoire du serveur).
void pump_news(Client& c, const NewsStore& store) {
    if (!c.subscribed || c.close_after_flush) return;
    while (c.next_id <= store.last_id() && c.out.size() < kOutHighWater) {
        c.out += "NEWS " + std::to_string(c.next_id) + " " + store.text(c.next_id) + "\n";
        ++c.next_id;
    }
}

// Envoi non bloquant. MSG_NOSIGNAL : pas de SIGPIPE si le client est mort.
void flush(Client& c) {
    while (!c.out.empty()) {
        ssize_t n = ::send(c.fd.get(), c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // on reessaiera quand poll() signalera POLLOUT
        } else {
            c.dead = true;
            return;
        }
    }
    if (c.close_after_flush) c.dead = true;
}

void read_from(Client& c) {
    char buf[4096];
    ssize_t n = ::recv(c.fd.get(), buf, sizeof buf, 0);
    if (n > 0) c.in.append(buf, static_cast<size_t>(n));
    else if (n == 0) c.dead = true;  // le client a ferme
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.dead = true;
}

Fd make_listener(const std::string& bind_addr, int port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid()) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    int yes = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);  // relance rapide apres crash
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("adresse invalide: " + bind_addr);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(fd.get(), 128) != 0) throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    if (!set_nonblocking(fd.get())) throw std::runtime_error("set_nonblocking");
    return fd;
}

// Generateur de news factices (le "flux" a diffuser).
std::string generate_news(uint64_t next_id) {
    static const char* topics[] = {"Economie", "Sport", "Tech", "Monde", "Science"};
    return std::string("[") + topics[next_id % 5] + "] Depeche numero " + std::to_string(next_id);
}

void usage() {
    std::cerr << "Usage: news_server [--port 5555] [--bind 0.0.0.0] [--users users.txt]\n"
                 "                   [--history history.log] [--interval ms (0 = pas de generation)]\n"
                 "       news_server --add-user <nom> <mot_de_passe> [--users users.txt]\n";
}

}  // namespace

int main(int argc, char** argv) {
    int port = 5555;
    int interval_ms = 1000;
    std::string bind_addr = "0.0.0.0", users_path = "users.txt", history_path = "history.log";
    std::string add_user, add_pass;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("valeur manquante pour " + a);
                return argv[++i];
            };
            if (a == "--port") port = std::stoi(next());
            else if (a == "--bind") bind_addr = next();
            else if (a == "--users") users_path = next();
            else if (a == "--history") history_path = next();
            else if (a == "--interval") interval_ms = std::stoi(next());
            else if (a == "--add-user") { add_user = next(); add_pass = next(); }
            else { usage(); return 2; }
        }
        if (!add_user.empty()) {
            UserDb::add(users_path, add_user, add_pass);
            std::cout << "Utilisateur '" << add_user << "' ajoute dans " << users_path << std::endl;
            return 0;
        }

        UserDb users(users_path);
        NewsStore store(history_path);
        Fd listener = make_listener(bind_addr, port);

        struct sigaction sa{};
        sa.sa_handler = on_signal;  // pas de SA_RESTART : poll() est interrompu proprement
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);
        ::signal(SIGPIPE, SIG_IGN);

        log("ecoute sur " + bind_addr + ":" + std::to_string(port) + ", historique: " +
            std::to_string(store.last_id()) + " news");

        using clock = std::chrono::steady_clock;
        auto next_gen = clock::now() + std::chrono::milliseconds(interval_ms);
        std::vector<Client> clients;

        while (!g_stop) {
            // 1) Construire la liste des descripteurs a surveiller.
            std::vector<pollfd> pfds;
            pfds.push_back({listener.get(), POLLIN, 0});
            for (const Client& c : clients)
                pfds.push_back({c.fd.get(), static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});

            // 2) Le timeout de poll() sert d'horloge pour le generateur de news.
            int timeout = -1;
            if (interval_ms > 0) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(next_gen - clock::now());
                timeout = remaining.count() < 0 ? 0 : static_cast<int>(remaining.count()) + 1;
            }
            int ready = ::poll(pfds.data(), pfds.size(), timeout);
            if (ready < 0 && errno != EINTR) throw std::runtime_error(std::string("poll: ") + std::strerror(errno));

            if (interval_ms > 0 && clock::now() >= next_gen) {
                store.append(generate_news(store.last_id() + 1));
                next_gen = clock::now() + std::chrono::milliseconds(interval_ms);
            }

            // 3) Traiter les clients existants (avant d'accepter les nouveaux :
            //    les indices de pfds correspondent a ceux de clients).
            if (ready > 0) {
                for (size_t i = 0; i < clients.size(); ++i) {
                    short ev = pfds[i + 1].revents;
                    if (ev & (POLLIN | POLLHUP | POLLERR)) read_from(clients[i]);
                    if (!clients[i].dead) process_input(clients[i], users, store);
                }
            }
            for (Client& c : clients) {
                if (c.dead) continue;
                pump_news(c, store);
                flush(c);
            }

            // 4) Retirer les clients morts (le destructeur de Fd ferme le socket).
            for (auto it = clients.begin(); it != clients.end();) {
                if (it->dead) {
                    log(it->peer + " deconnecte");
                    it = clients.erase(it);
                } else {
                    ++it;
                }
            }

            // 5) Accepter les nouvelles connexions.
            if (ready > 0 && (pfds[0].revents & POLLIN)) {
                for (;;) {
                    sockaddr_in addr{};
                    socklen_t len = sizeof addr;
                    Fd fd(::accept(listener.get(), reinterpret_cast<sockaddr*>(&addr), &len));
                    if (!fd.valid()) break;  // EAGAIN : plus rien a accepter
                    if (!set_nonblocking(fd.get())) continue;
                    char ip[INET_ADDRSTRLEN] = "?";
                    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
                    Client c;
                    c.fd = std::move(fd);
                    c.peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
                    log(c.peer + " connecte");
                    clients.push_back(std::move(c));
                }
            }
        }
        log("arret propre");
    } catch (const std::exception& e) {
        std::cerr << "Erreur: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
