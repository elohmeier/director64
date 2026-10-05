//! The stored tile layout is a contract between the converter and the
//! runtime: fdi.rs writes a wide image's planes in tile order and
//! runtime/director/bitmap_tiles.h reads them back without rearranging.
//! Nothing records the choice in the file, so a disagreement would render
//! every image past the texture limit scrambled rather than fail
//! (tests/test_image_tiles.py holds the packer to the same arithmetic).

use std::path::{Path, PathBuf};
use std::process::Command;

use director64_aot::convert::fdi::{plane_pixels, tile_index, tiled};

/// Compiles and runs a C harness against the runtime's header; None
/// without a C compiler.
fn run_c(name: &str, source: &str) -> Option<String> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let dir: PathBuf = std::env::temp_dir().join(format!("d64-tiles-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let harness = dir.join(format!("{name}.c"));
    std::fs::write(&harness, source).unwrap();
    let executable = dir.join(name);
    let built = Command::new("cc")
        .args(["-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I"])
        .arg(root.join("runtime/director"))
        .arg(&harness)
        .arg("-o")
        .arg(&executable)
        .status()
        .ok()?;
    assert!(built.success(), "harness did not compile");
    let out = Command::new(&executable).output().unwrap();
    std::fs::remove_dir_all(&dir).ok();
    Some(String::from_utf8(out.stdout).unwrap())
}

#[test]
fn plane_extent_agrees_with_the_runtime() {
    let cases = [(1100, 40), (40, 1100), (1786, 149), (1182, 127), (1025, 1025), (640, 480)];
    let rows: String = cases.iter().map(|(w, h)| format!("{{{w},{h}}},")).collect();
    let source = format!(
        "#include \"bitmap_tiles.h\"\n#include <stdio.h>\n\
         static const unsigned cases[][2] = {{{rows}}};\n\
         int main(void) {{\n\
           for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); i++)\n\
             printf(\"%llu %u\\n\", (unsigned long long)bitmap_plane_pixels(cases[i][0], cases[i][1]),\n\
                    bitmap_needs_tiles(cases[i][0], cases[i][1]));\n\
         }}\n"
    );
    let Some(out) = run_c("extent", &source) else { return };
    let runtime: Vec<(usize, bool)> = out
        .lines()
        .map(|line| {
            let mut parts = line.split_whitespace();
            (parts.next().unwrap().parse().unwrap(), parts.next().unwrap() == "1")
        })
        .collect();
    assert_eq!(runtime.len(), cases.len());
    for (&(width, height), &(pixels, needs_tiles)) in cases.iter().zip(&runtime) {
        assert_eq!(plane_pixels(width, height), pixels, "{width}x{height}");
        assert_eq!(tiled(width, height), needs_tiles, "{width}x{height}");
    }
}

#[test]
fn every_pixel_lands_on_the_slot_the_runtime_reads() {
    // Partial edge tiles on both axes, so padding is exercised.
    let (width, height) = (1100, 70);
    let source = format!(
        "#include \"bitmap_tiles.h\"\n#include <stdio.h>\n\
         int main(void) {{\n\
           for (unsigned y = 0; y < {height}; y++)\n\
             for (unsigned x = 0; x < {width}; x++)\n\
               printf(\"%u\\n\", bitmap_tile_index({width}, x, y));\n\
         }}\n"
    );
    let Some(out) = run_c("slots", &source) else { return };
    let runtime: Vec<usize> = out.lines().map(|l| l.parse().unwrap()).collect();
    let converter: Vec<usize> = (0..height).flat_map(|y| (0..width).map(move |x| tile_index(width, x, y))).collect();
    assert_eq!(converter, runtime);
}
