/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. See https://mozilla.org/MPL/2.0/.
 * Independently implemented from format observations; see README.md for sources. */
#include "score_recovery.h"
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace Director::ScoreRecovery {
namespace {

class Reader {
    const std::vector<uint8_t> &bytes;
public:
    explicit Reader(const std::vector<uint8_t> &data) : bytes(data) {}
    void range(size_t offset, size_t length) const {
        if (offset > bytes.size() || length > bytes.size() - offset)
            throw std::runtime_error("score range outside chunk");
    }
    uint16_t u16(size_t offset) const {
        range(offset, 2);
        return uint16_t((uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
    }
    uint32_t u32(size_t offset) const {
        range(offset, 4);
        return (uint32_t(u16(offset)) << 16) | u16(offset + 2);
    }
};

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void field(Channel &channel, const char *name, unsigned offset, unsigned length, bool signedValue = false) {
    uint64_t value = 0;
    for (unsigned i = 0; i < length; i++) {
        if (!channel.known[offset + i]) return;
        value = (value << 8) | channel.bytes[offset + i];
    }
    int64_t result = static_cast<int64_t>(value);
    if (signedValue && (value & (uint64_t(1) << (length * 8 - 1))))
        result -= int64_t(1) << (length * 8);
    channel.fields[name] = result;
}

void describe(Channel &channel, unsigned version) {
    if (version == 500) {
        if (channel.index >= 2) {
            field(channel, "sprite_type", 0, 1);
            field(channel, "ink_flags", 1, 1);
            field(channel, "cast_library", 2, 2, true);
            field(channel, "cast_member", 4, 2);
            field(channel, "script_cast_library", 6, 2, true);
            field(channel, "script_member", 8, 2);
            field(channel, "foreground_raw", 10, 1);
            field(channel, "background_raw", 11, 1);
            field(channel, "loc_v", 12, 2, true);
            field(channel, "loc_h", 14, 2, true);
            field(channel, "height", 16, 2, true);
            field(channel, "width", 18, 2, true);
            field(channel, "color_flags", 20, 1);
            field(channel, "blend_raw", 21, 1);
            field(channel, "thickness_raw", 22, 1);
        } else if (channel.index == 0) {
            field(channel, "script_cast_library", 0, 2, true);
            field(channel, "script_member", 2, 2);
            field(channel, "sound1_cast_library", 4, 2, true);
            field(channel, "sound1_member", 6, 2);
            field(channel, "sound2_cast_library", 8, 2, true);
            field(channel, "sound2_member", 10, 2);
            field(channel, "transition_cast_library", 12, 2, true);
            field(channel, "transition_member", 14, 2);
            field(channel, "tempo_raw", 21, 1);
        } else {
            field(channel, "cast_library", 0, 2, true);
            field(channel, "cast_member", 2, 2, true);
        }
        return;
    }
    if (channel.index >= 6) {
        field(channel, "sprite_type", 0, 1);
        field(channel, "ink_flags", 1, 1);
        field(channel, "foreground_raw", 2, 1);
        field(channel, "background_raw", 3, 1);
        field(channel, "cast_library", 4, 2, true);
        field(channel, "cast_member", 6, 2);
        field(channel, "detail_index", 8, 4);
        field(channel, "loc_v", 12, 2, true);
        field(channel, "loc_h", 14, 2, true);
        field(channel, "height", 16, 2, true);
        field(channel, "width", 18, 2, true);
        field(channel, "color_flags", 20, 1);
        field(channel, "blend_raw", 21, 1);
        field(channel, "thickness_raw", 22, 1);
        field(channel, "reserved_raw", 23, 1);
        if (channel.bytes.size() == 48) {
            field(channel, "foreground_green_raw", 24, 1);
            field(channel, "background_green_raw", 25, 1);
            field(channel, "foreground_blue_raw", 26, 1);
            field(channel, "background_blue_raw", 27, 1);
            field(channel, "rotation_raw", 28, 4, true);
            field(channel, "skew_raw", 32, 4, true);
        }
    } else if (channel.index == 1) {
        field(channel, "detail_index", 0, 4);
        field(channel, "cue_point_raw", 4, 2, true);
        field(channel, "tempo_opcode", 6, 1);
        field(channel, "color_code_raw", 7, 1);
    } else if (channel.index == 5) {
        field(channel, "cast_library", 0, 2, true);
        field(channel, "cast_member", 2, 2);
        field(channel, "speed_raw", 4, 1);
        field(channel, "flags_raw", 5, 1);
        field(channel, "first_color_raw", 6, 1);
        field(channel, "last_color_raw", 7, 1);
        field(channel, "frame_count", 8, 2);
        field(channel, "cycle_count", 10, 2);
        field(channel, "detail_index", 16, 4);
    } else {
        field(channel, "cast_library", 0, 2, true);
        field(channel, "cast_member", 2, 2);
        field(channel, "detail_index", 4, 4);
        field(channel, "color_code_raw", 8, 1);
    }
}

void score(Recovery &result, const Reader &reader, unsigned version) {
    const uint32_t declaredLength = reader.u32(0);
    require(version == 500 ? declaredLength >= 20 && declaredLength <= result.raw.size()
        : declaredLength == result.raw.size(), "VWSC declared chunk length mismatch");
    if (version == 500) {
        result.records.push_back({0, declaredLength});
        result.fields = {{"unindexed_tail_offset", declaredLength},
            {"unindexed_tail_length", uint32_t(result.raw.size() - declaredLength)}};
    } else {
    require(reader.u32(4) == 0xfffffffdU, "unsupported VWSC envelope version");
    uint32_t list = reader.u32(8);
    require(list >= 12, "VWSC index overlaps envelope");
    reader.range(list, 12);
    uint32_t entries = reader.u32(list), slots = reader.u32(list + 4);
    uint32_t maxDataLength = reader.u32(list + 8);
    // The index allocates at least one slot beyond the entry count; slot
    // [entries] is the closing fencepost that sizes the final record. The
    // reference reader never fetches that record, but authored data (behavior
    // initializers referencing index entries-1) lives there in the recovered
    // corpora, so entry i spans offsets[i]..offsets[i+1] for all i < entries.
    require(entries >= 2 && slots > entries && slots <= (result.raw.size() - list - 12) / 4,
        "invalid VWSC offset counts");
    uint32_t index = list + 12, base = index + slots * 4;
    require(maxDataLength <= result.raw.size() - base, "VWSC data capacity outside chunk");
    std::vector<uint32_t> offsets;
    for (uint32_t i = 0; i <= entries; i++) {
        uint32_t offset = reader.u32(index + i * 4);
        require(offset <= maxDataLength && (offsets.empty() || offset >= offsets.back()),
            "VWSC invalid active offset");
        offsets.push_back(offset);
    }
    for (uint32_t i = 0; i < entries; i++)
        result.records.push_back({base + offsets[i], offsets[i + 1] - offsets[i]});
    result.fields = {{"list_offset", list}, {"index_entries", entries}, {"index_slots", slots},
        {"data_offset", base}, {"data_capacity", maxDataLength},
        {"unindexed_tail_offset", base + offsets.back()},
        {"unindexed_tail_length", uint32_t(result.raw.size() - base - offsets.back())}};
    }
    const Range &header = result.records.front();
    reader.range(header.offset, 20);
    require(header.length >= 20, "VWSC score header truncated");
    uint32_t length = reader.u32(header.offset), firstFrame = reader.u32(header.offset + 4);
    uint32_t declaredFrames = reader.u32(header.offset + 8);
    uint16_t format = reader.u16(header.offset + 12), recordSize = reader.u16(header.offset + 14);
    uint16_t channels = reader.u16(header.offset + 16), reserved = reader.u16(header.offset + 18);
    // D8 local media declares 1006 allocated channels (six main channels plus
    // 1000 sprites). Keep allocation distinct from the displayed channel field.
    // D10 (MX 2004) retains the D7/D8 layout: every VWSC and film-loop SCVW in
    // the recovered corpus declares format 13, 48-byte records and 1006 channels.
    require(version == 500 ? format == 7 && recordSize == 24 && channels == 50
        : version == 600 ? format == 11 && recordSize == 24 && channels >= 6 && channels <= 126
        : format == 13 && recordSize == 48 && channels >= 6 && channels <= 1006,
        "unsupported versioned score channel layout");
    require(firstFrame >= 20 && firstFrame <= length && length <= header.length,
        "VWSC invalid frame stream range");
    result.fields.insert({{"score_length", length}, {"frame_offset", firstFrame},
        {"declared_frame_count", declaredFrames}, {"frames_version", format},
        {"channel_record_size", recordSize}, {"channel_count", channels}, {"header_reserved", reserved}});
    std::vector<uint8_t> state(size_t(channels) * recordSize, 0), known(state.size(), 0);
    size_t deltaCount = 0, snapshotCount = 0;
    uint32_t position = header.offset + firstFrame, end = header.offset + length;
    while (position < end) {
        require(result.frames.size() < 65536, "VWSC frame expansion budget exceeded");
        require(end - position >= 2, "VWSC truncated frame size");
        uint16_t frameLength = reader.u16(position);
        require(frameLength >= 2 && frameLength <= end - position, "VWSC frame outside score stream");
        Frame frame;
        frame.number = uint32_t(result.frames.size() + 1);
        frame.source = {position, frameLength};
        uint32_t frameEnd = position + frameLength;
        position += 2;
        std::vector<bool> touched(channels, false);
        while (position < frameEnd) {
            require(frameEnd - position >= 4, "VWSC truncated delta header");
            uint16_t size = reader.u16(position), destination = reader.u16(position + 2);
            position += 4;
            require(size != 0 && size <= frameEnd - position, "VWSC invalid delta length");
            require(destination <= state.size() && size <= state.size() - destination,
                "VWSC delta outside channel state");
            require(++deltaCount <= 262144, "VWSC delta expansion budget exceeded");
            frame.deltas.push_back({destination, {position, size}});
            for (uint32_t i = 0; i < size; i++) {
                state[destination + i] = result.raw[position + i];
                known[destination + i] = 1;
                touched[(destination + i) / recordSize] = true;
            }
            position += size;
        }
        for (uint16_t i = 0; i < channels; i++) if (touched[i]) {
            require(++snapshotCount <= 262144, "VWSC channel expansion budget exceeded");
            Channel channel;
            channel.index = i;
            channel.bytes.assign(state.begin() + i * recordSize, state.begin() + (i + 1) * recordSize);
            channel.known.assign(known.begin() + i * recordSize, known.begin() + (i + 1) * recordSize);
            describe(channel, version);
            frame.changedChannels.push_back(std::move(channel));
        }
        result.frames.push_back(std::move(frame));
    }
    result.fields["computed_frame_count"] = uint32_t(result.frames.size());
    if (result.frames.size() != declaredFrames) result.unresolved.push_back("declared-frame-count-disagrees");
    if (result.fields["unindexed_tail_length"]) result.unresolved.push_back("unindexed-score-tail");
    result.unresolved.insert(result.unresolved.end(), {"detail-record-semantics", "initial-channel-defaults",
        "palette-ink-tempo-and-script-semantics", "reference-comparison"});
}

void labels(Recovery &result, const Reader &reader) {
    uint32_t count = reader.u16(0), records = count + 1;
    reader.range(2, records * 4);
    uint32_t base = 2 + records * 4;
    uint32_t previous = reader.u16(4);
    require(previous <= result.raw.size() - base, "VWLB first string outside chunk");
    for (uint32_t i = 0; i < count; i++) {
        uint32_t next = reader.u16(2 + (i + 1) * 4 + 2);
        require(next >= previous && next <= result.raw.size() - base, "VWLB invalid string offsets");
        Label label;
        label.frame = reader.u16(2 + i * 4);
        uint32_t split = previous;
        while (split < next && result.raw[base + split] != 13) split++;
        label.text = {base + previous, split - previous};
        uint32_t commentStart = split < next ? split + 1 : split;
        label.comment = {base + commentStart, next - commentStart};
        result.labels.push_back(label);
        result.records.push_back({base + previous, next - previous});
        previous = next;
    }
    result.fields = {{"label_count", count}, {"string_data_offset", base},
        {"sentinel_frame", reader.u16(2 + count * 4)}, {"trailing_length", uint32_t(result.raw.size() - base - previous)}};
    result.unresolved = {"source-text-encoding", "reference-comparison"};
}

void fileInfo(Recovery &result, const Reader &reader) {
    uint32_t list = reader.u32(0);
    require(list >= 20, "VWFI list overlaps header");
    reader.range(list, 2);
    uint32_t count = reader.u16(list);
    result.fields = {{"list_offset", list}, {"unknown_1", reader.u32(4)},
        {"unknown_2", reader.u32(8)}, {"flags_raw", reader.u32(12)},
        {"script_id", reader.u32(16)}, {"entry_count", count}};
    if (count) {
        reader.range(list + 2, (count + 1) * 4);
        uint32_t base = list + 2 + (count + 1) * 4;
        uint32_t previous = reader.u32(list + 2);
        require(previous <= result.raw.size() - base, "VWFI first entry outside chunk");
        for (uint32_t i = 0; i < count; i++) {
            uint32_t next = reader.u32(list + 2 + (i + 1) * 4);
            require(next >= previous && next <= result.raw.size() - base, "VWFI invalid entry offsets");
            result.records.push_back({base + previous, next - previous});
            previous = next;
        }
        result.fields["data_offset"] = base;
        result.fields["trailing_length"] = uint32_t(result.raw.size() - base - previous);
        if (count >= 5 && result.records[4].length >= 2)
            result.fields["preload_raw"] = reader.u16(result.records[4].offset);
    }
    result.unresolved = {"file-info-flags-and-extra-entries", "source-text-encoding", "reference-comparison"};
}

std::string hex(const uint8_t *data, size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 15]);
    }
    return result;
}

void rangeJSON(std::ostream &out, const Range &range, const std::vector<uint8_t> &raw) {
    out << "{\"offset\":" << range.offset << ",\"length\":" << range.length
        << ",\"hex\":\"" << hex(raw.data() + range.offset, range.length) << "\"}";
}

template<typename T> void fieldsJSON(std::ostream &out, const std::map<std::string, T> &fields) {
    out << '{';
    bool first = true;
    for (const auto &entry : fields) {
        if (!first) out << ',';
        first = false;
        out << '"' << entry.first << "\":" << entry.second;
    }
    out << '}';
}
}

