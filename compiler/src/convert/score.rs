// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. See https://mozilla.org/MPL/2.0/. It is a port of
// tools/projectorrays/score_recovery.cpp, which carries the same license.

//! Structural score recovery: the Rust port of
//! tools/projectorrays/score_recovery.cpp (the pinned ProjectorRays
//! extension). Bounded decoding of VWSC score streams, VWLB labels and VWFI
//! file info; the JSON is the probe's, byte for byte. The output is
//! deliberately not a frozen runtime IR or a semantic certification.

use std::collections::BTreeMap;

use super::js::hex;

#[derive(Clone, Copy)]
struct Range {
    offset: u32,
    length: u32,
}
struct Label {
    frame: u16,
    text: Range,
    comment: Range,
}
struct Delta {
    channel_offset: u16,
    payload: Range,
}
struct Channel {
    index: u16,
    bytes: Vec<u8>,
    known: Vec<u8>,
    fields: BTreeMap<&'static str, i64>,
}
struct Frame {
    number: u32,
    source: Range,
    deltas: Vec<Delta>,
    changed: Vec<Channel>,
}

struct Recovery<'a> {
    fourcc: &'a str,
    raw: &'a [u8],
    fields: BTreeMap<&'static str, u32>,
    records: Vec<Range>,
    labels: Vec<Label>,
    frames: Vec<Frame>,
    unresolved: Vec<&'static str>,
}

/// `size_t` subtraction in the C++ reference: wraps rather than failing.
fn wide(a: u32, b: u32) -> u64 {
    (a as u64).wrapping_sub(b as u64)
}

fn require(condition: bool, message: &str) -> Result<(), String> {
    if condition { Ok(()) } else { Err(message.to_string()) }
}

impl Recovery<'_> {
    fn range(&self, offset: usize, length: usize) -> Result<(), String> {
        require(offset <= self.raw.len() && length <= self.raw.len() - offset, "score range outside chunk")
    }
    fn u16(&self, offset: usize) -> Result<u16, String> {
        self.range(offset, 2)?;
        Ok(u16::from_be_bytes([self.raw[offset], self.raw[offset + 1]]))
    }
    fn u32(&self, offset: usize) -> Result<u32, String> {
        Ok((self.u16(offset)? as u32) << 16 | self.u16(offset + 2)? as u32)
    }
}

fn field(channel: &mut Channel, name: &'static str, offset: usize, length: usize, signed: bool) {
    let mut value: u64 = 0;
    for i in 0..length {
        if channel.known[offset + i] == 0 {
            return;
        }
        value = value << 8 | channel.bytes[offset + i] as u64;
    }
    let mut result = value as i64;
    if signed && value & (1u64 << (length * 8 - 1)) != 0 {
        result -= 1i64 << (length * 8);
    }
    channel.fields.insert(name, result);
}

