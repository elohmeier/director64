/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. See https://mozilla.org/MPL/2.0/. */
#include "score_recovery.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: score-probe VWSC|VWLB|VWFI 500|600|700|800|1000 chunk.bin");
        const std::string version(argv[2]);
        if (version != "500" && version != "600" && version != "700" && version != "800" && version != "1000") throw std::runtime_error("unsupported Director version");
        std::ifstream input(argv[3], std::ios::binary | std::ios::ate);
        if (!input || input.tellg() <= 0 || input.tellg() > 64 * 1024 * 1024)
            throw std::runtime_error("cannot read bounded input chunk");
        input.seekg(0);
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), {}};
        std::cout << Director::ScoreRecovery::decode(argv[1], data.data(), data.size(),
            static_cast<unsigned>(std::stoi(version))).toJSON() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "score-probe: " << error.what() << '\n';
        return 1;
    }
}
