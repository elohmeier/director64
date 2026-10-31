// Shared UTF16 ranges, word wrapping and baselines for formatted text. The
// browser supplies real glyph advances; the Node probe supplies nominal ones.
export function formatText(text, width, formats, measure) {
  if (!formats.length || formats[0].offset !== 0) throw new Error('formatted text needs an initial style');
  const at = (offset) => {
    let i = 0; while (i + 1 < formats.length && formats[i + 1].offset <= offset) i++;
    return i;
  };
  const fragments = (start, end) => {
    const out = []; let i = at(start);
    while (start < end) {
      const stop = Math.min(end, formats[i + 1]?.offset ?? end);
      if (stop <= start) { i++; continue; }
      const style = formats[i], value = text.slice(start, stop);
      out.push({start, end: stop, style, text: value,
        width: measure(value, style) + (style.charSpacing ?? 0) * [...value].length});
      start = stop; i++;
    }
    return out;
  };
  const length = (start, end) => fragments(start, end).reduce((n, f) => n + f.width, 0);
  const trimmed = (start, end) => end - (text.slice(start, end).match(/\s+$/u)?.[0].length ?? 0);
  const rows = []; let y = 0;
  const row = (start, end, final = false) => {
    let parts = fragments(start, end);const base = formats[at(start)];
    const styles = parts.length ? parts.map(f => f.style) : [base];
    const ascent = Math.max(...styles.map(s => s.ascent > 0 ? s.ascent : Math.round((s.size || 12) * .8)));
    const height = Math.max(...styles.map(s => s.lineHeight > 0 ? s.lineHeight : Math.round((s.size || 12) * 1.2)));
    const shownEnd = trimmed(start, end), shown = length(start, shownEnd);
    const firstWord = start + (text.slice(start, shownEnd).match(/^ +/)?.[0].length ?? 0);
    const spaces = [...text.slice(firstWord, shownEnd)].filter(c => c === ' ').length;
    if (base.align === 3 && !final && spaces && width > shown) {
      const padding = (width - shown) / spaces;
      parts = parts.flatMap(part => {
        const split = [];let offset = part.start;
        for (const value of part.text.match(/[^ ]+| /g) ?? []) {
          const pad = value === ' ' && offset >= firstWord && offset < shownEnd ? padding : 0;
          split.push({start:offset,end:offset+value.length,style:part.style,text:value,pad,
            width:measure(value,part.style)+(part.style.charSpacing??0)*[...value].length+pad});
          offset += value.length;
        }
        return split;
      });
    }
    let x = base.align === 1 ? (width - shown) / 2 : base.align === 2 ? width - shown : 0;
    const cellHeight = base.layoutFlags & 2 ? ascent + 3 : height;
    for (const part of parts) { part.x = x; part.y = y; part.baseline = y + ascent; part.height = cellHeight; x += part.width; }
    rows.push({start, end, parts, y, height}); y += height;
  };
  let paragraph = 0;
  while (true) {
    let end = paragraph; while (end < text.length && text[end] !== '\r' && text[end] !== '\n') end++;
    let line = paragraph, cursor = paragraph;
    const tokens = formats[at(paragraph)].layoutFlags & 1 ? [] : text.slice(paragraph, end).match(/\S+\s*|\s+/gu) ?? [];
    for (const token of tokens) {
      const stop = cursor + token.length, shownEnd = trimmed(line, stop);
      if (line < cursor && length(line, shownEnd) > width) { row(line, cursor); line = cursor; }
      while (line < shownEnd && length(line, shownEnd) > Math.max(1, width)) {
        let fit = line, probe = line;
        for (const c of text.slice(line, shownEnd)) {
          probe += c.length;
          if (length(line, probe) > Math.max(1, width) && fit > line) break;
          fit = probe;
        }
        if (fit >= shownEnd) break;
        row(line, fit); line = fit;
      }
      cursor = stop;
    }
    row(line, end, true);
    if (end >= text.length) break;
    paragraph = end + (text.startsWith('\r\n', end) ? 2 : 1);
  }
  const boxes = new Int16Array(text.length * 4);
  const short = value => Math.max(-32768,Math.min(32767,Math.round(value)));
  for (const line of rows) for (const part of line.parts) {
    let k = 0, ordinal = 0;
    for (const c of part.text) {
      const left = part.x + measure(part.text.slice(0, k), part.style) + (part.style.charSpacing ?? 0) * ordinal;
      const right = part.x + measure(part.text.slice(0, k + c.length), part.style) + (part.style.charSpacing ?? 0) * (ordinal + 1) + (part.pad ?? 0);
      for (let u = 0; u < c.length; u++) boxes.set([short(left), short(part.y),
        short(right - left), short(part.height)], (part.start + k + u) * 4);
      k += c.length; ordinal++;
    }
  }
  return {rows, boxes, height: y};
}
