# NewsFlow : serveur TCP de news avec authentification et reprise de flux

Serveur et client C++17, sans dépendance externe (STL + sockets POSIX), Linux.

## Prérequis et compilation

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

- `unit_tests` : SHA-256 (vecteurs NIST), authentification, historique (dont fichier tronqué après crash).
- `demo.sh` : scénario automatique (kill -9 du client puis du serveur, vérification d'un flux sans trou).

## Utilisation

    # 1. créer un utilisateur (stocké salé + haché dans users.txt)
    ./build/news_server --add-user alice secret

    # 2. lancer le serveur (une news générée par seconde)
    ./build/news_server --port 5555

    # 3. dans un autre terminal, lancer le client
    ./build/news_client --user alice --pass secret --port 5555

Scénario manuel : `kill -9` le client, attendre quelques secondes, le relancer : il reprend à
l'id suivant le dernier reçu (fichier `last_id.txt`). Même chose en tuant le serveur : le client se
reconnecte tout seul et l'historique (`history.log`) est rechargé.

Options serveur : `--port`, `--bind`, `--users`, `--history`, `--interval <ms>` (0 = pas de génération).
Options client : `--host`, `--port`, `--user`, `--pass`, `--state`.

On peut aussi tester à la main : `nc 127.0.0.1 5555` puis taper `AUTH alice secret` et `SUBSCRIBE 0`.

## Protocole

Texte, une commande par ligne (`\n`, `\r\n` accepté).

| Sens | Message | Rôle |
|---|---|---|
| C -> S | `AUTH <user> <password>` | authentification (3 échecs = déconnexion) |
| C -> S | `SUBSCRIBE <last_id>` | abonnement, rejeu des news d'id > last_id (0 = tout) |
| C -> S | `QUIT` | fermeture propre |
| S -> C | `OK` | commande acceptée |
| S -> C | `ERR <raison>` | refus (`auth_failed`, `not_authenticated`, `bad_since`, ...) |
| S -> C | `NEWS <id> <texte>` | une news ; les ids sont séquentiels (1, 2, 3...) |

## Choix d'architecture

- **Serveur mono-thread avec `poll()`** : pas de verrou ni de data race, comportement déterministe, code
  court à relire. Sockets non bloquants. Le timeout de `poll` sert d'horloge au générateur de news.
  Limite : `poll` est O(n) par itération ; pour des milliers de clients, `epoll` (ou Asio).
- **TCP est un flux d'octets** : chaque client a un tampon d'entrée (lectures partielles, plusieurs
  commandes dans un paquet) et un tampon de sortie (écritures partielles, `POLLOUT`).
- **Contre-pression** : le tampon de sortie d'un client est plafonné (64 Ko) ; le rejeu se fait par
  paquets via un curseur `next_id` par client, donc un client lent ne fait pas exploser la mémoire.
- **RAII** : la classe `Fd` ferme les descripteurs ; aucun `new`/`delete`.
- **Robustesse** : `MSG_NOSIGNAL` + `SIGPIPE` ignoré, ligne max 4 Ko, arrêt propre sur SIGINT/SIGTERM.
- **Authentification** : `users.txt` contient `user:sel:SHA256(sel+mdp)`, jamais le mot de passe en
  clair ; comparaison à temps constant ; même coût de calcul pour un utilisateur inconnu.
- **Reprise après crash du client** : le client mémorise l'id du dernier message traité (écriture
  atomique fichier temporaire + `rename`) et le renvoie dans `SUBSCRIBE`. Reconnexion avec backoff
  exponentiel (0,5 s -> 30 s).
- **Reprise après crash du serveur** : historique append-only (`history.log`) relu au démarrage ; une
  dernière ligne tronquée est écartée. Les ids continuent là où ils s'étaient arrêtés.
- **Sémantique de livraison : at-least-once**. Le client traite puis mémorise : un crash entre les
  deux donne un doublon, jamais une perte. Le client ignore les ids déjà vus (idempotence).

## Limites connues et améliorations

- Pas de TLS : le mot de passe transite en clair sur le réseau (TLS, ou un challenge-response, serait
  la première amélioration). SHA-256 salé n'est pas un hash de mot de passe moderne (préférer argon2/bcrypt).
- Historique non borné en mémoire ; pas de rotation/compaction du fichier. `flush` envoie les données à
  l'OS (survit à un crash du processus) mais sans `fsync` : une coupure de courant peut perdre les
  dernières news.
- IPv4 uniquement côté serveur ; pas de heartbeat applicatif (un lien mort silencieux n'est détecté que
  par TCP keepalive côté client).
- Les news sont générées automatiquement ; un vrai service les recevrait d'un producteur (autre
  commande, stdin, file de messages).
- Un seul fil d'exécution : à grande échelle, `epoll` + pool de threads, ou Boost.Asio.
- Le client affiche les news sur stdout (`id<TAB>texte`) ; pas de plugin de traitement.