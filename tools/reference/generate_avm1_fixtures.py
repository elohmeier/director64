#!/usr/bin/env python3
"""Generate source-free VM inputs for numerical original-player contracts."""
import argparse
import json
from pathlib import Path
from generate_avm1_probe import cases, unicode_cases, super_cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    common = {"undefined-write": "after", "null-write": "after", "number-write": "after",
              "string-write": "after", "undefined-call": "after", "undefined-property": "undefined"}
    unicode = {"unicode-length": "8", "unicode-char-code": "252", "unicode-char": "ü",
               "unicode-substring": "üße", "unicode-substr": "üße", "unicode-upper": "GRÜßE😀!",
               "unicode-lower-sigma": "οσ", "unicode-lower-dotted-i": "i", "unicode-upper-ligature": "ﬃ",
               "unicode-high-surrogate": "55357", "unicode-low-surrogate": "56832"}
    supers = {"super-method": "37", "super-constructor": "BCG", "super-constructor-reset": "BCG",
              "super-type": "object"}
    lines = ["// Synthetic inputs; expected values measured in official Flash32,0,0,465."]
    observations = []
    for version in (5, 6, 7):
        expected = common | {"undefined-number": "0" if version < 7 else "NaN",
                             "undefined-string": "[]" if version < 7 else "[undefined]"}
        inputs = cases()
        if version >= 6:
            inputs |= unicode_cases() | super_cases()
            expected |= unicode | supers | {"super-inherited-method": "27" if version == 6 else "17",
                                            "super-data": "9" if version == 6 else "5",
                                            "super-preload": "17" if version == 6 else "7"}
        for name, code in inputs.items():
            code += b"\0"
            label = f"{name}-v{version}"
            symbol = f"observed_code_{len(observations)}"
            lines += [f"static const uint8_t {symbol}[] = {{"]
            lines += ["  " + ",".join(str(b) for b in code[at:at + 24]) + "," for at in range(0, len(code), 24)]
            lines += ["};"]
            observations.append(f"  {{{json.dumps(label)}, {version}, {symbol}, sizeof({symbol}), {json.dumps(expected[name], ensure_ascii=False)}}},")
    lines += ["static const avm_observation_t observations[] = {", *observations, "};", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