fn describe(c: &mut Channel, version: u32) {
    if version == 500 {
        if c.index >= 2 {
            for (name, offset, length, signed) in [
                ("sprite_type", 0, 1, false), ("ink_flags", 1, 1, false), ("cast_library", 2, 2, true),
                ("cast_member", 4, 2, false), ("script_cast_library", 6, 2, true), ("script_member", 8, 2, false),
                ("foreground_raw", 10, 1, false), ("background_raw", 11, 1, false), ("loc_v", 12, 2, true),
                ("loc_h", 14, 2, true), ("height", 16, 2, true), ("width", 18, 2, true), ("color_flags", 20, 1, false),
                ("blend_raw", 21, 1, false), ("thickness_raw", 22, 1, false),
            ] {
                field(c, name, offset, length, signed);
            }
        } else if c.index == 0 {
            for (name, offset, length, signed) in [
                ("script_cast_library", 0, 2, true), ("script_member", 2, 2, false),
                ("sound1_cast_library", 4, 2, true), ("sound1_member", 6, 2, false),
                ("sound2_cast_library", 8, 2, true), ("sound2_member", 10, 2, false),
                ("transition_cast_library", 12, 2, true), ("transition_member", 14, 2, false),
                ("tempo_raw", 21, 1, false),
            ] {
                field(c, name, offset, length, signed);
            }
        } else {
            field(c, "cast_library", 0, 2, true);
            field(c, "cast_member", 2, 2, true);
        }
        return;
    }
    if c.index >= 6 {
        for (name, offset, length, signed) in [
            ("sprite_type", 0, 1, false), ("ink_flags", 1, 1, false), ("foreground_raw", 2, 1, false),
            ("background_raw", 3, 1, false), ("cast_library", 4, 2, true), ("cast_member", 6, 2, false),
            ("detail_index", 8, 4, false), ("loc_v", 12, 2, true), ("loc_h", 14, 2, true), ("height", 16, 2, true),
            ("width", 18, 2, true), ("color_flags", 20, 1, false), ("blend_raw", 21, 1, false),
            ("thickness_raw", 22, 1, false), ("reserved_raw", 23, 1, false),
        ] {
            field(c, name, offset, length, signed);
        }
        if c.bytes.len() == 48 {
            for (name, offset, length, signed) in [
                ("foreground_green_raw", 24, 1, false), ("background_green_raw", 25, 1, false),
                ("foreground_blue_raw", 26, 1, false), ("background_blue_raw", 27, 1, false),
                ("rotation_raw", 28, 4, true), ("skew_raw", 32, 4, true),
            ] {
                field(c, name, offset, length, signed);
            }
        }
    } else if c.index == 1 {
        for (name, offset, length, signed) in [
            ("detail_index", 0, 4, false), ("cue_point_raw", 4, 2, true), ("tempo_opcode", 6, 1, false),
            ("color_code_raw", 7, 1, false),
        ] {
            field(c, name, offset, length, signed);
        }
    } else if c.index == 5 {
        for (name, offset, length, signed) in [
            ("cast_library", 0, 2, true), ("cast_member", 2, 2, false), ("speed_raw", 4, 1, false),
            ("flags_raw", 5, 1, false), ("first_color_raw", 6, 1, false), ("last_color_raw", 7, 1, false),
            ("frame_count", 8, 2, false), ("cycle_count", 10, 2, false), ("detail_index", 16, 4, false),
        ] {
            field(c, name, offset, length, signed);
        }
    } else {
        for (name, offset, length, signed) in [
            ("cast_library", 0, 2, true), ("cast_member", 2, 2, false), ("detail_index", 4, 4, false),
            ("color_code_raw", 8, 1, false),
        ] {
            field(c, name, offset, length, signed);
        }
    }
}

