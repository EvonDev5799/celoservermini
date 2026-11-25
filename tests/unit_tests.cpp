// Tests unitaires sans framework : sha256, base utilisateurs, historique.
#include <cstdio>
#include <fstream>
#include <iostream>

#include "../src/history.hpp"
#include "../src/sha256.hpp"
#include "../src/users.hpp"

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "ECHEC " << __FILE__ << ":" << __LINE__ << " " #cond "\n"; \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

int main() {
    // Vecteurs de test officiels NIST
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Utilisateurs
    const std::string upath = "/tmp/newsflow_test_users.txt";
    std::remove(upath.c_str());
    UserDb::add(upath, "alice", "s3cret pass");
    UserDb db(upath);
    CHECK(db.check("alice", "s3cret pass"));
    CHECK(!db.check("alice", "wrong"));
    CHECK(!db.check("bob", "s3cret pass"));
    {
        std::ifstream in(upath);
        std::string content((std::istreambuf_iterator<char>(in)), {});
        CHECK(content.find("s3cret") == std::string::npos);  // pas de mot de passe en clair
    }

    // Historique : persistance, ids sequentiels, ligne finale tronquee
    const std::string hpath = "/tmp/newsflow_test_history.log";
    std::remove(hpath.c_str());
    {
        NewsStore s(hpath);
        CHECK(s.last_id() == 0);
        CHECK(s.append("un") == 1);
        CHECK(s.append("deux\navec saut de ligne") == 2);
    }
    {
        NewsStore s(hpath);  // "redemarrage"
        CHECK(s.last_id() == 2);
        CHECK(s.text(1) == "un");
        CHECK(s.text(2) == "deux avec saut de ligne");
    }
    {
        std::ofstream o(hpath, std::ios::app);
        o << "3\ttronq";  // simule un crash en pleine ecriture (pas de '\n')
    }
    {
        NewsStore s(hpath);
        CHECK(s.last_id() == 2);          // la ligne tronquee est ecartee
        CHECK(s.append("trois") == 3);    // et l'ecriture suivante reste coherente
    }
    {
        NewsStore s(hpath);
        CHECK(s.last_id() == 3);
        CHECK(s.text(3) == "trois");
    }
    std::remove(upath.c_str());
    std::remove(hpath.c_str());

    if (failures == 0) std::cout << "Tous les tests unitaires passent.\n";
    return failures == 0 ? 0 : 1;
}
