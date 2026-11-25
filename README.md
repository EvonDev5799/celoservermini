# NewsFlow : serveur TCP de news avec authentification et reprise de flux

Serveur et client C++17, sans dependance externe (STL + sockets POSIX), Linux.

## Prerequis et compilation

Compilateur C++17 (GCC >= 7 ou Clang >= 6). Testé avec GCC, `-Wall -Wextra -Wpedantic` sans warning,
et avec `-fsanitize=address,undefined`.

    # Debian/Ubuntu
    sudo apt install build-essential cmake
    # Fedora
    sudo dnf install gcc-c++ cmake make

    # Option A : CMake
    cmake -S . -B build && cmake --build build
    # Option B : sans CMake
    make

Les binaires sont dans `build/` : `news_server`, `news_client`, `unit_tests`.

## Tests

    ctest --test-dir build --output-on-failure     # ou : make test

- `unit_tests` : SHA-256 (vecteurs NIST), authentification, historique (dont fichier tronque apres crash).
- `demo.sh` : scenario automatique (kill -9 du client puis du serveur, verification d'un flux sans trou).

## Utilisation

    # 1. creer un utilisateur (stocke sale + hache dans users.txt)
    ./build/news_server --add-user alice secret

    # 2. lancer le serveur (une news generee par seconde)
    ./build/news_server --port 5555

    # 3. dans un autre terminal, lancer le client
    ./build/news_client --user alice --pass secret --port 5555

Scenario manuel : `kill -9` le client, attendre quelques secondes, le relancer : il reprend a
l'id suivant le dernier recu (fichier `last_id.txt`). Meme chose en tuant le serveur : le client se
reconnecte tout seul et l'historique (`history.log`) est recharge.

Options serveur : `--port`, `--bind`, `--users`, `--history`, `--interval <ms>` (0 = pas de generation).
Options client : `--host`, `--port`, `--user`, `--pass`, `--state`.

On peut aussi tester a la main : `nc 127.0.0.1 5555` puis taper `AUTH alice secret` et `SUBSCRIBE 0`.

## Protocole

Texte, une commande par ligne (`\n`, `\r\n` accepte).

| Sens | Message | Role |
|---|---|---|
| C -> S | `AUTH <user> <password>` | authentification (3 echecs = deconnexion) |
| C -> S | `SUBSCRIBE <last_id>` | abonnement, rejeu des news d'id > last_id (0 = tout) |
| C -> S | `QUIT` | fermeture propre |
| S -> C | `OK` | commande acceptee |
| S -> C | `ERR <raison>` | refus (`auth_failed`, `not_authenticated`, `bad_since`, ...) |
| S -> C | `NEWS <id> <texte>` | une news ; les ids sont sequentiels (1, 2, 3...) |

## Choix d'architecture

- **Serveur mono-thread avec `poll()`** : pas de verrou ni de data race, comportement deterministe, code
  court a relire. Sockets non bloquants. Le timeout de `poll` sert d'horloge au generateur de news.
  Limite : `poll` est O(n) par iteration ; pour des milliers de clients, `epoll` (ou Asio).
- **TCP est un flux d'octets** : chaque client a un tampon d'entree (lectures partielles, plusieurs
  commandes dans un paquet) et un tampon de sortie (ecritures partielles, `POLLOUT`).
- **Contre-pression** : le tampon de sortie d'un client est plafonne (64 Ko) ; le rejeu se fait par
  paquets via un curseur `next_id` par client, donc un client lent ne fait pas exploser la memoire.
- **RAII** : la classe `Fd` ferme les descripteurs ; aucun `new`/`delete`.
- **Robustesse** : `MSG_NOSIGNAL` + `SIGPIPE` ignore, ligne max 4 Ko, arret propre sur SIGINT/SIGTERM.
- **Authentification** : `users.txt` contient `user:sel:SHA256(sel+mdp)`, jamais le mot de passe en
  clair ; comparaison a temps constant ; meme cout de calcul pour un utilisateur inconnu.
- **Reprise apres crash du client** : le client memorise l'id du dernier message traite (ecriture
  atomique fichier temporaire + `rename`) et le renvoie dans `SUBSCRIBE`. Reconnexion avec backoff
  exponentiel (0,5 s -> 30 s).
- **Reprise apres crash du serveur** : historique append-only (`history.log`) relu au demarrage ; une
  derniere ligne tronquee est ecartee. Les ids continuent la ou ils s'etaient arretes.
- **Semantique de livraison : at-least-once**. Le client traite puis memorise : un crash entre les
  deux donne un doublon, jamais une perte. Le client ignore les ids deja vus (idempotence).

## Limites connues et ameliorations

- Pas de TLS : le mot de passe transite en clair sur le reseau (TLS, ou un challenge-response, serait
  la premiere amelioration). SHA-256 sale n'est pas un hash de mot de passe moderne (preferer argon2/bcrypt).
- Historique non borne en memoire ; pas de rotation/compaction du fichier. `flush` envoie les donnees a
  l'OS (survit a un crash du processus) mais sans `fsync` : une coupure de courant peut perdre les
  dernieres news.
- IPv4 uniquement cote serveur ; pas de heartbeat applicatif (un lien mort silencieux n'est detecte que
  par TCP keepalive cote client).
- Les news sont generees automatiquement ; un vrai service les recevrait d'un producteur (autre
  commande, stdin, file de messages).
- Un seul fil d'execution : a grande echelle, `epoll` + pool de threads, ou Boost.Asio.
- Le client affiche les news sur stdout (`id<TAB>texte`) ; pas de plugin de traitement.
