#pragma once

namespace SidebarScript {
// Runs in WebView2's renderer, not Qt's GUI thread. Follow only the two HA
// shell roots; dashboard cards and sensor updates never trigger a DOM scan.
// These selectors and CSS variables belong to HA's frontend, so leave other
// sites and mobile/modal menus alone when that shell is absent.
inline constexpr auto source = LR"JS(
(() => {
    if (window !== window.top) return;
    let width = 256;
    let current = null;
    let dispose = () => {};
    const send = value => window.chrome.webview.postMessage(value);
    window.chrome.webview.addEventListener('message', event => {
        const data = event.data;
        if (data?.type !== 'what-sidebar' || !Number.isInteger(data.width)
            || data.width < 180 || data.width > 420) return;
        width = data.width;
        current?.style.setProperty('--what-sidebar-width', width + 'px');
    });

    async function attach(main) {
        dispose();
        dispose = () => {};
        current = main;
        if (!main) return;
        await customElements.whenDefined('home-assistant-main');
        await main.updateComplete;
        if (current !== main || !main.isConnected || !main.shadowRoot?.querySelector('ha-sidebar')) return;

        const style = document.createElement('style');
        style.textContent = `
            :host([expanded]:not([modal])) {
                --ha-sidebar-width: calc(var(--what-sidebar-width) + var(--safe-area-inset-left, 0px));
                --ha-sidebar-expanded-width: var(--what-sidebar-width);
                --ha-sidebar-expanded-item-width: calc(var(--what-sidebar-width) - 8px);
            }
            :host([modal]) #what-sidebar-grip { display: none; }
            #what-sidebar-grip {
                position: fixed; top: 0; bottom: 0;
                inset-inline-start: calc(var(--ha-sidebar-width) - 4px);
                width: 8px; z-index: 6; cursor: ew-resize;
                touch-action: none; user-select: none; outline: none;
            }
            #what-sidebar-grip:hover, #what-sidebar-grip:focus-visible,
            #what-sidebar-grip[data-dragging] {
                background: var(--primary-color); opacity: .65;
            }`;
        const grip = document.createElement('div');
        grip.id = 'what-sidebar-grip';
        grip.tabIndex = 0;
        grip.setAttribute('role', 'separator');
        grip.setAttribute('aria-orientation', 'vertical');
        grip.setAttribute('aria-label', 'Resize Home Assistant sidebar');
        grip.setAttribute('aria-valuemin', '56');
        grip.setAttribute('aria-valuemax', '420');
        grip.title = 'Drag to resize. Drag left to collapse. Double click or press Enter to toggle.';
        main.style.setProperty('--what-sidebar-width', width + 'px');
        main.shadowRoot.append(style, grip);

        const expanded = () => main.hasAttribute('expanded');
        const menu = open => {
            if (open !== expanded()) main.dispatchEvent(new CustomEvent('hass-toggle-menu', {
                detail: { open }, bubbles: true, composed: true
            }));
        };
        const aria = () => grip.setAttribute('aria-valuenow', String(expanded() ? width : 56));
        const observer = new MutationObserver(aria);
        observer.observe(main, { attributes: true, attributeFilter: ['expanded', 'modal'] });
        aria();
        let dragging = false;
        let frame = 0;
        let pendingX = 0;
        function resizeTo(value) {
            menu(value >= 136);
            if (value >= 136) {
                width = Math.max(180, Math.min(420, Math.round(value)));
                main.style.setProperty('--what-sidebar-width', width + 'px');
            }
            aria();
        }
        function move() {
            frame = 0;
            const rect = (main.shadowRoot.querySelector('ha-drawer') || main).getBoundingClientRect();
            resizeTo(getComputedStyle(main).direction === 'rtl' ? rect.right - pendingX : pendingX - rect.left);
        }
        grip.addEventListener('pointerdown', event => {
            if (event.button !== 0 || main.hasAttribute('modal')) return;
            dragging = true;
            grip.dataset.dragging = '';
            grip.setPointerCapture(event.pointerId);
            event.preventDefault();
        });
        grip.addEventListener('pointermove', event => {
            if (!dragging) return;
            pendingX = event.clientX;
            if (!frame) frame = requestAnimationFrame(move);
        });
        function finish() {
            if (!dragging) return;
            dragging = false;
            delete grip.dataset.dragging;
            if (frame) { cancelAnimationFrame(frame); move(); }
            send('what:sidebar-width:' + width);
        }
        grip.addEventListener('pointerup', finish);
        grip.addEventListener('lostpointercapture', finish);
        grip.addEventListener('pointercancel', finish);
        grip.addEventListener('dblclick', () => menu(!expanded()));
        grip.addEventListener('keydown', event => {
            const rtl = getComputedStyle(main).direction === 'rtl';
            if (event.key === 'Enter' || event.key === ' ') menu(!expanded());
            else if (event.key === (rtl ? 'ArrowRight' : 'ArrowLeft')) resizeTo(width <= 180 ? 56 : width - 20);
            else if (event.key === (rtl ? 'ArrowLeft' : 'ArrowRight')) resizeTo(expanded() ? width + 20 : width);
            else return;
            event.preventDefault();
            send('what:sidebar-width:' + width);
        });
        dispose = () => {
            observer.disconnect();
            cancelAnimationFrame(frame);
            grip.remove();
            style.remove();
            main.style.removeProperty('--what-sidebar-width');
        };
        send('what:sidebar-ready');
    }

    async function start() {
        await customElements.whenDefined('home-assistant');
        const shell = document.querySelector('home-assistant');
        if (!shell) return;
        await shell.updateComplete;
        if (!shell.shadowRoot) return;
        const check = () => {
            const main = shell.shadowRoot.querySelector('home-assistant-main');
            if (main !== current) attach(main);
        };
        const observer = new MutationObserver(check);
        observer.observe(shell.shadowRoot, { childList: true });
        check();
        window.addEventListener('pagehide', () => { observer.disconnect(); dispose(); }, { once: true });
    }
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start, { once: true });
    else start();
})();
)JS";
} // namespace SidebarScript
