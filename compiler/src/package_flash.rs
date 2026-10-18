use super::*;
use crate::scene::{FilmLayer, flash};
pub(super) fn actions(o: &mut Out, a: &[flash::Action]) -> Result<(), String> {
    o.u16(u16_of(a.len(), "Flash action count")?);
    for a in a {
        o.u16(a.frame);
        o.u32(a.event);
        o.u8(a.key);
        o.count(a.code.len())?;
        o.0.extend_from_slice(&a.code);
    }
    Ok(())
}
pub(super) fn frame(
    o: &mut Out,
    s: &mut Strings,
    frame: &[FilmLayer],
    p: Profile,
) -> Result<(), String> {
    o.u16(u16_of(frame.len(), "runtime film layer count")?);
    for l in frame {
        o.u16(l.channel);
        o.u16(l.film_frame);
        write_spec(o, s, &l.value, p)?;
        o.u32(s.add(&l.asset)?);
        o.u32(s.add(&l.name)?);
        o.u32(s.add(&l.mask_asset)?);
        o.i32(l.origin_x);
        o.i32(l.origin_y);
        o.u8(u8::from(l.affine));
        for &v in &l.matrix {
            o.i32(v);
        }
        for v in [l.clip_id, l.button_id, l.field_id, l.clip_depth] {
            o.u16(v);
        }
        o.u32(l.placement_generation);
        o.u8(u8::from(l.cxform.is_some()));
        if let Some(c) = l.cxform {
            for v in c {
                o.i16(v);
            }
        }
        actions(o, &l.actions)?;
    }
    Ok(())
}
pub(super) fn runtime(
    o: &mut Out,
    s: &mut Strings,
    r: &flash::Runtime,
    p: Profile,
) -> Result<(), String> {
    for n in [
        r.clips.len(),
        r.buttons.len(),
        r.sounds.len(),
        r.exports.len(),
    ] {
        o.u16(u16_of(n, "Flash definition count")?);
    }
    for c in &r.clips {
        o.u16(c.id);
        o.u16(c.frame_count);
        for v in c.local_bounds {
            o.i32(v);
        }
        for f in &c.frames {
            frame(o, s, f, p)?;
        }
        actions(o, &c.actions)?;
        o.u16(u16_of(c.labels.len(), "Flash clip labels")?);
        for (name, f) in &c.labels {
            o.u32(s.add(name)?);
            o.u16(*f);
        }
        o.u16(u16_of(c.sounds.len(), "Flash sound events")?);
        for e in &c.sounds {
            o.u16(e.frame);
            o.u16(e.sound);
            o.u16(e.loops);
            o.u8(e.flags);
            o.u8(e.envelopes.len() as u8);
            o.u32(e.in_point);
            o.u32(e.out_point);
            for &(n, l, r) in &e.envelopes {
                o.u32(n);
                o.u16(l);
                o.u16(r);
            }
        }
    }
    for b in &r.buttons {
        o.u16(b.id);
        o.u8(u8::from(b.track_as_menu));
        for v in b.local_bounds {
            o.i32(v);
        }
        for f in &b.states {
            frame(o, s, f, p)?;
        }
        actions(o, &b.actions)?;
    }
    for sound in &r.sounds {
        o.u16(sound.id);
        o.u32(s.add(&sound.asset)?);
        o.u32(sound.samples);
        o.u32(sound.rate);
    }
    for (id, name) in &r.exports {
        o.u16(*id);
        o.u32(s.add(name)?);
    }
    o.u16(u16_of(r.initializers.len(), "Flash initializer count")?);
    for (id, a) in &r.initializers {
        o.u16(*id);
        actions(o, std::slice::from_ref(a))?;
    }
    Ok(())
}
