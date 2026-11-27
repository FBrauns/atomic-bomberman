// The virtual network of the browser build (used by src/net/socket_web.cpp).
//
// The game's UDP sockets are handles here. A datagram to 127.x goes to another socket of
// this page; a datagram to 10.x.y.z goes over the WebRTC data channel of the player that
// address was given to. Channels are opened with PeerJS: a page that hosts a game takes
// the peer id "abm-web-<ROOM CODE>", a page that joins connects to it. The game's own
// link layer (sequence numbers, repeats, encryption) runs on top, so an unreliable,
// unordered channel is what it wants.
(function () {
  const kDiscoveryPort = 27409;
  const kPrefix = 'abm-web-';
  const kAlphabet = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';

  const net = {
    sockets: new Map(),   // handle -> { port, queue: [{ip, port, data}] }
    byPort: new Map(),    // port -> Set(handle)
    nextHandle: 1,
    nextEphemeral: 49152,
    peer: null,           // our PeerJS peer
    peerReady: null,      // promise of it being open
    room: null,           // our room code while hosting
    links: new Map(),     // virtual ip -> { conn, open, pending: [Uint8Array], peerId }
    ipOf: new Map(),      // peer id -> virtual ip
    nextIp: 1,

    // --- sockets ---------------------------------------------------------------
    open(port, shared) {
      if (port === 0) {
        do { port = this.nextEphemeral++; if (this.nextEphemeral > 65000) this.nextEphemeral = 49152; }
        while (this.byPort.has(port));
      } else if (this.byPort.has(port) && !shared) {
        return -1;  // in use
      }
      const handle = this.nextHandle++;
      this.sockets.set(handle, { port, queue: [] });
      if (!this.byPort.has(port)) this.byPort.set(port, new Set());
      this.byPort.get(port).add(handle);
      // A game server's port (not the LAN search port, not a client's): this page hosts.
      if (port !== kDiscoveryPort && port < 49152) this.host();
      return handle;
    },
    portOf(handle) { const s = this.sockets.get(handle); return s ? s.port : 0; },
    close(handle) {
      const s = this.sockets.get(handle);
      if (!s) return;
      this.sockets.delete(handle);
      const set = this.byPort.get(s.port);
      if (set) { set.delete(handle); if (set.size === 0) this.byPort.delete(s.port); }
    },
    deliver(port, from) {
      const set = this.byPort.get(port);
      if (!set) return;
      for (const h of set) {
        const s = this.sockets.get(h);
        if (s && s.queue.length < 1024) s.queue.push(from);
      }
    },
    send(handle, ip, port, data) {
      const s = this.sockets.get(handle);
      if (!s) return false;
      if ((ip >>> 24) === 127) {  // this page
        this.deliver(port, { ip: 0x7F000001, port: s.port, data });
        return true;
      }
      if (ip === 0xFFFFFFFF) return true;  // LAN broadcast: there is no LAN
      const link = this.links.get(ip);
      if (!link) return true;  // nobody there: lost, as UDP would be
      const packet = new Uint8Array(4 + data.length);
      packet[0] = s.port >> 8; packet[1] = s.port & 255;
      packet[2] = port >> 8; packet[3] = port & 255;
      packet.set(data, 4);
      if (link.open) {
        try { link.conn.send(packet.buffer); } catch (e) { /* closing */ }
      } else if (link.pending.length < 256) {
        link.pending.push(packet.buffer);
      }
      return true;
    },
    receive(handle) {
      const s = this.sockets.get(handle);
      return s && s.queue.length ? s.queue.shift() : null;
    },

    // --- peers -------------------------------------------------------------------
    ensurePeer(id) {
      if (this.peer) return this.peerReady;
      if (typeof Peer === 'undefined') { this.ui.error('The network library did not load (check the connection).'); return null; }
      const options = Object.assign({ debug: 1 }, Module.abPeerOptions || {});
      this.peer = id ? new Peer(id, options) : new Peer(options);
      this.peerReady = new Promise((resolve, reject) => {
        this.peer.on('open', (got) => resolve(got));
        this.peer.on('error', (err) => {
          console.warn('[net] peer error', err.type, err);
          if (err.type === 'unavailable-id' && this.room) {  // the code is taken: another one
            this.peer.destroy(); this.peer = null; this.room = null;
            this.host();
            return;
          }
          if (err.type === 'peer-unavailable') this.ui.error('No game with that room code.');
          else if (err.type === 'network' || err.type === 'server-error' || err.type === 'socket-error')
            this.ui.error('Cannot reach the matchmaking server (' + err.type + ').');
          reject(err);
        });
      });
      this.peer.on('connection', (conn) => this.attach(conn));
      this.peer.on('disconnected', () => { if (this.peer && !this.peer.destroyed) this.peer.reconnect(); });
      this.peerReady.catch(() => {});
      return this.peerReady;
    },
    virtualIp(peerId) {
      let ip = this.ipOf.get(peerId);
      if (ip === undefined) {
        const n = this.nextIp++;
        ip = (0x0A000000 | ((n >> 8) & 255) << 8 | (n & 255)) >>> 0;
        this.ipOf.set(peerId, ip);
      }
      return ip;
    },
    attach(conn) {
      const ip = this.virtualIp(conn.peer);
      const old = this.links.get(ip);
      const link = { conn, open: false, pending: old ? old.pending : [], peerId: conn.peer };
      this.links.set(ip, link);
      conn.on('open', () => {
        link.open = true;
        console.log('[net] channel open with', conn.peer);
        for (const p of link.pending) { try { conn.send(p); } catch (e) {} }
        link.pending = [];
        this.ui.peers();
      });
      conn.on('data', (raw) => {
        const take = (buf) => {
          const b = new Uint8Array(buf);
          if (b.length < 4) return;
          this.deliver((b[2] << 8) | b[3], { ip, port: (b[0] << 8) | b[1], data: b.slice(4) });
        };
        if (raw instanceof ArrayBuffer) take(raw);
        else if (ArrayBuffer.isView(raw)) take(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength));
        else if (raw instanceof Blob) raw.arrayBuffer().then(take);
      });
      conn.on('close', () => {
        if (this.links.get(ip) === link) this.links.delete(ip);
        console.log('[net] channel closed with', conn.peer);
        this.ui.peers();
      });
      conn.on('error', (e) => console.warn('[net] channel error', e));
    },
    makeCode() {
      let c = '';
      const r = new Uint32Array(5);
      crypto.getRandomValues(r);
      for (const v of r) c += kAlphabet[v % kAlphabet.length];
      return c;
    },
    host() {
      if (this.room) return;
      if (this.peer) {  // a peer made for joining a game before: a host needs its room id
        this.peer.destroy();
        this.peer = null;
        this.links.clear();
      }
      this.room = this.makeCode();
      const p = this.ensurePeer(kPrefix + this.room);
      if (p) p.then(() => this.ui.room(this.room)).catch(() => {});
    },
    // A room code to the virtual address of its host; 0 if the text is not a room code.
    join(text) {
      const code = String(text || '').trim().toUpperCase().replace(/[^A-Z0-9]/g, '');
      if (code.length < 4 || code.length > 8) return 0;
      const hostId = kPrefix + code;
      const ip = this.virtualIp(hostId);
      if (this.links.has(ip)) return ip;
      this.links.set(ip, { conn: null, open: false, pending: [], peerId: hostId });
      const p = this.ensurePeer(null);
      if (p) p.then(() => {
        const conn = this.peer.connect(hostId, { reliable: false, serialization: 'raw' });
        this.attach(conn);
      }).catch(() => {});
      return ip;
    },

    // --- what the page shows (web/shell.html fills these in) ------------------------
    ui: {
      room(code) { if (Module.abShowRoom) Module.abShowRoom(code); },
      error(text) { if (Module.abShowNetError) Module.abShowNetError(text); else console.warn(text); },
      peers() { if (Module.abShowPeers) Module.abShowPeers([...net.links.values()].filter((l) => l.open).length); },
    },
  };

  Module.abNet = net;
})();
