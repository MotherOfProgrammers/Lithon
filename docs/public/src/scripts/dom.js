export function qs(selector, scope = document) {
    return scope.querySelector(selector);
}
export function qsa(selector, scope = document) {
    return Array.from(scope.querySelectorAll(selector));
}
export function onNextFrame(handler) {
    let queued = false;
    return () => {
        if (queued)
            return;
        queued = true;
        window.requestAnimationFrame(() => {
            queued = false;
            handler();
        });
    };
}