fn score(r: &mut Recovery, version: u32) -> Result<(), String> {
    let size = r.raw.len() as u32;
    let declared = r.u32(0)?;
    require(
        if version == 500 { declared >= 20 && declared <= size } else { declared == size },
        "VWSC declared chunk length mismatch",
    )?;
    if version == 500 {
        r.records.push(Range { offset: 0, length: declared });
        r.fields = BTreeMap::from([("unindexed_tail_offset", declared), ("unindexed_tail_length", size - declared)]);
    } else {
        require(r.u32(4)? == 0xfffffffd, "unsupported VWSC envelope version")?;
        let list = r.u32(8)?;
        require(list >= 12, "VWSC index overlaps envelope")?;
        r.range(list as usize, 12)?;
        let entries = r.u32(list as usize)?;
        let slots = r.u32(list as usize + 4)?;
        let max_data = r.u32(list as usize + 8)?;
        require(
            entries >= 2 && slots > entries && (slots as u64) <= (size as u64 - list as u64 - 12) / 4,
            "invalid VWSC offset counts",
        )?;
        let index = list + 12;
        let base = index + slots * 4;
        require(max_data as u64 <= wide(size, base), "VWSC data capacity outside chunk")?;
        let mut offsets: Vec<u32> = Vec::new();
        for i in 0..=entries {
            let offset = r.u32((index + i * 4) as usize)?;
            require(offset <= max_data && offsets.last().is_none_or(|&last| offset >= last), "VWSC invalid active offset")?;
            offsets.push(offset);
        }
        for i in 0..entries as usize {
            r.records.push(Range { offset: base + offsets[i], length: offsets[i + 1] - offsets[i] });
        }
        let last = *offsets.last().unwrap();
        r.fields = BTreeMap::from([
            ("list_offset", list), ("index_entries", entries), ("index_slots", slots), ("data_offset", base),
            ("data_capacity", max_data), ("unindexed_tail_offset", base + last),
            ("unindexed_tail_length", size.wrapping_sub(base).wrapping_sub(last)),
        ]);
    }
    let header = r.records[0];
    r.range(header.offset as usize, 20)?;
    require(header.length >= 20, "VWSC score header truncated")?;
    let h = header.offset as usize;
    let length = r.u32(h)?;
    let first = r.u32(h + 4)?;
    let declared_frames = r.u32(h + 8)?;
    let format = r.u16(h + 12)?;
    let record_size = r.u16(h + 14)?;
    let channels = r.u16(h + 16)?;
    let reserved = r.u16(h + 18)?;
    require(
        match version {
            500 => format == 7 && record_size == 24 && channels == 50,
            600 => format == 11 && record_size == 24 && (6..=126).contains(&channels),
            _ => format == 13 && record_size == 48 && (6..=1006).contains(&channels),
        },
        "unsupported versioned score channel layout",
    )?;
    require(first >= 20 && first <= length && length <= header.length, "VWSC invalid frame stream range")?;
    for (key, value) in [
        ("score_length", length), ("frame_offset", first), ("declared_frame_count", declared_frames),
        ("frames_version", format as u32), ("channel_record_size", record_size as u32),
        ("channel_count", channels as u32), ("header_reserved", reserved as u32),
    ] {
        r.fields.entry(key).or_insert(value);
    }
    let record_size = record_size as usize;
    let mut state = vec![0u8; channels as usize * record_size];
    let mut known = vec![0u8; state.len()];
    let (mut delta_count, mut snapshot_count) = (0usize, 0usize);
    let mut position = header.offset + first;
    let end = header.offset + length;
    while position < end {
        require(r.frames.len() < 65536, "VWSC frame expansion budget exceeded")?;
        require(end - position >= 2, "VWSC truncated frame size")?;
        let frame_length = r.u16(position as usize)? as u32;
        require(frame_length >= 2 && frame_length <= end - position, "VWSC frame outside score stream")?;
        let mut frame = Frame {
            number: r.frames.len() as u32 + 1,
            source: Range { offset: position, length: frame_length },
            deltas: Vec::new(),
            changed: Vec::new(),
        };
        let frame_end = position + frame_length;
        position += 2;
        let mut touched = vec![false; channels as usize];
        while position < frame_end {
            require(frame_end - position >= 4, "VWSC truncated delta header")?;
            let size = r.u16(position as usize)? as u32;
            let destination = r.u16(position as usize + 2)? as u32;
            position += 4;
            require(size != 0 && size <= frame_end - position, "VWSC invalid delta length")?;
            require(
                destination as usize <= state.len() && size as usize <= state.len() - destination as usize,
                "VWSC delta outside channel state",
            )?;
            delta_count += 1;
            require(delta_count <= 262144, "VWSC delta expansion budget exceeded")?;
            frame.deltas.push(Delta { channel_offset: destination as u16, payload: Range { offset: position, length: size } });
            for i in 0..size as usize {
                state[destination as usize + i] = r.raw[position as usize + i];
                known[destination as usize + i] = 1;
                touched[(destination as usize + i) / record_size] = true;
            }
            position += size;
        }
        for i in 0..channels as usize {
            if !touched[i] {
                continue;
            }
            snapshot_count += 1;
            require(snapshot_count <= 262144, "VWSC channel expansion budget exceeded")?;
            let mut channel = Channel {
                index: i as u16,
                bytes: state[i * record_size..(i + 1) * record_size].to_vec(),
                known: known[i * record_size..(i + 1) * record_size].to_vec(),
                fields: BTreeMap::new(),
            };
            describe(&mut channel, version);
            frame.changed.push(channel);
        }
        r.frames.push(frame);
    }
    r.fields.insert("computed_frame_count", r.frames.len() as u32);
    if r.frames.len() as u32 != declared_frames {
        r.unresolved.push("declared-frame-count-disagrees");
    }
    if r.fields.get("unindexed_tail_length").copied().unwrap_or(0) != 0 {
        r.unresolved.push("unindexed-score-tail");
    }
    r.unresolved.extend([
        "detail-record-semantics", "initial-channel-defaults", "palette-ink-tempo-and-script-semantics",
        "reference-comparison",
    ]);
    Ok(())
}

