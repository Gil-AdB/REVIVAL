// Audio worklet for REVIVAL/FLOOD.
//
// Adapted from Modplayer's modplayer-wasm/www/audio-worklet.js. The
// processor owns a 2-second Float32 ring buffer per channel and pulls
// samples from it on the audio thread (independent of main). The main
// thread tops up the ring via 'audio' messages whenever the worklet
// signals 'needData' / 'starve'.
//
// We swapped this in to replace SDL2's emscripten audio backend, which
// uses ScriptProcessorNode (deprecated, runs the audio callback on
// browser-main, gets starved by heavy rAF ticks at HD).

class FloodModplayerWorklet extends AudioWorkletProcessor {
	constructor() {
		super();
		// 2 seconds at 48 kHz. Way more headroom than the 10 ms
		// ScriptProcessorNode buffer we had before.
		this.bufferLeft  = new Float32Array(96000);
		this.bufferRight = new Float32Array(96000);
		this.writePos = 0;
		this.readPos  = 0;

		this.port.onmessage = (e) => {
			const msg = e.data;
			if (msg.type === 'audio') {
				const left  = msg.left;
				const right = msg.right;
				const len   = this.bufferLeft.length;
				const available = (this.writePos - this.readPos + len) % len;
				// Drop if the chunk would overflow the ring.
				if (available + left.length >= len - 100) return;
				for (let i = 0; i < left.length; i++) {
					this.bufferLeft[this.writePos]  = left[i];
					this.bufferRight[this.writePos] = right[i];
					this.writePos = (this.writePos + 1) % len;
				}
			} else if (msg.type === 'stop') {
				this.writePos = 0;
				this.readPos  = 0;
			}
		};
	}

	process(inputs, outputs) {
		const out = outputs[0];
		if (!out || !out[0] || !out[1]) return true;

		const channelLeft  = out[0];
		const channelRight = out[1];
		const frames = channelLeft.length;
		const len = this.bufferLeft.length;

		let avail = (this.writePos - this.readPos + len) % len;
		if (avail < frames) {
			channelLeft.fill(0);
			channelRight.fill(0);
			this.port.postMessage({ type: 'starve' });
			return true;
		}

		for (let i = 0; i < frames; i++) {
			channelLeft[i]  = this.bufferLeft[this.readPos];
			channelRight[i] = this.bufferRight[this.readPos];
			this.readPos = (this.readPos + 1) % len;
		}

		// Ask main thread for more when the buffer drops below 50 ms
		// (2400 samples @ 48 kHz). Plenty of slack for a heavy rAF tick.
		avail = (this.writePos - this.readPos + len) % len;
		if (avail < 2400) this.port.postMessage({ type: 'needData' });
		return true;
	}
}

registerProcessor('flood-modplayer-worklet', FloodModplayerWorklet);
