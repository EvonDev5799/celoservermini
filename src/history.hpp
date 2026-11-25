// Historique des news : identifiants sequentiels 1,2,3... conserves en memoire
// ET dans un fichier append-only, relu au demarrage (survie a un crash serveur).
#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

class NewsStore {
public:
    explicit NewsStore(std::string path) : path_(std::move(path)) {
        bool corrupt = false;
        {
            std::ifstream in(path_);
            std::string line;
            while (std::getline(in, line)) {
                // Format d'une ligne : "<id>\t<texte>". Une derniere ligne tronquee
                // (crash pendant l'ecriture) est detectee et ecartee.
                auto tab = line.find('\t');
                if (tab == std::string::npos || in.eof()) { corrupt = true; break; }
                uint64_t id = 0;
                try { id = std::stoull(line.substr(0, tab)); } catch (...) { corrupt = true; break; }
                if (id != items_.size() + 1) { corrupt = true; break; }
                items_.push_back(line.substr(tab + 1));
            }
        }
        if (corrupt) rewrite();  // repart d'un fichier propre
        out_.open(path_, std::ios::app);
        if (!out_) throw std::runtime_error("impossible d'ouvrir l'historique: " + path_);
    }

    uint64_t last_id() const { return items_.size(); }
    const std::string& text(uint64_t id) const { return items_[id - 1]; }

    // Ajoute une news et la persiste (flush vers l'OS : survit a un crash du
    // processus, pas a une coupure de courant -- voir README).
    uint64_t append(std::string text) {
        for (char& c : text) if (c == '\n' || c == '\r') c = ' ';
        items_.push_back(std::move(text));
        const uint64_t id = items_.size();
        out_ << id << '\t' << items_.back() << '\n';
        out_.flush();
        return id;
    }

private:
    void rewrite() {
        const std::string tmp = path_ + ".tmp";
        {
            std::ofstream o(tmp, std::ios::trunc);
            for (size_t i = 0; i < items_.size(); ++i) o << (i + 1) << '\t' << items_[i] << '\n';
        }
        std::rename(tmp.c_str(), path_.c_str());
    }
    std::string path_;
    std::vector<std::string> items_;
    std::ofstream out_;
};
