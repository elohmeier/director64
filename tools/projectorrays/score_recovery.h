/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. See https://mozilla.org/MPL/2.0/. */
#ifndef PROJECTORRAYS_SCORE_RECOVERY_H
#define PROJECTORRAYS_SCORE_RECOVERY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Director::ScoreRecovery {

struct Range { uint32_t offset = 0, length = 0; };
struct Label { uint16_t frame = 0; Range text, comment; };
struct Delta { uint16_t channelOffset = 0; Range payload; };
struct Channel {
    uint16_t index = 0;
    std::vector<uint8_t> bytes, known;
    std::map<std::string, int64_t> fields;
};
struct Frame {
    uint32_t number = 0;
    Range source;
    std::vector<Delta> deltas;
    std::vector<Channel> changedChannels;
};
struct Recovery {
    std::string fourCC;
    std::vector<uint8_t> raw;
    std::map<std::string, uint32_t> fields;
    std::vector<Range> records;
    std::vector<Label> labels;
    std::vector<Frame> frames;
    std::vector<std::string> unresolved;
    std::string toJSON() const;
};

/* Structural D5/D6/D8 recovery. Throws std::runtime_error on malformed/unsupported input.
 * The output is deliberately not a frozen runtime IR or semantic certification. */
Recovery decode(const std::string &fourCC, const uint8_t *bytes, size_t size, unsigned version);

}
#endif
