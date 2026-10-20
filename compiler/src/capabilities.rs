//! Static rendering requirements shared by host, Wasm and Android packaging.
//! This is a fail-closed check of authored score records, not a reachability or
//! visual qualification claim. Dynamic members still need rendered journeys.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use serde_json::{Value, json};

use crate::scene::Profile;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Target {
    Web,
    Android,
}

impl Target {
    pub fn parse(name: &str) -> Result<Self, String> {
        match name {
            "web" => Ok(Self::Web),
            "android" => Ok(Self::Android),
            _ => Err(format!("unknown renderer: {name}")),
        }
    }
    fn name(self) -> &'static str {
        match self {
            Self::Web => "web",
            Self::Android => "android",
        }
    }
}

/// Mirrors the implemented pixel operations in runtime/director/ink.h. New
/// ink numbers must acquire a pixel contract before joining this allowlist.
pub fn bitmap_ink(profile: Profile, ink: u8) -> bool {
    matches!(ink, 0 | 8 | 36)
        || (profile.modern() && (ink <= 8 || (32..=41).contains(&ink)))
        || (profile.d10() && ink == 9)
}

fn number(value: &Value, default: u64) -> u64 {
    value.as_u64().unwrap_or(default)
}

pub fn profile(model: &Value) -> Result<Profile, String> {
    let movies = model["movies"].as_array().ok_or("model without movies")?;
    let mut version = 600;
    for movie in movies {
        if let Some(declared) = movie.get("directorVersion") {
            if !matches!(
                declared.as_u64(),
                Some(5 | 6 | 7 | 8 | 10 | 500 | 600 | 700 | 800 | 850 | 1000)
            ) {
                return Err(format!(
                    "{}: unsupported Director versions: {declared}",
                    movie["name"]
                ));
            }
        }
        version = version.max(crate::scene::movie_version(movie) as u16 * 100);
    }
    // A pure D5 model uses D5 tables rather than the D6 defaults.
    if !movies.is_empty() && movies.iter().all(|m| crate::scene::movie_version(m) == 5) {
        version = 500;
    }
    Ok(Profile {
        version,
        extended: model["extendedD6"].as_bool().unwrap_or(false),
        full_engine: model["fullEngine"].as_bool().unwrap_or(false),
    })
}

