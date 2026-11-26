// Utilitaires reseau bas niveau.
#pragma once
#include <fcntl.h>
#include <unistd.h>

// Fd = "File Descriptor". Cette classe possede un descripteur de fichier (un
// socket, ici) et le ferme toute seule quand l'objet disparait : c'est le RAII.
// On ne peut donc pas oublier de faire close(), meme en cas d'exception.
//
// Regle d'or : UN SEUL Fd possede un descripteur donne. D'ou :
//   - la copie est interdite (sinon deux objets fermeraient le meme socket) ;
//   - le deplacement est permis : l'ancien proprietaire cede le descripteur.
class Fd {
public:
    static constexpr int kInvalid = -1; // valeur "je ne possede rien"

    Fd() = default;                  // ne possede rien
    explicit Fd(int fd) : fd_(fd) {} // prend possession de fd
    ~Fd() { reset(); }               // ferme le descripteur

    // Copie interdite.
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    // Deplacement : on prend le descripteur de "other", qui n'en possede plus.
    Fd(Fd &&other) noexcept : fd_(other.release()) {}
    Fd &operator=(Fd &&other) noexcept {
        reset(other.release()); // ferme l'ancien descripteur, puis prend le nouveau
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ != kInvalid; }

    // Abandonne la propriete SANS fermer : l'appelant devient responsable du descripteur.
    int release() {
        int old = fd_;
        fd_ = kInvalid;
        return old;
    }

    // Ferme le descripteur actuel (s'il y en a un), puis prend new_fd (par defaut : rien).
    void reset(int new_fd = kInvalid) {
        if (valid()) ::close(fd_);
        fd_ = new_fd;
    }

private:
    int fd_ = kInvalid;
};

// Passe un descripteur en mode non bloquant : recv/send/accept rendent la main
// tout de suite (errno = EAGAIN) au lieu d'attendre. Indispensable pour un
// serveur a un seul thread. Retourne false en cas d'erreur.
inline bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0); // lire les options actuelles
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0; // ajouter O_NONBLOCK
}