fn labels(r: &mut Recovery) -> Result<(), String> {
    let size = r.raw.len() as u32;
    let count = r.u16(0)? as u32;
    let records = count + 1;
    r.range(2, records as usize * 4)?;
    let base = 2 + records * 4;
    let mut previous = r.u16(4)? as u32;
    require(previous as u64 <= wide(size, base), "VWLB first string outside chunk")?;
    for i in 0..count {
        let next = r.u16((2 + (i + 1) * 4 + 2) as usize)? as u32;
        require(next >= previous && next as u64 <= wide(size, base), "VWLB invalid string offsets")?;
        let frame = r.u16((2 + i * 4) as usize)?;
        let mut split = previous;
        while split < next && r.raw[(base + split) as usize] != 13 {
            split += 1;
        }
        let comment_start = if split < next { split + 1 } else { split };
        r.labels.push(Label {
            frame,
            text: Range { offset: base + previous, length: split - previous },
            comment: Range { offset: base + comment_start, length: next - comment_start },
        });
        r.records.push(Range { offset: base + previous, length: next - previous });
        previous = next;
    }
    let sentinel = r.u16((2 + count * 4) as usize)? as u32;
    r.fields = BTreeMap::from([
        ("label_count", count), ("string_data_offset", base), ("sentinel_frame", sentinel),
        ("trailing_length", size.wrapping_sub(base).wrapping_sub(previous)),
    ]);
    r.unresolved = vec!["source-text-encoding", "reference-comparison"];
    Ok(())
}

fn file_info(r: &mut Recovery) -> Result<(), String> {
    let size = r.raw.len() as u32;
    let list = r.u32(0)?;
    require(list >= 20, "VWFI list overlaps header")?;
    r.range(list as usize, 2)?;
    let count = r.u16(list as usize)? as u32;
    r.fields = BTreeMap::from([
        ("list_offset", list), ("unknown_1", r.u32(4)?), ("unknown_2", r.u32(8)?), ("flags_raw", r.u32(12)?),
        ("script_id", r.u32(16)?), ("entry_count", count),
    ]);
    if count != 0 {
        r.range(list as usize + 2, (count as usize + 1) * 4)?;
        let base = list + 2 + (count + 1) * 4;
        let mut previous = r.u32(list as usize + 2)?;
        require(previous as u64 <= wide(size, base), "VWFI first entry outside chunk")?;
        for i in 0..count {
            let next = r.u32((list + 2 + (i + 1) * 4) as usize)?;
            require(next >= previous && next as u64 <= wide(size, base), "VWFI invalid entry offsets")?;
            r.records.push(Range { offset: base + previous, length: next - previous });
            previous = next;
        }
        r.fields.insert("data_offset", base);
        r.fields.insert("trailing_length", size.wrapping_sub(base).wrapping_sub(previous));
        if count >= 5 && r.records[4].length >= 2 {
            let preload = r.u16(r.records[4].offset as usize)? as u32;
            r.fields.insert("preload_raw", preload);
        }
    }
    r.unresolved = vec!["file-info-flags-and-extra-entries", "source-text-encoding", "reference-comparison"];
    Ok(())
}

