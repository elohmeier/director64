//! SWF8 gradient bevels, independently implemented from Adobe's SWF19 and
//! ActionScript2 GradientBevelFilter contracts (no player source copied).
//! https://open-flash.github.io/mirrors/swf-spec-19.pdf
//! https://open-flash.github.io/mirrors/as2-language-reference/flash/filters/GradientBevelFilter.html
//!
//! Displaced alpha difference supplies the two gradient edges. Separable box
//! passes approximate Flash's blur. The source-free Adobe Flash32 debugger
//! fixture qualifies all nine probe cases: zero-strength/endpoints exact,
//! the authored inner-blur parameters within3/channel, outer/knockout within15
//! at low alpha. Its integer premultiplication is not yet bit-identical; this
//! comparison does not claim selected Director Xtra or N64 hardware parity.

type R<T> = Result<T, String>;
#[derive(Clone, Debug)]
pub struct GradientBevel {
    pub colors: Vec<[u8; 4]>,
    pub ratios: Vec<u8>,
    pub blur_x: f64,
    pub blur_y: f64,
    pub angle: f64,
    pub distance: f64,
    pub strength: f64,
    pub inner: bool,
    pub knockout: bool,
    pub composite_source: bool,
    pub on_top: bool,
    pub passes: u8,
}
impl GradientBevel {
    pub fn identity(&self) -> bool {
        self.strength == 0.0 || self.passes == 0
    }
    pub fn padding(&self) -> (usize, usize) {
        if self.identity() {
            return (0, 0);
        }
        (
            (self.distance * self.angle.cos()).abs().ceil() as usize
                + (self.blur_x * self.passes as f64 * 0.5).ceil() as usize,
            (self.distance * self.angle.sin()).abs().ceil() as usize
                + (self.blur_y * self.passes as f64 * 0.5).ceil() as usize,
        )
    }
}
fn take<'a>(bytes: &'a [u8], pos: &mut usize, count: usize) -> R<&'a [u8]> {
    let end = pos.checked_add(count).ok_or("SWF filter size overflow")?;
    let result = bytes.get(*pos..end).ok_or("truncated SWF filter")?;
    *pos = end;
    Ok(result)
}
fn fixed(bytes: &[u8], pos: &mut usize) -> R<f64> {
    Ok(i32::from_le_bytes(take(bytes, pos, 4)?.try_into().unwrap()) as f64 / 65536.0)
}
pub fn read_list(bytes: &[u8], pos: &mut usize) -> R<Vec<GradientBevel>> {
    let count = take(bytes, pos, 1)?[0];
    let mut filters = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let kind = take(bytes, pos, 1)?[0];
        if kind != 7 {
            return Err(format!("unsupported SWF filter {kind}"));
        }
        let colors_count = take(bytes, pos, 1)?[0] as usize;
        if colors_count == 0 {
            return Err("SWF gradient bevel has no colors".into());
        }
        let colors = take(bytes, pos, colors_count * 4)?
            .chunks_exact(4)
            .map(|c| c.try_into().unwrap())
            .collect();
        let ratios = take(bytes, pos, colors_count)?.to_vec();
        if ratios.windows(2).any(|pair| pair[0] > pair[1]) {
            return Err("SWF gradient bevel ratios are not ordered".into());
        }
        let blur_x = fixed(bytes, pos)?;
        let blur_y = fixed(bytes, pos)?;
        let angle = fixed(bytes, pos)?;
        let distance = fixed(bytes, pos)?;
        let strength = u16::from_le_bytes(take(bytes, pos, 2)?.try_into().unwrap()) as f64 / 256.0;
        let flags = take(bytes, pos, 1)?[0];
        if !(0.0..=255.0).contains(&blur_x)
            || !(0.0..=255.0).contains(&blur_y)
            || strength > 255.0
            || distance.abs() > 8192.0
        {
            return Err("SWF gradient bevel parameters exceed supported bounds".into());
        }
        filters.push(GradientBevel {
            colors,
            ratios,
            blur_x,
            blur_y,
            angle,
            distance,
            strength,
            inner: flags & 128 != 0,
            knockout: flags & 64 != 0,
            composite_source: flags & 32 != 0,
            on_top: flags & 16 != 0,
            passes: flags & 15,
        });
    }
    Ok(filters)
}
fn integral(prefix: &[f64], at: f64) -> f64 {
    let length = prefix.len() - 1;
    if at <= 0.0 {
        return 0.0;
    }
    if at >= length as f64 {
        return prefix[length];
    }
    let index = at.floor() as usize;
    prefix[index] + (prefix[index + 1] - prefix[index]) * (at - index as f64)
}
fn blur_axis(source: &[f64], width: usize, height: usize, amount: f64, vertical: bool) -> Vec<f64> {
    if amount <= 1.0 {
        return source.to_vec();
    }
    let (length, lines) = if vertical {
        (height, width)
    } else {
        (width, height)
    };
    let mut out = vec![0.0; source.len()];
    let mut prefix = vec![0.0; length + 1];
    for line in 0..lines {
        for n in 0..length {
            let index = if vertical {
                n * width + line
            } else {
                line * width + n
            };
            prefix[n + 1] = prefix[n] + source[index];
        }
        for n in 0..length {
            let at = n as f64 + 0.5;
            let value = (integral(&prefix, at + amount * 0.5)
                - integral(&prefix, at - amount * 0.5))
                / amount;
            let index = if vertical {
                n * width + line
            } else {
                line * width + n
            };
            out[index] = value;
        }
    }
    out
}
fn sample(alpha: &[f64], width: usize, height: usize, x: f64, y: f64) -> f64 {
    let ix = x.floor() as i64;
    let iy = y.floor() as i64;
    let fx = x - ix as f64;
    let fy = y - iy as f64;
    let mut value = 0.0;
    for oy in 0..2 {
        for ox in 0..2 {
            let xx = ix + ox;
            let yy = iy + oy;
            if xx >= 0 && yy >= 0 && xx < width as i64 && yy < height as i64 {
                value += alpha[yy as usize * width + xx as usize]
                    * if ox == 0 { 1.0 - fx } else { fx }
                    * if oy == 0 { 1.0 - fy } else { fy };
            }
        }
    }
    value
}
fn gradient(filter: &GradientBevel, position: f64) -> [f64; 4] {
    if position <= filter.ratios[0] as f64 {
        return filter.colors[0].map(f64::from);
    }
    for i in 1..filter.ratios.len() {
        if position <= filter.ratios[i] as f64 {
            let span = filter.ratios[i] as f64 - filter.ratios[i - 1] as f64;
            let mix = if span == 0.0 {
                1.0
            } else {
                (position - filter.ratios[i - 1] as f64) / span
            };
            return [0, 1, 2, 3].map(|c| {
                filter.colors[i - 1][c] as f64 * (1.0 - mix) + filter.colors[i][c] as f64 * mix
            });
        }
    }
    filter.colors.last().unwrap().map(f64::from)
}
/// Straight-alpha RGBA input/output. Filter radii are stage pixels, independent
/// of the object's scale/rotation, as specified by the Adobe AS2 reference.
pub fn apply(filter: &GradientBevel, width: usize, height: usize, rgba: &mut [u8]) -> R<()> {
    let pixels = width
        .checked_mul(height)
        .filter(|&n| n > 0 && n <= 16 * 1024 * 1024)
        .ok_or("SWF filter surface dimensions exceed bound")?;
    if rgba.len() != pixels * 4 {
        return Err("SWF filter surface size mismatch".into());
    }
    if filter.identity() {
        return Ok(());
    }
    let mut bounds = [width, height, 0, 0];
    for (index, p) in rgba.chunks_exact(4).enumerate() {
        if p[3] != 0 {
            bounds[0] = bounds[0].min(index % width);
            bounds[1] = bounds[1].min(index / width);
            bounds[2] = bounds[2].max(index % width + 1);
            bounds[3] = bounds[3].max(index / width + 1);
        }
    }
    if bounds[2] <= bounds[0] || bounds[3] <= bounds[1] {
        return Ok(());
    }
    let dx = filter.angle.cos() * filter.distance;
    let dy = filter.angle.sin() * filter.distance;
    let pad_x = (dx.abs() + filter.blur_x * filter.passes as f64 * 0.5).ceil() as usize;
    let pad_y = (dy.abs() + filter.blur_y * filter.passes as f64 * 0.5).ceil() as usize;
    // Flash8 disables a filter whose resulting cache is larger than 2880px.
    if bounds[2] - bounds[0] + 2 * pad_x > 2880 || bounds[3] - bounds[1] + 2 * pad_y > 2880 {
        return Ok(());
    }
    let mut alpha: Vec<f64> = rgba.chunks_exact(4).map(|p| p[3] as f64 / 255.0).collect();
    for _ in 0..filter.passes {
        alpha = blur_axis(&alpha, width, height, filter.blur_x, false);
        alpha = blur_axis(&alpha, width, height, filter.blur_y, true);
    }
    let original = rgba.to_vec();
    if filter.knockout || !filter.composite_source {
        rgba.fill(0);
    }
    let left = bounds[0].saturating_sub(pad_x);
    let top = bounds[1].saturating_sub(pad_y);
    let right = (bounds[2] + pad_x).min(width);
    let bottom = (bounds[3] + pad_y).min(height);
    for y in top..bottom {
        for x in left..right {
            let index = (y * width + x) * 4;
            let difference = (sample(&alpha, width, height, x as f64 + dx, y as f64 + dy)
                - sample(&alpha, width, height, x as f64 - dx, y as f64 - dy))
                * filter.strength;
            let ratio = (128.0 + difference * if difference < 0.0 { 128.0 } else { 127.0 })
                .clamp(0.0, 255.0);
            let mut color = gradient(filter, ratio);
            let coverage = original[index + 3] as f64 / 255.0;
            color[3] *= if filter.on_top {
                1.0
            } else if filter.inner {
                coverage
            } else {
                1.0 - coverage
            };
            super::blend(rgba, index, color);
        }
    }
    Ok(())
}
