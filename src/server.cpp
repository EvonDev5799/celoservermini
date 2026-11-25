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
//
// Lecture conseillee : main() -> Server::run() -> les methodes appelees dans
// l'ordre de la boucle -> handle_command() (la machine a etats).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "history.hpp"
#include "net.hpp"
#include "users.hpp"

namespace {

    using Clock = std::chrono::steady_clock;

    volatile std::sig_atomic_t g_stop = 0;
    void on_signal(int) { g_stop = 1; }

    constexpr size_t kMaxLine = 4096;           // taille max d'une commande client
    constexpr size_t kOutHighWater = 64 * 1024; // on ne remplit pas plus le tampon d'envoi
    constexpr int kMaxAuthFailures = 3;

    void log(const std::string &msg) { std::cerr << "[server] " << msg << std::endl; }

    // ---------------------------------------------------------------------------
    // Client : etat d'une connexion
    // ---------------------------------------------------------------------------

    // Les trois etats d'un client. Un client ne peut recevoir de news que dans
    // l'etat Subscribed, donc seulement apres une authentification reussie.
    enum class State { Unauthenticated,
                       Authenticated,
                       Subscribed };

    struct Client {
        Fd fd;
        std::string peer;
        std::string in, out; // tampons de lecture / d'ecriture
        State state = State::Unauthenticated;
        int auth_failures = 0;
        uint64_t next_id = 0; // curseur : prochaine news a envoyer a ce client
        bool close_after_flush = false;
        bool dead = false;

        void reply(const std::string &line) { out += line + "\n"; }
        void reply_error(const std::string &reason) { reply("ERR " + reason); }
    };

    // ---------------------------------------------------------------------------
    // Analyse d'une ligne de commande (separee de son execution)
    // ---------------------------------------------------------------------------

    struct Command {
        std::string name; // "AUTH", "SUBSCRIBE", ...
        std::string arg;  // premier argument
        std::string rest; // le reste de la ligne (le mot de passe peut contenir des espaces)
    };

    Command parse_command(const std::string &line) {
        std::istringstream is(line);
        Command cmd;
        is >> cmd.name >> cmd.arg;
        std::getline(is >> std::ws, cmd.rest);
        return cmd;
    }

    // ---------------------------------------------------------------------------
    // Entrees/sorties d'un client (sans logique de protocole)
    // ---------------------------------------------------------------------------

    void read_from(Client &c) {
        char buf[4096];
        ssize_t n = ::recv(c.fd.get(), buf, sizeof buf, 0);
        if (n > 0) c.in.append(buf, static_cast<size_t>(n));
        else if (n == 0) c.dead = true; // le client a ferme
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.dead = true;
    }