fn range_json(out: &mut String, range: Range, raw: &[u8]) {
    let bytes = &raw[range.offset as usize..(range.offset + range.length) as usize];
    out.push_str(&format!("{{\"offset\":{},\"length\":{},\"hex\":\"{}\"}}", range.offset, range.length, hex(bytes)));
}

fn fields_json<T: std::fmt::Display>(out: &mut String, fields: &BTreeMap<&'static str, T>) {
    out.push('{');
    for (i, (key, value)) in fields.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!("\"{key}\":{value}"));
    }
    out.push('}');
}

/// Decodes one score chunk (`VWSC`, `VWLB` or `VWFI`) of a Director file of
/// the given version (500..1000) to the probe's JSON.
pub fn decode(fourcc: &str, bytes: &[u8], version: u32) -> Result<String, String> {
    require(matches!(version, 500 | 600 | 700 | 800 | 1000), "score recovery supports Director 5, 6, 7, 8 and 10 only")?;
    require(!bytes.is_empty() && bytes.len() <= 64 * 1024 * 1024, "invalid score chunk size")?;
    let mut r = Recovery {
        fourcc,
        raw: bytes,
        fields: BTreeMap::new(),
        records: Vec::new(),
        labels: Vec::new(),
        frames: Vec::new(),
        unresolved: Vec::new(),
    };
    match fourcc {
        "VWSC" => score(&mut r, version)?,
        "VWLB" => labels(&mut r)?,
        "VWFI" => file_info(&mut r)?,
        _ => return Err("unsupported score chunk type".into()),
    }
    let raw = r.raw;
    let mut out = String::with_capacity(raw.len() * 6);
    out.push_str("{\"schema_version\":1,\"stage\":\"structural-score-spike\",\"runtime_ir_frozen\":false,");
    out.push_str("\"semantic_complete\":false,\"reference_verified\":false,\"fourcc\":\"");
    out.push_str(r.fourcc);
    out.push_str("\",\"fields\":");
    fields_json(&mut out, &r.fields);
    out.push_str(",\"raw_hex\":\"");
    out.push_str(&hex(raw));
    out.push_str("\",\"records\":[");
    for (i, record) in r.records.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        range_json(&mut out, *record, raw);
    }
    out.push_str("],\"labels\":[");
    for (i, label) in r.labels.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!("{{\"frame\":{},\"text\":", label.frame));
        range_json(&mut out, label.text, raw);
        out.push_str(",\"comment\":");
        range_json(&mut out, label.comment, raw);
        out.push('}');
    }
    out.push_str("],\"frames\":[");
    for (i, frame) in r.frames.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!("{{\"number\":{},\"source\":", frame.number));
        range_json(&mut out, frame.source, raw);
        out.push_str(",\"deltas\":[");
        for (j, delta) in frame.deltas.iter().enumerate() {
            if j > 0 {
                out.push(',');
            }
            out.push_str(&format!("{{\"channel_offset\":{},\"payload\":", delta.channel_offset));
            range_json(&mut out, delta.payload, raw);
            out.push('}');
        }
        out.push_str("],\"changed_channels\":[");
        for (j, channel) in frame.changed.iter().enumerate() {
            if j > 0 {
                out.push(',');
            }
            out.push_str(&format!(
                "{{\"index\":{},\"record_hex\":\"{}\",\"known_mask_hex\":\"{}\",\"fields\":",
                channel.index,
                hex(&channel.bytes),
                hex(&channel.known)
            ));
            fields_json(&mut out, &channel.fields);
            out.push('}');
        }
        out.push_str("]}");
    }
    out.push_str("],\"unresolved\":[");
    for (i, item) in r.unresolved.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!("\"{item}\""));
    }
    out.push_str("]}");
    Ok(out)
}
