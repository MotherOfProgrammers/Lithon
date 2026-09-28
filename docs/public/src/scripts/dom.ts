export function qs<T extends Element = HTMLElement>(selector: string, scope: ParentNode = document): T | null {
  return scope.querySelector<T>(selector);
}

export function qsa<T extends Element = HTMLElement>(selector: string, scope: ParentNode = document): T[] {
  return Array.from(scope.querySelectorAll<T>(selector));
}

export function onNextFrame(handler: () => void): () => void {
  let queued = false;
  return (): void => {
    if (queued) return;
    queued = true;
    window.requestAnimationFrame(() => {
      queued = false;
      handler();
    });
  };
}