    // Envoi non bloquant. MSG_NOSIGNAL : pas de SIGPIPE si le client est mort.
    void flush(Client &c) {
        while (!c.out.empty()) {
            ssize_t n = ::send(c.fd.get(), c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n > 0) {
                c.out.erase(0, static_cast<size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return; // on reessaiera quand poll() signalera POLLOUT
            } else {
                c.dead = true;
                return;
            }
        }
        if (c.close_after_flush) c.dead = true;
    }

    Fd make_listener(const std::string &bind_addr, int port) {
        Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
        if (!fd.valid()) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
        int yes = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes); // relance rapide apres crash
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("adresse invalide: " + bind_addr);
        if (::bind(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0)
            throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
        if (::listen(fd.get(), 128) != 0) throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
        if (!set_nonblocking(fd.get())) throw std::runtime_error("set_nonblocking");
        return fd;
    }

    // Generateur de news factices (le "flux" a diffuser).
    std::string generate_news(uint64_t id) {
        static const char *topics[] = {"Economie", "Sport", "Tech", "Monde", "Science"};
        return std::string("[") + topics[id % 5] + "] Depeche numero " + std::to_string(id);
    }

    // ---------------------------------------------------------------------------
    // Options de la ligne de commande
    // ---------------------------------------------------------------------------

    struct Options {
        int port = 5555;
        int interval_ms = 1000;
        std::string bind_addr = "0.0.0.0";
        std::string users_path = "users.txt";
        std::string history_path = "history.log";
        std::string add_user, add_pass; // mode "--add-user"
    };

    void usage() {
        std::cerr << "Usage: news_server [--port 5555] [--bind 0.0.0.0] [--users users.txt]\n"
                     "                   [--history history.log] [--interval ms (0 = pas de generation)]\n"
                     "       news_server --add-user <nom> <mot_de_passe> [--users users.txt]\n";
    }

    // Retourne false si les arguments sont invalides.
    bool parse_args(int argc, char **argv, Options &opt) {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("valeur manquante pour " + a);
                return argv[++i];
            };
            if (a == "--port") opt.port = std::stoi(next());
            else if (a == "--bind") opt.bind_addr = next();
            else if (a == "--users") opt.users_path = next();
            else if (a == "--history") opt.history_path = next();
            else if (a == "--interval") opt.interval_ms = std::stoi(next());
            else if (a == "--add-user") {
                opt.add_user = next();
                opt.add_pass = next();
            } else return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    // Le serveur
    // ---------------------------------------------------------------------------

    class Server {
    public:
        explicit Server(const Options &opt)
            : users_(opt.users_path),
              store_(opt.history_path),
              listener_(make_listener(opt.bind_addr, opt.port)),
              interval_ms_(opt.interval_ms),
              next_gen_(Clock::now() + std::chrono::milliseconds(opt.interval_ms)) {
            log("ecoute sur " + opt.bind_addr + ":" + std::to_string(opt.port) + ", historique: " +
                std::to_string(store_.last_id()) + " news");
        }

        // Boucle principale : une iteration = un "tour" de traitement.
        void run() {
            while (!g_stop) {
                std::vector<pollfd> pfds = build_pollfds();
                int ready = wait_for_events(pfds);
                generate_news_if_due();
                service_clients(pfds, ready); // pfds[i + 1] correspond a clients_[i]
                remove_dead_clients();
                if (ready > 0 && (pfds[0].revents & POLLIN)) accept_new_clients();
            }
            log("arret propre");
        }

    private:
        // --- Etape 1 : que surveiller ? ------------------------------------------
        std::vector<pollfd> build_pollfds() const {
            std::vector<pollfd> pfds;
            pfds.push_back({listener_.get(), POLLIN, 0});
            for (const Client &c : clients_) {
                // POLLOUT seulement s'il reste des donnees a envoyer, sinon poll() reveillerait en boucle.
                short events = static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT));
                pfds.push_back({c.fd.get(), events, 0});
            }
            return pfds;
        }

