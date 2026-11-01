"""Portable renderer-work gates; host timings rank costs without claiming TV FPS."""
from __future__ import annotations

import math

# These cap algorithmic work, not a particular machine's frame time. Translation
# waste is independently useful even when a fast desktop hides the slowdown.
LIMITS = {"bitmap_screens": 16, "inverse_screens": 8, "inverse_scan_ratio": 4,
          "inverse_waste_screens": .25, "cache_pressure_run": 3}
COUNTERS = ("frames", "scene_frames", "bitmap_calls", "bitmap_pixels", "inverse_pixels",
            "translated_pixels", "translated_bounds_pixels", "render_us")
PEAKS = ("peak_frame_pixels", "peak_frame_inverse_pixels", "peak_frame_loads",
         "peak_frame_evictions", "peak_frame_pressure_evictions", "peak_cache_pressure_run",
         "peak_image_bytes", "peak_render_us")


def findings(work: dict) -> list[str]:
    """Reject missing/malformed work and excessive scans, independent of wall time."""
    limits = LIMITS
    if type(work.get("version")) is not int or work["version"] != 2:
        return ["renderer work metrics missing or unsupported; rebuild the web runtime"]
    if (type(work.get("inverse_scan_ratio")) is not int or
            work["inverse_scan_ratio"] != limits["inverse_scan_ratio"]):
        return ["renderer scan-ratio policy differs; rebuild the web runtime"]
    keys = (*COUNTERS, *PEAKS, "stage_pixels", "image_budget")
    if any(type(work.get(key)) is not int or work[key] < 0 for key in keys):
        return ["invalid renderer work counters"]
    stage = work["stage_pixels"]
    if stage not in (640 * 480, 800 * 600):
        return ["invalid renderer stage size"]
    for name in ("worst_inverse", "wasteful_inverse"):
        draw = work.get(name)
        if (not isinstance(draw, dict) or any(type(draw.get(key)) is not int or draw[key] < 0
                for key in ("pixels", "sampled_pixels", "channel", "member", "frame")) or
                draw["sampled_pixels"] > draw["pixels"] or not isinstance(draw.get("movie"), str)):
            return ["invalid inverse-mapping work counters"]
    worst = work["wasteful_inverse"]
    errors = []
    if (worst["pixels"] > max(1, worst["sampled_pixels"]) * limits["inverse_scan_ratio"]
            and worst["pixels"] - worst["sampled_pixels"] >
            stage * limits["inverse_waste_screens"]):
        errors.append(f"inverse mapping scans {worst['pixels']} pixels for "
                      f"{worst['sampled_pixels']} sampled pixels in {worst.get('movie')} "
                      f"frame {worst['frame']}, channel {worst['channel']}, "
                      f"member {worst['member']}")
    for key, maximum in (("peak_frame_pixels", limits["bitmap_screens"]),
                         ("peak_frame_inverse_pixels", limits["inverse_screens"])):
        if work[key] > stage * maximum:
            errors.append(f"{key}: {work[key] / stage:.2f} stage equivalents exceeds {maximum}")
    if work["peak_cache_pressure_run"] >= limits["cache_pressure_run"]:
        errors.append(f"image cache churn: >32 pressure evictions for "
                      f"{work['peak_cache_pressure_run']} consecutive scene redraws")
    return errors


def summarize(samples: list[dict]) -> dict:
    """Retain per-command offenders and the slowest rendered frames with provenance."""
    result = {"available": True, "samples": 0, "missing_samples": 0,
              "totals": dict.fromkeys(COUNTERS, 0), "peaks": dict.fromkeys(PEAKS, 0),
              "findings": [], "hotspots": []}
    for sample in samples:
        work = sample.get("render_work")
        if not isinstance(work, dict):
            result["missing_samples"] += 1
            continue
        result["samples"] += 1
        errors = findings(work)
        location = {"movie": sample.get("movie"), "frame": sample.get("frame"),
                    "command": sample.get("command")}
        result["findings"].extend(location | {"error": error} for error in errors)
        if any(type(work.get(key)) is not int or work[key] < 0 for key in (*COUNTERS, *PEAKS)):
            continue
        for key in COUNTERS:
            result["totals"][key] += work[key]
        for key in PEAKS:
            result["peaks"][key] = max(result["peaks"][key], work[key])
        if work["frames"]:
            result["hotspots"].append(location | {
                "peak_render_ms": work["peak_render_us"] / 1000,
                "peak_render_movie": work.get("peak_render_movie"),
                "peak_render_frame": work.get("peak_render_frame"),
                "peak_frame_pixels": work["peak_frame_pixels"],
                "peak_frame_inverse_pixels": work["peak_frame_inverse_pixels"],
                "image_budget": work.get("image_budget"),
                "worst_inverse": work.get("worst_inverse"),
                "wasteful_inverse": work.get("wasteful_inverse")})
    result["available"] = not result["missing_samples"] and result["samples"] > 0
    # Preserve findings in full; ranking is diagnostic and is not a device-FPS estimate.
    result["hotspots"].sort(key=lambda row: (row["peak_render_ms"],
                                           row["peak_frame_inverse_pixels"]), reverse=True)
    result["hotspots"] = result["hotspots"][:20]
    if any(not math.isfinite(row["peak_render_ms"]) for row in result["hotspots"]):
        raise ValueError("non-finite renderer timing")
    return result
