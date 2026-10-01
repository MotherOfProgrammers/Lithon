/**
 * Tactile feedback for the icon controls: a spring press, a haptic pulse, and
 * a short synthesised tick.
 *
 * All three channels are opt-in on their own terms and all three degrade
 * silently:
 *   - haptics need `navigator.vibrate` (absent on desktop Safari/Firefox),
 *   - audio needs a user gesture to unlock `AudioContext`,
 *   - motion collapses to a colour change under `prefers-reduced-motion`.
 *
 * Sound is off until the visitor asks for it, and the choice is remembered.
 */

const SOUND_STORAGE_KEY = "lithon-sound";
const REDUCED_MOTION_QUERY = "(prefers-reduced-motion: reduce)";

/** One spring, sampled as a CSS linear() easing. Stiff enough to read as a
 *  mechanical detent rather than a wobble, damped so it settles in one pass. */
const SPRING_STIFFNESS = 420;
const SPRING_DAMPING = 26;
const SPRING_MASS = 1;
const SPRING_SETTLE_MS = 460;
const SPRING_SAMPLES = 46;

const PRESS_SCALE_DOWN = 0.86;
const RELEASE_OVERSHOOT = 1.09;

const HAPTIC_TICK_MS = 9;
const HAPTIC_CONFIRM_MS = [12, 26, 14];

const TICK_FREQUENCY_HZ = 660;
const TICK_DURATION_S = 0.06;
const TICK_GAIN = 0.1;
/* Hover sits a fifth above the click tick and well under its level: it has to
   be distinguishable from the press without becoming the louder of the pair. */
const HOVER_FREQUENCY_HZ = 880;
const HOVER_DURATION_S = 0.035;
const HOVER_GAIN = 0.035;
/** Sweeping the cursor across a row of links would otherwise machine-gun ticks. */
const HOVER_MIN_GAP_MS = 70;
const CONFIRM_FREQUENCIES_HZ = [523.25, 783.99];
const DISMISS_FREQUENCIES_HZ = [659.25, 493.88];
const NOTE_DURATION_S = 0.14;
const NOTE_GAP_S = 0.06;
const CHIME_GAIN = 0.12;
/** Small lead so a note scheduled during the resume turn still lands in the
 *  future instead of being clipped at the context's current time. */
const SCHEDULE_LEAD_S = 0.02;

export type SoundState = boolean;

function prefersReducedMotion(): boolean {
  return window.matchMedia(REDUCED_MOTION_QUERY).matches;
}

function readStoredSound(): SoundState | null {
  try {
    const stored = window.localStorage.getItem(SOUND_STORAGE_KEY);
    return stored === "on" || stored === "off" ? stored === "on" : null;
  } catch {
    return null;
  }
}

function writeStoredSound(enabled: SoundState): void {
  try {
    window.localStorage.setItem(SOUND_STORAGE_KEY, enabled ? "on" : "off");
  } catch {}
}

/** Underdamped spring step, integrated at a fixed sub-step so the sampling is
 *  stable regardless of the device's frame rate. */
function springOffset(progress: number): number {
  if (progress >= 1) return 0;
  const steps = 240;
  const dt = (progress * SPRING_SETTLE_MS) / steps / 1000;
  let displacement = 1;
  let velocity = 0;
  for (let step = 0; step < steps; step += 1) {
    const springForce = -SPRING_STIFFNESS * displacement;
    const dampingForce = -SPRING_DAMPING * velocity;
    const acceleration = (springForce + dampingForce) / SPRING_MASS;
    velocity += acceleration * dt;
    displacement += velocity * dt;
  }
  return displacement;
}

function buildSpringEasing(): string {
  const stops: string[] = [];
  for (let index = 0; index <= SPRING_SAMPLES; index += 1) {
    const progress = index / SPRING_SAMPLES;
    stops.push(springOffset(progress).toFixed(4));
  }
  return `linear(${stops.join(",")})`;
}

const SPRING_EASING = buildSpringEasing();

/** Shared with the site-wide click hook below, so every control agrees on
 *  whether sound is currently wanted. */
let sound: SoundState = false;

