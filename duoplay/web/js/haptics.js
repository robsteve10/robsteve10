// Taptic feedback on iPhone.
//
// Safari has no vibration API, but since iOS 18 toggling a native switch control
// (<input type="checkbox" switch>) plays the system "selection" haptic. Clicking a
// hidden one from inside a touch handler gives a crisp tick under the thumb, the same
// feel as a native app's buttons. It has to happen during the user's touch (that is
// what lets the switch flip), which is exactly when we want it.

let label = null;
let enabled = true;

function ensure() {
  if (label || typeof document === 'undefined') return label;
  label = document.createElement('label');
  label.setAttribute('aria-hidden', 'true');
  label.style.cssText = 'position:fixed;left:-200px;top:0;width:1px;height:1px;overflow:hidden;opacity:0;pointer-events:none';
  const input = document.createElement('input');
  input.type = 'checkbox';
  input.setAttribute('switch', '');
  input.tabIndex = -1;
  label.appendChild(input);
  document.body.appendChild(label);
  return label;
}

export function setHapticsEnabled(on) { enabled = !!on; }

/** One light tick. Call from a pointerdown / touchstart handler. */
export function tick() {
  if (!enabled) return;
  try { ensure()?.click(); } catch { /* not supported */ }
}
