// Prefer the physical mouse's OS pointer for standard Director cursor roles.
// Authored bitmaps, executable masks/invert and tool glyphs remain on canvas.
export function cursorPresentation(role, physicalMouse) {
  if (role === 200) return {css: "none", software: false};
  const css = new Map([[0, "default"], [1, "text"], [2, "crosshair"], [3, "crosshair"], [4, "wait"]]).get(role);
  return physicalMouse && css ? {css, software: false} : {css: "none", software: true};
}