export function initSoundToggle(toggle: HTMLElement | null): SoundState {
  const stored = readStoredSound();
  // Sound stays off until it is asked for: an unrequested noise on a marketing
  // header is hostile, and a click can no longer be the thing that turns it on.
  sound = stored ?? false;
  toggle?.classList.toggle("is-on", sound);
  syncToggle(toggle, sound);
  return sound;
}

function syncToggle(toggle: HTMLElement | null, enabled: SoundState): void {
  if (!toggle) return;
  toggle.setAttribute("aria-pressed", String(enabled));
  toggle.setAttribute("aria-label", enabled ? "Turn interface sound off" : "Turn interface sound on");
  toggle.dataset["tooltip"] = enabled ? "Sound on" : "Sound off";
}

let audioContext: AudioContext | null = null;

/** Lazily created on the first real gesture, so autoplay policy is satisfied
 *  without priming the context on page load. */
function context(): AudioContext | null {
  if (audioContext !== null) return audioContext;
  const Ctor = window.AudioContext ?? (window as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
  if (Ctor === undefined) return null;
  try {
    audioContext = new Ctor();
  } catch {
    return null;
  }
  return audioContext;
}

function tone(ctx: AudioContext, frequency: number, at: number, gain: number, duration: number): void {
  const osc = ctx.createOscillator();
  const amp = ctx.createGain();
  osc.type = "sine";
  osc.frequency.setValueAtTime(frequency, at);
  // Fast attack, exponential tail: a struck key, not a beep.
  amp.gain.setValueAtTime(0.0001, at);
  amp.gain.exponentialRampToValueAtTime(gain, at + 0.006);
  amp.gain.exponentialRampToValueAtTime(0.0001, at + duration);
  osc.connect(amp).connect(ctx.destination);
  osc.start(at);
  osc.stop(at + duration + 0.02);
}

/** Schedules only once the context is actually running. A note queued while the
 *  context is still suspended is stamped with a time in the past and never
 *  plays — which is how the toggle used to go silent after a click. */
async function withAudio(run: (ctx: AudioContext) => void): Promise<void> {
  const ctx = context();
  if (ctx === null) return;
  if (ctx.state !== "running") {
    try {
      await ctx.resume();
    } catch {
      return;
    }
  }
  if (ctx.state !== "running") return;
  run(ctx);
}

function playSequence(frequencies: readonly number[], gain: number, duration: number, gap: number): void {
  void withAudio((ctx) => {
    const start = ctx.currentTime + SCHEDULE_LEAD_S;
    frequencies.forEach((frequency, index) => {
      tone(ctx, frequency, start + index * gap, gain, duration);
    });
  });
}

function playTick(): void {
  playSequence([TICK_FREQUENCY_HZ], TICK_GAIN, TICK_DURATION_S, 0);
}

function playHover(): void {
  playSequence([HOVER_FREQUENCY_HZ], HOVER_GAIN, HOVER_DURATION_S, 0);
}

function playConfirm(): void {
  playSequence(CONFIRM_FREQUENCIES_HZ, CHIME_GAIN, NOTE_DURATION_S, NOTE_GAP_S);
}

/** Turning sound off still deserves an answer: the site falls silent right
 *  after this, so the dismissal is the only feedback the click gets. */
function playDismiss(): void {
  playSequence(DISMISS_FREQUENCIES_HZ, CHIME_GAIN, NOTE_DURATION_S, NOTE_GAP_S);
}

function vibrate(pattern: number | number[]): void {
  if (typeof navigator.vibrate !== "function") return;
  try {
    navigator.vibrate(pattern);
  } catch {}
}

/** Spring the control down on press and let it overshoot back on release.
 *  Driven by the Web Animations API so a second press interrupts cleanly
 *  instead of queueing behind the first. */
function springPress(target: HTMLElement, direction: "in" | "out"): void {
  if (prefersReducedMotion()) return;

  const frames: Keyframe[] =
    direction === "in"
      ? [{ transform: "scale(1)" }, { transform: `scale(${PRESS_SCALE_DOWN})` }]
      : [
          { transform: `scale(${PRESS_SCALE_DOWN})` },
          { transform: `scale(${RELEASE_OVERSHOOT})`, offset: 0.42 },
          { transform: "scale(1)" },
        ];

  target.getAnimations().forEach((animation) => {
    if (animation instanceof CSSAnimation) animation.cancel();
  });

  target.animate(frames, {
    duration: direction === "in" ? 120 : SPRING_SETTLE_MS,
    easing: direction === "in" ? "cubic-bezier(0.3, 0, 0.8, 0.6)" : SPRING_EASING,
    fill: "none",
  });
}

/** Anything clickable that is not one of the header controls answers with a
 *  tick too: sound on should wake the whole interface, not a quarter of it.
 *  The contributor links are built at runtime from the GitHub API, so this
 *  selector is matched by the delegated hook below rather than bound once. */
const SOUND_TARGETS = [
  ".nav-links a",
  ".footer-links a",
  ".contributors-link",
  ".contributor-link",
  ".button",
  ".text-link",
  ".card-link",
  ".case-link",
  ".run-button",
  ".copy-button",
  ".contributors-retry",
  ".roadmap-filter button",
  ".suggestion",
].join(", ");

/** Restarts the halo ping around a control so a state change is visible even
 *  when the visitor is not listening. */
function ping(target: HTMLElement): void {
  target.classList.remove("is-pinging");
  void target.offsetWidth;
  target.classList.add("is-pinging");
}

export function initControlFeedback(
  scope: ParentNode = document,
  toggle: HTMLElement | null = document.querySelector<HTMLElement>(".sound-toggle"),
): void {
  const controls = Array.from(scope.querySelectorAll<HTMLElement>(".icon-button, .menu-toggle"));

  initSoundToggle(toggle);

  const isSoundToggle = (control: HTMLElement): boolean => control === toggle;

  for (const control of controls) {
    // The sound toggle is itself a control: pressing it must not also fire the
    // tick it is about to enable or disable.
    if (isSoundToggle(control)) {
      control.addEventListener("pointerdown", () => springPress(control, "in"));
      control.addEventListener("pointerup", () => springPress(control, "out"));
      control.addEventListener("pointercancel", () => springPress(control, "out"));
      control.addEventListener("click", () => {
        sound = !sound;
        writeStoredSound(sound);
        syncToggle(control, sound);
        control.classList.toggle("is-on", sound);
        ping(control);
        vibrate(HAPTIC_CONFIRM_MS);
        if (sound) playConfirm();
        else playDismiss();
      });
      continue;
    }

    control.addEventListener("pointerdown", () => springPress(control, "in"));
    control.addEventListener("pointerup", () => springPress(control, "out"));
    control.addEventListener("pointercancel", () => springPress(control, "out"));

    control.addEventListener("click", () => {
      vibrate(HAPTIC_TICK_MS);
      if (sound) playTick();
    });
  }

  if (scope === document) {
    document.addEventListener("click", (event) => {
      if (!sound) return;
      const target = event.target;
      if (!(target instanceof Element)) return;
      // Header controls carry their own handler above; this hook covers the rest.
      if (target.closest(".icon-button, .menu-toggle") !== null) return;
      if (target.closest(SOUND_TARGETS) === null) return;
      vibrate(HAPTIC_TICK_MS);
      playTick();
    });

    /* Hovering a control is the other half of the interaction, so it answers
       too — but only when the pointer genuinely entered the control. */
    let lastHoverAt = 0;

    document.addEventListener("pointerover", (event) => {
      if (!sound) return;
      // Touch and pen synthesise a pointerover immediately before the click,
      // which would double up on the press tick.
      if (event.pointerType !== "mouse") return;
      if (!(event.target instanceof Element)) return;

      const entered = event.target.closest(SOUND_TARGETS);
      if (entered === null) return;

      // pointerover also fires when the cursor moves between a control's own
      // descendants; relatedTarget tells the two cases apart.
      const previous = event.relatedTarget;
      if (previous instanceof Element && entered.contains(previous)) return;

      const now = Date.now();
      if (now - lastHoverAt < HOVER_MIN_GAP_MS) return;
      lastHoverAt = now;
      playHover();
    });

    document.addEventListener("animationend", (event) => {
      if (event.animationName !== "control-ping") return;
      if (event.target instanceof Element) event.target.classList.remove("is-pinging");
    });
  }
}
