export class DecodedCoverage {
  constructor(stream, config) {
    this.stream = stream;
    this.config = config;
    this.frameCount = 0;
    this.sampleCount = 0;
    this.coveredSeconds = 0;
    this.maxGapSeconds = 0;
    this.firstTime = undefined;
    this.lastTime = undefined;
    this.end = undefined;
  }
  push(frame) {
    if (!Number.isFinite(frame.time) || (this.lastTime !== undefined && frame.time < this.lastTime - 1e-6)) {
      throw Error('Invalid or backward decoded timestamp');
    }
    let duration;
    if (this.stream.codec_type === 'audio') {
      const rate = Number(this.stream.sample_rate);
      if (!Number.isInteger(frame.samples) || frame.samples <= 0 || !Number.isFinite(rate) || rate <= 0) {
        throw Error('Missing decoded audio samples or sample rate');
      }
      this.sampleCount += frame.samples;
      duration = frame.samples / rate;
    } else {
      // Recorder output cadence is configured CFR. Matroska rounds a 60 FPS
      // frame duration to 16 ms; summing that metadata loses seconds in a long
      // valid recording. Use one configured frame, then measure actual timestamp
      // gaps/overlaps. A large packet duration cannot substitute for frame count.
      duration = 1 / this.config.fps;
    }
    if (!Number.isFinite(duration) || duration <= 0) throw Error('Invalid decoded frame duration');
    this.firstTime ??= frame.time;
    if (this.end !== undefined) this.maxGapSeconds = Math.max(this.maxGapSeconds, frame.time - this.end);
    // Union of frame intervals: duplicates and overlaps cannot inflate coverage.
    const nextEnd = frame.time + duration;
    this.coveredSeconds += Math.max(0, nextEnd - Math.max(frame.time, this.end ?? frame.time));
    this.end = Math.max(this.end ?? nextEnd, nextEnd);
    this.lastTime = frame.time;
    this.frameCount++;
  }
  verify(seconds) {
    if (!this.frameCount || this.coveredSeconds < seconds - 5) throw Error('Insufficient decoded coverage');
    if (this.maxGapSeconds > .25 || this.firstTime > .25) throw Error('Decoded timestamp gap exceeds 250 ms');
    return this.stats();
  }
  stats() {
    return {frameCount: this.frameCount, sampleCount: this.sampleCount, coveredSeconds: this.coveredSeconds,
      firstTimestamp: this.firstTime, endTimestamp: this.end, maxGapSeconds: this.maxGapSeconds};
  }
}
