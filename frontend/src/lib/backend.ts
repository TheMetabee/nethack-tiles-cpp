/**
 * Typed wrappers over the C++ backend's command and event surface.
 *
 * The backend hosts this page in a system webview and binds one function,
 * `window.__nh_invoke(command, argsJson)`, which resolves with the command's
 * JSON result. `argsJson` is the arguments as JSON text; see `asciiJson`. Events go the other way: the backend calls `window.__nh_emit(event,
 * payload)`, and the listeners registered here receive the payload.
 */

import type {
  Profile,
  ServerVersion,
  Status,
  StreamItem,
  TilesetManifest,
  TilesetPayload,
} from "./protocol";

type Invoke = (command: string, argsJson: string) => Promise<unknown>;
type Listener = (payload: unknown) => void;
export type UnlistenFn = () => void;

declare global {
  interface Window {
    __nh_invoke?: Invoke;
    __nh_emit?: (event: string, payload: unknown) => void;
  }
}

export const EVENTS = {
  stream: "nh://stream",
  status: "nh://status",
  tiledataSeen: "nh://tiledata-seen",
  serverVersion: "nh://server-version",
} as const;

const listeners = new Map<string, Set<Listener>>();

window.__nh_emit = (event, payload) => {
  listeners.get(event)?.forEach((fn) => {
    try {
      fn(payload);
    } catch (error) {
      void reportFrontendError(`listener for ${event} threw: ${error}`);
    }
  });
};

/**
 * JSON text with every character outside printable ASCII written as `\uXXXX`.
 *
 * webview checks each call with a small JSON parser of its own before the
 * backend sees it, and that parser rejects a raw DEL (0x7f), which
 * `JSON.stringify` leaves as it is. DEL is what Backspace sends, so a plain
 * call lost every Backspace. Escaped, the call is plain ASCII, and the
 * backend's own parser reads the escapes back.
 */
export function asciiJson(value: unknown): string {
  return JSON.stringify(value).replace(
    /[\u007f-￿]/g,
    (c) => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"),
  );
}

/**
 * Calls a backend command. Without the bridge (the page opened in a plain
 * browser) every command rejects instead of throwing, so callers that already
 * handle a failed command handle this too.
 */
function invoke<T>(command: string, args: Record<string, unknown> = {}): Promise<T> {
  const bridge = window.__nh_invoke;
  if (!bridge) return Promise.reject(new Error(`no backend for ${command}`));
  return bridge(command, asciiJson(args)) as Promise<T>;
}

export const listProfiles = () => invoke<Profile[]>("list_profiles");
export const lastUsedProfile = () => invoke<string | null>("last_used_profile");

/** `password` of `null` leaves any stored password untouched. */
export const saveProfile = (profile: Profile, password: string | null) =>
  invoke<void>("save_profile", { profile, password });

export const deleteProfile = (id: string) => invoke<void>("delete_profile", { id });
export const hasSavedPassword = (id: string) =>
  invoke<boolean>("has_saved_password", { id });

export const listTilesets = () => invoke<TilesetManifest[]>("list_tilesets");
export const getTileset = (id: string) => invoke<TilesetPayload>("get_tileset", { id });
export const addCustomTileset = (manifest: TilesetManifest, path: string) =>
  invoke<TilesetPayload>("add_custom_tileset", { manifest, path });

export const sessionConnect = (profileId: string, cols: number, rows: number) =>
  invoke<void>("session_connect", { request: { profileId, cols, rows } });

export const sessionWrite = (data: string) => invoke<void>("session_write", { data });
/** For bytes no UTF-8 string can carry, such as NetHack's meta commands. */
export const sessionWriteBytes = (bytes: number[]) =>
  invoke<void>("session_write_bytes", { bytes });
export const sessionResize = (cols: number, rows: number) =>
  invoke<void>("session_resize", { cols, rows });
export const sessionDisconnect = () => invoke<void>("session_disconnect");

/** Replaces the known files in a state-log directory. Unknown names are refused. */
export const writeStateLog = (directory: string, files: Record<string, string>) =>
  invoke<void>("state_log_write", { request: { directory, files } });

/** Asks the backend for a native folder picker. Resolves `null` on cancel. */
export const pickFolder = (title: string) =>
  invoke<string | null>("pick_folder", { title });

/**
 * Hands a link to the desktop browser.
 *
 * A bare `<a href>` would navigate the webview itself, replacing the game with
 * a web page and no way back, so every outbound link goes through here.
 */
export const openExternal = (url: string) =>
  invoke<void>("open_url", { url }).catch((error) => {
    void reportFrontendError(`openUrl(${url}) failed: ${error}`);
  });

/** Sends a frontend failure to the backend log, where it is actually visible. */
export const reportFrontendError = (message: string) =>
  invoke<void>("log_frontend_error", { message }).catch(() => {
    // Nothing more we can do; at least it reached the browser console.
    console.error(message);
  });

function subscribe<T>(event: string, fn: (payload: T) => void): Promise<UnlistenFn> {
  const listener = fn as Listener;
  let set = listeners.get(event);
  if (!set) {
    set = new Set();
    listeners.set(event, set);
  }
  set.add(listener);
  return Promise.resolve(() => {
    listeners.get(event)?.delete(listener);
  });
}

export const onStream = (fn: (items: StreamItem[]) => void) =>
  subscribe<StreamItem[]>(EVENTS.stream, fn);

export const onStatus = (fn: (status: Status) => void) =>
  subscribe<Status>(EVENTS.status, fn);

export const onTiledataSeen = (fn: () => void) =>
  subscribe<unknown>(EVENTS.tiledataSeen, () => fn());

export const onServerVersion = (fn: (found: ServerVersion) => void) =>
  subscribe<ServerVersion>(EVENTS.serverVersion, fn);