pub fn audit(model: &Value, target: Target, selected: Option<Profile>) -> Result<Value, String> {
    let inferred = profile(model)?;
    let profile = selected.unwrap_or(inferred);
    let movies = model["movies"].as_array().ok_or("model without movies")?;
    let by_name: HashMap<_, _> = movies
        .iter()
        .map(|m| (m["name"].as_str().unwrap_or("").to_uppercase(), m))
        .collect();
    let mut versions = BTreeSet::new();
    let mut findings = Vec::<Value>::new();
    let mut unresolved = 0;
    let mut ignored_palettes = Vec::new();
    let mut pattern_problems = Vec::new();
    let mut font_variants = BTreeSet::new();
    for font in model["fonts"].as_array().into_iter().flatten() {
        if !(1..255).contains(&number(&font["number"], 0)) {
            continue;
        }
        for alias in font["aliases"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
        {
            for variant in font["variants"].as_array().into_iter().flatten() {
                if variant["asset"].as_str().is_some_and(|s| !s.is_empty()) {
                    font_variants.insert((alias.to_lowercase(), number(&variant["size"], 0)));
                }
            }
        }
    }
    for movie in movies {
        let name = movie["name"].as_str().ok_or("movie without name")?;
        for cast in movie["casts"].as_array().into_iter().flatten() {
            for problem in cast["patternProblems"].as_array().into_iter().flatten() {
                let mut detail = problem.clone();
                detail["movie"] = json!(name);
                detail["cast"] = cast["number"].clone();
                pattern_problems.push(detail);
            }
        }
        for member in movie["members"].as_array().into_iter().flatten() {
            let pattern = number(&member["pattern"], 0);
            if member["type"] == 8 && (57..=64).contains(&pattern) {
                let cast = movie["casts"]
                    .as_array()
                    .and_then(|casts| casts.iter().find(|c| c["number"] == member["cast"]));
                let catalog = cast
                    .and_then(|c| c["patternAssets"].as_array())
                    .filter(|_| profile.modern());
                let available = match catalog {
                    Some(assets) => assets
                        .get((pattern - 57) as usize)
                        .and_then(Value::as_str)
                        .is_some_and(|s| !s.is_empty()),
                    None => member["asset"].as_str().is_some_and(|s| !s.is_empty()),
                };
                if !available {
                    findings.push(json!({"code":"shape-pattern-asset-missing","records":1,
                    "examples":[{"movie":name,"cast":member["cast"],"member":member["number"],"pattern":pattern}]}));
                }
            }
            if member["flashRuntime"].is_object() {
                if !profile.d10() {
                    findings.push(
                        json!({"code":"unsupported-flash-runtime-profile","records":1,
                    "examples":[{"movie":name,"cast":member["cast"],"member":member["number"]}]}),
                    );
                }
                fn masks(v: &Value) -> usize {
                    match v {
                        Value::Object(o) => {
                            usize::from(
                                o.get("asset")
                                    .and_then(Value::as_str)
                                    .is_some_and(|s| !s.is_empty())
                                    && o.contains_key("channel")
                                    && !o
                                        .get("maskAsset")
                                        .and_then(Value::as_str)
                                        .is_some_and(|s| !s.is_empty()),
                            ) + o.values().map(masks).sum::<usize>()
                        }
                        Value::Array(a) => a.iter().map(masks).sum(),
                        _ => 0,
                    }
                }
                let count = masks(&member["flashRuntime"]) + masks(&member["filmLayers"]);
                if count > 0 {
                    findings.push(
                        json!({"code":"flash-geometric-mask-missing","records":count,
                    "examples":[{"movie":name,"cast":member["cast"],"member":member["number"]}]}),
                    );
                }
            }
            let mut missing = BTreeSet::new();
            for field in member["flashFields"]
                .as_array()
                .into_iter()
                .flatten()
                .chain(member["flashFontUses"].as_array().into_iter().flatten())
            {
                let Some(font) = field["fontName"].as_str().filter(|s| !s.is_empty()) else {
                    continue;
                };
                let size = field["fontHeight"].as_f64().unwrap_or(12.0).round() as u64;
                if !font_variants.contains(&(font.to_lowercase(), size)) {
                    missing.insert((font, size));
                }
            }
            for (font, size) in missing {
                findings.push(json!({"code":"flash-font-variant-missing","records":1,
                    "examples":[{"movie":name,"cast":member["cast"],"member":member["number"],"font":font,"size":size}]}));
            }
        }
        versions.insert(number(
            &movie["directorVersion"],
            crate::scene::movie_version(movie) as u64 * 100,
        ));
        let mut members: HashMap<(u64, u64), &Value> = movie["members"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|m| ((number(&m["cast"], 1), number(&m["number"], 0)), m))
            .collect();
        for cast in movie["casts"].as_array().into_iter().flatten() {
            let file = cast["file"].as_str().unwrap_or(name).to_uppercase();
            if file == name.to_uppercase() {
                continue;
            }
            let external = by_name
                .get(&file)
                .ok_or_else(|| format!("{name}: missing external cast {file}"))?;
            for member in external["members"].as_array().into_iter().flatten() {
                if number(&member["cast"], 1) == 1 {
                    members.insert(
                        (number(&cast["number"], 1), number(&member["number"], 0)),
                        member,
                    );
                }
            }
        }
        let record_size = if crate::scene::movie_version(movie) >= 7 {
            48
        } else {
            24
        };
        let mut scores = vec![(&movie["score"], None)];
        for film in movie["members"]
            .as_array()
            .into_iter()
            .flatten()
            .filter(|m| m["filmRuntime"] == true || (profile.d10() && m["filmScore"].is_object()))
        {
            if !profile.d10() {
                return Err(format!(
                    "{name}: runtime film layers require the D10 profile"
                ));
            }
            scores.push((&film["filmScore"], Some(film)));
        }
        for (score, film) in scores {
            let record_size = film.and_then(|f| f["filmScore"]["recordSize"].as_u64())
                .map_or(record_size, |n| n as usize);
            for (index, frame) in score["frames"].as_array().into_iter().flatten().enumerate() {
                for channel in frame["channels"].as_array().into_iter().flatten() {
                    let channel_number = number(&channel["channel"], 0);
                    if channel_number == 5 && film.is_none() {
                        let data = channel["bytes"]
                            .as_array()
                            .ok_or("palette record without bytes")?;
                        if data.len() != record_size
                            || data.iter().any(|v| v.as_u64().is_none_or(|n| n > 255))
                        {
                            return Err(format!("{name}: invalid normalized palette record"));
                        }
                        let b = |i: usize| data[i].as_u64().unwrap() as u16;
                        let cast = (b(0) << 8 | b(1)) as u64;
                        let id = b(2) << 8 | b(3);
                        let signed = id as i16;
                        let code = if id == 0 {
                            None
                        } else if b(5) != 0
                            || (b(8) << 8 | b(9)) > 1
                            || (b(10) << 8 | b(11)) > 1
                            || (12..15).any(|i| b(i) != 0)
                        {
                            Some("palette-interpolation-unsupported")
                        } else if signed < 0 {
                            if [-1, -3, -102].contains(&signed) {
                                None
                            } else {
                                Some("builtin-palette-unsupported")
                            }
                        } else if members
                            .get(&(cast, id as u64))
                            .is_none_or(|m| m["type"] != 4)
                        {
                            ignored_palettes.push(json!({"movie":name,"frame":number(&frame["number"],index as u64+1),
                            "cast":cast,"member":id,"reason":if members.contains_key(&(cast,id as u64)){"not-palette"}else{"missing"},
                            "behavior":"retain-active-palette","original_projector_verified":false}));
                            None
                        } else if members[&(cast, id as u64)]["palette"]
                            .as_array()
                            .is_none_or(|p| {
                                p.len() != 256
                                    || p.iter().any(|c| c.as_u64().is_none_or(|c| c > 0xffffff))
                            })
                        {
                            Some("palette-member-colors-missing")
                        } else {
                            None
                        };
                        if let Some(code) = code {
                            findings.push(json!({"code":code,"records":1,"examples":[{"movie":name,
                            "frame":number(&frame["number"],index as u64+1),"cast":cast,"member":signed}]}));
                        }
                    }
                    if channel_number < 6 {
                        continue;
                    }
                    let data = channel["bytes"]
                        .as_array()
                        .ok_or_else(|| format!("{name}: missing sprite record bytes"))?;
                    if data.len() != record_size
                        || data.iter().any(|v| v.as_u64().is_none_or(|n| n > 255))
                    {
                        return Err(format!(
                            "{name}: invalid normalized sprite record length or byte value"
                        ));
                    }
                    let byte = |i: usize| data[i].as_u64().unwrap();
                    if byte(0) == 0 {
                        continue;
                    }
                    let mut cast = byte(4) * 256 + byte(5);
                    let member_number = byte(6) * 256 + byte(7);
                    let member = if let Some(film) = film {
                        let (source, resolved) =
                            crate::convert::film::reference(movie, film, cast as i64, movies)?;
                        cast = resolved as u64;
                        crate::convert::film::child_member(
                            source,
                            resolved,
                            member_number as i64,
                            movies,
                        )
                        .map(|(_, member)| member)
                    } else {
                        members.get(&(cast, member_number)).copied()
                    };
                    let Some(member) = member else {
                        if member_number != 0 && byte(0) >= 16 {
                            unresolved += 1;
                        }
                        continue;
                    };
                    if film.is_some() && crate::convert::film::nonvisual(member) {
                        continue;
                    }
                    let kind = number(&member["type"], 0);
                    let ink = (byte(1) & 63) as u8;
                    let code = if film.is_some() && number(&member["flashVariantCount"], 1) > 1 {
                        Some("film-random-flash-child-needs-instance-state")
                    } else if kind == 10
                        && (profile.d5() || profile.d10())
                        && member["asset"].as_str().is_none_or(str::is_empty)
                        && !(member["videoAudioOnly"] == true
                            && member["videoAudio"].as_str().is_some_and(|s| !s.is_empty())
                            && number(&member["frames"], 0) > 0)
                    {
                        Some("video-asset-missing")
                    } else if matches!(kind, 5 | 12)
                        && (member["asset"].as_str().is_none_or(str::is_empty)
                            || number(&member["width"], 0) == 0
                            || number(&member["height"], 0) == 0)
                    {
                        Some("raster-cast-asset-missing")
                    } else if matches!(kind, 1 | 2 | 5 | 12)
                        && number(&member["width"], 0) != 0
                        && number(&member["height"], 0) != 0
                        && !bitmap_ink(profile, ink)
                    {
                        Some("unsupported-bitmap-ink")
                    } else if kind == 9 {
                        if !profile.d10() {
                            Some("linked-movie-not-rendered")
                        } else if member["linkedMovie"]
                            .as_str()
                            .is_none_or(|name| !by_name.contains_key(&name.to_uppercase()))
                        {
                            Some("linked-movie-target-missing")
                        } else if !(1..=800).contains(&number(&member["width"], 0))
                            || !(1..=600).contains(&number(&member["height"], 0))
                        {
                            Some("unsupported-linked-movie-bounds")
                        } else if ink == 9 || !bitmap_ink(profile, ink) {
                            Some("unsupported-linked-movie-ink")
                        } else {
                            None
                        }
                    } else if !matches!(kind, 1..=8 | 11 | 12 | 14)
                        && !(kind == 10 && (profile.d5() || profile.d10()))
                    {
                        Some("unsupported-visible-cast")
                    } else {
                        None
                    };
                    if let Some(code) = code {
                        let mut detail = json!({"code":code});
                        if matches!(
                            code,
                            "unsupported-bitmap-ink" | "unsupported-linked-movie-ink"
                        ) {
                            detail["ink"] = json!(ink);
                        }
                        if code == "unsupported-visible-cast" {
                            detail["cast_type"] = json!(kind);
                        }
                        let key = detail.clone();
                        let found = findings
                            .iter()
                            .position(|f| key.as_object().unwrap().iter().all(|(k, v)| f[k] == *v));
                        let i = found.unwrap_or_else(|| {
                            detail["records"] = json!(0);
                            detail["examples"] = json!([]);
                            findings.push(detail);
                            findings.len() - 1
                        });
                        findings[i]["records"] =
                            json!(findings[i]["records"].as_u64().unwrap() + 1);
                        let examples = findings[i]["examples"].as_array_mut().unwrap();
                        if examples.len() < 3 {
                            let mut example = json!({"movie":name,"frame":number(&frame["number"], index as u64 + 1),
                            "sprite":channel_number - 5,"cast":cast,"member":member_number});
                            if let Some(film) = film {
                                example["film_cast"] = film["cast"].clone();
                                example["film_member"] = film["number"].clone();
                            }
                            examples.push(example);
                        }
                    }
                }
            }
        }
    }
    let mut approximations = BTreeMap::<String, usize>::new();
    let mut losses = Vec::new();
    for note in model["approximations"].as_array().into_iter().flatten() {
        let kind = note["kind"].as_str().unwrap_or("unknown");
        *approximations.entry(kind.to_string()).or_default() += 1;
        // These records explicitly describe discarded authored output. A
        // recovery deferral pin documents that loss; it does not implement
        // the target feature. Unresolved dynamic children and genuine
        // conversion approximations remain inventory rather than blockers.
        // A preserved mutable placeholder is separate from discarded layers.
        if note["implemented"] == false
            || matches!(
                kind,
                "deferred-conversion"
                    | "film-loop-unflattened-child"
                    | "film-loop-sounds-pending-native"
                    | "film-loop-unresolved-sound"
                    | "flash-actions-ignored"
                    | "flash-runtime-required"
                    | "flash-effects-ignored"
            )
            || (matches!(kind, "film-loop-nested-cycle" | "film-loop-absent-child")
                && note["implemented"] != true)
            || (kind == "film-loop-mask-missing"
                && !(note["implemented"] == true && note["fallback"] == "matte"))
        {
            losses.push(note.clone());
            let found = findings
                .iter()
                .position(|f| f["code"] == "unimplemented-conversion-loss" && f["kind"] == kind);
            let i = found.unwrap_or_else(|| {
                findings.push(json!({"code":"unimplemented-conversion-loss","kind":kind,"records":0,"examples":[]}));
                findings.len() - 1
            });
            findings[i]["records"] = json!(findings[i]["records"].as_u64().unwrap() + 1);
            let examples = findings[i]["examples"].as_array_mut().unwrap();
            if examples.len() < 3 {
                let mut example = note.clone();
                if let Some(channel) = note["channel"].as_u64().filter(|n| *n >= 6) {
                    example["sprite"] = json!(channel - 5);
                }
                examples.push(example);
            }
        }
    }
    Ok(
        json!({"target":target.name(),"director_versions":versions,"runtime_family":profile.version / 100,
        "movies":movies.len(),"problems":model.get("problems").cloned().unwrap_or(json!([])),
        "findings":findings,"unresolved_member_records":unresolved,"approximation_counts":approximations,
        "conversion_losses":losses,"ignored_palette_commands":ignored_palettes,"source_pattern_problems":pattern_problems,
        "scope":"Authored score records and explicit conversion losses; scripts, visibility and reachability are not simulated."}),
    )
}

pub fn require(report: &Value, identity: &str) -> Result<(), String> {
    if report["problems"].as_array().is_none_or(|v| !v.is_empty()) {
        return Err(format!(
            "{identity}: resource conversion is incomplete: {}",
            report["problems"]
        ));
    }
    if let Some(finding) = report["findings"].as_array().and_then(|v| v.first()) {
        let at = &finding["examples"][0];
        let location = [
            "film_cast",
            "film_member",
            "frame",
            "sprite",
            "cast",
            "member",
            "font",
            "size",
        ]
        .iter()
        .filter_map(|k| at.get(*k).map(|v| format!(" {k} {v}")))
        .collect::<String>();
        return Err(format!(
            "{identity}: renderer {} capability {}{}{} at {}{} ({} records)",
            report["target"].as_str().unwrap_or("?"),
            finding["code"].as_str().unwrap_or("?"),
            finding
                .get("ink")
                .map_or(String::new(), |v| format!(" {v}")),
            finding["kind"]
                .as_str()
                .map_or(String::new(), |v| format!(" {v}")),
            at["movie"].as_str().unwrap_or("?"),
            location,
            finding["records"]
        ));
    }
    Ok(())
}
