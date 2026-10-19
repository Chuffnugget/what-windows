#pragma once

namespace BrowserScript {
// Home Assistant uses shadow DOM for its dialogs. Inspect it only on Escape,
// before the page closes a dialog, then give its event handlers first refusal.
// The bridge exposes just two UI gestures, never arbitrary native commands.
inline constexpr auto source = LR"JS(
(() => {
    if (window !== window.top) return;
    const send = value => window.chrome.webview.postMessage(value);
    function hasPopup(root) {
        for (const node of root.querySelectorAll('*')) {
            if (node.matches('dialog[open], [role="dialog"], [aria-modal="true"], [role="menu"], ha-dialog[open], mwc-dialog[open]')
                && Array.from(node.getClientRects()).some(rect => rect.width > 0 && rect.height > 0)
                && getComputedStyle(node).visibility !== 'hidden') return true;
            if (node.shadowRoot && hasPopup(node.shadowRoot)) return true;
        }
        return false;
    }
    window.addEventListener('keydown', event => {
        if (event.key !== 'Escape' || event.repeat || event.isComposing) return;
        const busy = !!document.fullscreenElement || hasPopup(document);
        setTimeout(() => {
            if (!busy && !event.defaultPrevented) send('what:escape');
        }, 0);
    }, true);
    let atTop = false;
    window.addEventListener('pointermove', event => {
        const next = event.clientY >= 0 && event.clientY < 16;
        if (next && !atTop) send('what:toolbar');
        atTop = next;
    }, { passive: true });
})();
)JS";
} // namespace BrowserScript
