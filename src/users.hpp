// Base d'utilisateurs : fichier texte, une ligne "user:salt:sha256(salt+password)".
// Les mots de passe ne sont jamais stockes en clair.
#pragma once
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

#include "sha256.hpp"

class UserDb {
public:
    explicit UserDb(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("impossible d'ouvrir le fichier utilisateurs: " + path);
        std::string line;
        while (std::getline(in, line)) {
            auto p1 = line.find(':');
            if (p1 == std::string::npos) continue;
            auto p2 = line.find(':', p1 + 1);
            if (p2 == std::string::npos) continue;  // ligne mal formee : ignoree
            users_[line.substr(0, p1)] = {line.substr(p1 + 1, p2 - p1 - 1), line.substr(p2 + 1)};
        }
    }

    bool check(const std::string& user, const std::string& password) const {
        auto it = users_.find(user);
        // Meme si l'utilisateur n'existe pas, on calcule un hash pour ne pas
        // reveler son existence par le temps de reponse.
        const bool known = it != users_.end();
        const std::string& salt = known ? it->second.first : kDummy;
        const std::string computed = sha256_hex(salt + password);
        const std::string& expected = known ? it->second.second : kDummy;
        return known && constant_time_equal(computed, expected);
    }

    static void add(const std::string& path, const std::string& user, const std::string& password) {
        if (user.empty() || user.find_first_of(": \t\r\n") != std::string::npos)
            throw std::runtime_error("nom d'utilisateur invalide");
        std::random_device rd;
        static const char* hex = "0123456789abcdef";
        std::string salt;
        for (int i = 0; i < 16; ++i) salt.push_back(hex[rd() % 16]);
        std::ofstream out(path, std::ios::app);
        if (!out) throw std::runtime_error("impossible d'ecrire: " + path);
        out << user << ':' << salt << ':' << sha256_hex(salt + password) << '\n';
    }

private:
    static bool constant_time_equal(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }
    inline static const std::string kDummy = "0000000000000000";
    std::map<std::string, std::pair<std::string, std::string>> users_;  // user -> (salt, hash)
};
