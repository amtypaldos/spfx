// Web Serial transport for the real SP-1 (desktop Chromium). UNTESTED against hardware.
// Implements the Client transport contract: write(), onData, onClose, close().

export const serialSupported = () => typeof navigator !== 'undefined' && 'serial' in navigator;

export class WebSerialTransport {
  constructor(port) {
    this.port = port; this.onData = null; this.onClose = null;
    this.writer = null; this.reader = null; this.closing = false;
  }

  /** Prompts the user to pick a port (must be called from a user gesture). */
  static async request() {
    if (!serialSupported()) throw new Error('Web Serial is not available in this browser.');
    const port = await navigator.serial.requestPort();
    return new WebSerialTransport(port);
  }

  async open() {
    await this.port.open({ baudRate: 115200 });   // baud rate is irrelevant on CDC-ACM
    this.writer = this.port.writable.getWriter();
    this._readLoop();
    this._onDisconnect = (e) => { if (e.target === this.port) this._finish(); };
    navigator.serial.addEventListener('disconnect', this._onDisconnect);
  }

  async _readLoop() {
    try {
      while (this.port.readable && !this.closing) {
        this.reader = this.port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) break;
            if (value?.length) this.onData?.(value);
          }
        } finally { this.reader.releaseLock(); }
      }
    } catch { /* port lost or read error: fall through to close handling */ }
    this._finish();
  }

  async write(bytes) { await this.writer.write(bytes); }

  _finish() {
    if (this._done) return;
    this._done = true;
    navigator.serial.removeEventListener('disconnect', this._onDisconnect);
    this.onClose?.();
  }

  async close() {
    this.closing = true;
    try { await this.reader?.cancel(); } catch { /* ignore */ }
    try { this.writer?.releaseLock(); } catch { /* ignore */ }
    try { await this.port.close(); } catch { /* ignore */ }
    this._finish();
  }
}
