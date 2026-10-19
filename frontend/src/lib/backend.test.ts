import { beforeAll, describe, expect, it, vi } from "vitest";

// backend.ts registers its event entry point on `window` when it loads, and
// the tests run in Node, which has none.
let asciiJson: (value: unknown) => string;
beforeAll(async () => {
  vi.stubGlobal("window", {});
  ({ asciiJson } = await import("./backend"));
});

describe("asciiJson", () => {
  it("escapes the DEL that Backspace sends", () => {
    // webview's own parser drops a call holding a raw DEL.
    const text = asciiJson({ data: "\x7f" });
    expect(text).toBe('{"data":"\\u007f"}');
    expect(JSON.parse(text)).toEqual({ data: "\x7f" });
  });

  it("leaves nothing outside printable ASCII", () => {
    const value = { data: "\x1b[A\x00é★😀", list: [1, "Ω"] };
    const text = asciiJson(value);
    expect(text).toMatch(/^[\x20-\x7e]*$/);
    expect(JSON.parse(text)).toEqual(value);
  });

  it("matches JSON.stringify for plain ASCII", () => {
    const value = { profile: { id: "nao", port: 22 }, password: null };
    expect(asciiJson(value)).toBe(JSON.stringify(value));
  });
});