        // --- Etape 2 : attendre. Le timeout sert d'horloge au generateur. ---------
        int wait_for_events(std::vector<pollfd> &pfds) {
            int timeout = -1; // pas de generateur : on attend indefiniment un evenement
            if (interval_ms_ > 0) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(next_gen_ - Clock::now());
                timeout = remaining.count() < 0 ? 0 : static_cast<int>(remaining.count()) + 1;
            }
            int ready = ::poll(pfds.data(), pfds.size(), timeout);
            if (ready < 0) {
                if (errno == EINTR) return 0; // signal recu : g_stop sera teste par run()
                throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
            }
            return ready;
        }

        // --- Etape 3 : produire une news si l'intervalle est ecoule ---------------
        void generate_news_if_due() {
            if (interval_ms_ <= 0 || Clock::now() < next_gen_) return;
            store_.append(generate_news(store_.last_id() + 1));
            next_gen_ = Clock::now() + std::chrono::milliseconds(interval_ms_);
        }

        // --- Etape 4 : lire, interpreter, puis envoyer pour chaque client ---------
        void service_clients(const std::vector<pollfd> &pfds, int ready) {
            if (ready > 0) {
                for (size_t i = 0; i < clients_.size(); ++i) {
                    Client &c = clients_[i];
                    short ev = pfds[i + 1].revents;
                    if (ev & (POLLIN | POLLHUP | POLLERR)) read_from(c);
                    if (!c.dead) process_input(c);
                }
            }
            for (Client &c : clients_) {
                if (c.dead) continue;
                pump_news(c);
                flush(c);
            }
        }

        // --- Etape 5 : retirer les clients morts (le destructeur de Fd ferme le socket)
        void remove_dead_clients() {
            for (auto it = clients_.begin(); it != clients_.end();) {
                if (it->dead) {
                    log(it->peer + " deconnecte");
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // --- Etape 6 : accepter les nouvelles connexions ---------------------------
        void accept_new_clients() {
            for (;;) {
                sockaddr_in addr{};
                socklen_t len = sizeof addr;
                Fd fd(::accept(listener_.get(), reinterpret_cast<sockaddr *>(&addr), &len));
                if (!fd.valid()) return; // EAGAIN : plus rien a accepter
                if (!set_nonblocking(fd.get())) continue;
                char ip[INET_ADDRSTRLEN] = "?";
                ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
                Client c;
                c.fd = std::move(fd);
                c.peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
                log(c.peer + " connecte");
                clients_.push_back(std::move(c));
            }
        }

        // --- Protocole -------------------------------------------------------------

        // TCP = flux d'octets : une lecture peut contenir une demi-ligne ou plusieurs
        // lignes. On ne traite que les lignes completes ; le reste attend dans c.in.
        void process_input(Client &c) {
            size_t pos;
            while (!c.close_after_flush && (pos = c.in.find('\n')) != std::string::npos) {
                std::string line = c.in.substr(0, pos);
                c.in.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                handle_command(c, parse_command(line));
            }
            if (c.in.size() > kMaxLine) {
                c.reply_error("line_too_long");
                c.close_after_flush = true;
            }
        }

        // La machine a etats : quelles commandes sont permises dans quel etat.
        void handle_command(Client &c, const Command &cmd) {
            if (cmd.name.empty()) return;
            if (cmd.name == "QUIT") {
                c.close_after_flush = true;
                return;
            }
            switch (c.state) {
            case State::Unauthenticated:
                if (cmd.name == "AUTH") on_auth(c, cmd);
                else if (cmd.name == "SUBSCRIBE") c.reply_error("not_authenticated");
                else c.reply_error("unknown_command");
                break;
            case State::Authenticated:
                if (cmd.name == "SUBSCRIBE") on_subscribe(c, cmd);
                else if (cmd.name == "AUTH") c.reply_error("already_authenticated");
                else c.reply_error("unknown_command");
                break;
            case State::Subscribed:
                if (cmd.name == "SUBSCRIBE") c.reply_error("already_subscribed");
                else if (cmd.name == "AUTH") c.reply_error("already_authenticated");
                else c.reply_error("unknown_command");
                break;
            }
        }

        // AUTH <user> <password> : Unauthenticated -> Authenticated
        void on_auth(Client &c, const Command &cmd) {
            if (users_.check(cmd.arg, cmd.rest)) {
                c.state = State::Authenticated;
                c.reply("OK");
                log(c.peer + " authentifie en tant que '" + cmd.arg + "'");
            } else {
                c.reply_error("auth_failed");
                log(c.peer + " echec d'authentification");
                if (++c.auth_failures >= kMaxAuthFailures) c.close_after_flush = true;
            }
        }

        // SUBSCRIBE <last_id> : Authenticated -> Subscribed
        void on_subscribe(Client &c, const Command &cmd) {
            long long since = -1;
            try {
                size_t used = 0;
                since = std::stoll(cmd.arg, &used);
                if (used != cmd.arg.size()) since = -1;
            } catch (const std::exception &) {
                since = -1;
            }
            if (since < 0) {
                c.reply_error("bad_request");
            } else if (static_cast<uint64_t>(since) > store_.last_id()) {
                // Le client pretend avoir recu plus que ce que le serveur connait
                // (ex. historique serveur perdu) : on refuse plutot que de mentir.
                c.reply_error("bad_since last_id=" + std::to_string(store_.last_id()));
                c.close_after_flush = true;
            } else {
                c.state = State::Subscribed;
                c.next_id = static_cast<uint64_t>(since) + 1;
                c.reply("OK");
                log(c.peer + " abonne, reprise apres id=" + std::to_string(since));
            }
        }

        // Ajoute au tampon d'envoi les news manquantes, par paquets (contre-pression :
        // un client lent ne fait pas gonfler la memoire du serveur).
        void pump_news(Client &c) {
            if (c.state != State::Subscribed || c.close_after_flush) return;
            while (c.next_id <= store_.last_id() && c.out.size() < kOutHighWater) {
                c.reply("NEWS " + std::to_string(c.next_id) + " " + store_.text(c.next_id));
                ++c.next_id;
            }
        }

        // L'ordre de declaration fixe l'ordre d'initialisation (voir le constructeur).
        UserDb users_;
        NewsStore store_;
        Fd listener_;
        int interval_ms_;
        Clock::time_point next_gen_;
        std::vector<Client> clients_;
    };

} // namespace

int main(int argc, char **argv) {
    try {
        Options opt;
        if (!parse_args(argc, argv, opt)) {
            usage();
            return 2;
        }
        if (!opt.add_user.empty()) { // mode administration : pas de serveur
            UserDb::add(opt.users_path, opt.add_user, opt.add_pass);
            std::cout << "Utilisateur '" << opt.add_user << "' ajoute dans " << opt.users_path << std::endl;
            return 0;
        }

        Server server(opt);

        struct sigaction sa{};
        sa.sa_handler = on_signal; // pas de SA_RESTART : poll() est interrompu proprement
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);
        ::signal(SIGPIPE, SIG_IGN);

        server.run();
    } catch (const std::exception &e) {
        std::cerr << "Erreur: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}