Recovery decode(const std::string &fourCC, const uint8_t *bytes, size_t size, unsigned version) {
    require(version == 500 || version == 600 || version == 700 || version == 800 || version == 1000,
        "score recovery supports Director 5, 6, 7, 8 and 10 only");
    require(bytes != nullptr && size > 0 && size <= 64 * 1024 * 1024, "invalid score chunk size");
    Recovery result;
    result.fourCC = fourCC;
    result.raw.assign(bytes, bytes + size);
    Reader reader(result.raw);
    if (fourCC == "VWSC") score(result, reader, version);
    else if (fourCC == "VWLB") labels(result, reader);
    else if (fourCC == "VWFI") fileInfo(result, reader);
    else throw std::runtime_error("unsupported score chunk type");
    return result;
}

std::string Recovery::toJSON() const {
    std::ostringstream out;
    out << "{\"schema_version\":1,\"stage\":\"structural-score-spike\","
        "\"runtime_ir_frozen\":false,\"semantic_complete\":false,\"reference_verified\":false,"
        "\"fourcc\":\"" << fourCC << "\",\"fields\":";
    fieldsJSON(out, fields);
    out << ",\"raw_hex\":\"" << hex(raw.data(), raw.size()) << "\",\"records\":[";
    for (size_t i = 0; i < records.size(); i++) {
        if (i) out << ',';
        rangeJSON(out, records[i], raw);
    }
    out << "],\"labels\":[";
    for (size_t i = 0; i < labels.size(); i++) {
        if (i) out << ',';
        out << "{\"frame\":" << labels[i].frame << ",\"text\":";
        rangeJSON(out, labels[i].text, raw);
        out << ",\"comment\":";
        rangeJSON(out, labels[i].comment, raw);
        out << '}';
    }
    out << "],\"frames\":[";
    for (size_t i = 0; i < frames.size(); i++) {
        if (i) out << ',';
        const Frame &frame = frames[i];
        out << "{\"number\":" << frame.number << ",\"source\":";
        rangeJSON(out, frame.source, raw);
        out << ",\"deltas\":[";
        for (size_t j = 0; j < frame.deltas.size(); j++) {
            if (j) out << ',';
            out << "{\"channel_offset\":" << frame.deltas[j].channelOffset << ",\"payload\":";
            rangeJSON(out, frame.deltas[j].payload, raw);
            out << '}';
        }
        out << "],\"changed_channels\":[";
        for (size_t j = 0; j < frame.changedChannels.size(); j++) {
            if (j) out << ',';
            const Channel &channel = frame.changedChannels[j];
            out << "{\"index\":" << channel.index << ",\"record_hex\":\""
                << hex(channel.bytes.data(), channel.bytes.size()) << "\",\"known_mask_hex\":\""
                << hex(channel.known.data(), channel.known.size()) << "\",\"fields\":";
            fieldsJSON(out, channel.fields);
            out << '}';
        }
        out << "]}";
    }
    out << "],\"unresolved\":[";
    for (size_t i = 0; i < unresolved.size(); i++) {
        if (i) out << ',';
        out << '"' << unresolved[i] << '"';
    }
    out << "]}";
    return out.str();
}
}
