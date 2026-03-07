import { describe, test, expect } from "bun:test";
import { RealtimeClient } from "./realtime-client";

describe("RealtimeClient", () => {
  test("constructor sets defaults", () => {
    const client = new RealtimeClient({ url: "ws://localhost:9999/v1/realtime" });
    expect(client.connected).toBe(false);
    expect(client.eventsSent).toBe(0);
    expect(client.eventsReceived).toBe(0);
  });

  test("send returns false when not connected", () => {
    const client = new RealtimeClient({ url: "ws://localhost:9999/v1/realtime" });
    expect(client.appendAudio("AAAA")).toBe(false);
    expect(client.commit()).toBe(false);
    expect(client.clear()).toBe(false);
  });

  test("close is safe when not connected", () => {
    const client = new RealtimeClient({ url: "ws://localhost:9999/v1/realtime" });
    expect(() => client.close()).not.toThrow();
  });

  test("connect rejects on bad url", async () => {
    const client = new RealtimeClient({
      url: "ws://127.0.0.1:1/bad",
      connectTimeoutMs: 500,
    });
    await expect(client.connect()).rejects.toThrow();
  });
});
