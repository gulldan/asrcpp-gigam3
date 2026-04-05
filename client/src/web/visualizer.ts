// Canvas frequency bar visualizer

export class Visualizer {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private running = false;

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d')!;
    this.resize();
    window.addEventListener('resize', () => this.resize());
  }

  private resize() {
    const dpr = Math.max(1, Math.min(window.devicePixelRatio || 1, 2));
    const width = Math.max(1, Math.floor(this.canvas.offsetWidth * dpr));
    const height = Math.max(1, Math.floor(this.canvas.offsetHeight * dpr));
    this.canvas.width = width;
    this.canvas.height = height;
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  start(analyser: AnalyserNode, audioContext: AudioContext) {
    this.running = true;
    const draw = () => {
      if (!this.running) return;
      if (audioContext.state === 'suspended') {
        audioContext.resume().catch(() => {});
      }
      requestAnimationFrame(draw);

      const bufferLength = analyser.frequencyBinCount;
      const dataArray = new Uint8Array(bufferLength);
      analyser.getByteFrequencyData(dataArray);

      const w = this.canvas.offsetWidth;
      const h = this.canvas.offsetHeight;
      this.ctx.clearRect(0, 0, w, h);

      const barW = Math.max(1, (w / bufferLength) * 2);
      let x = 0;

      for (let i = 0; i < bufferLength; i++) {
        const barH = (dataArray[i] / 255) * h;
        if (barH < 2) {
          x += barW;
          continue;
        }

        const gradient = this.ctx.createLinearGradient(0, h - barH, 0, h);
        gradient.addColorStop(0, 'rgba(13, 212, 168, 0.85)');
        gradient.addColorStop(0.5, 'rgba(6, 182, 212, 0.45)');
        gradient.addColorStop(1, 'rgba(13, 212, 168, 0.08)');
        this.ctx.fillStyle = gradient;
        this.ctx.fillRect(x, h - barH, Math.max(1, barW - 1), barH);
        x += barW;
      }
    };
    draw();
  }

  stop() {
    this.running = false;
    this.ctx.clearRect(0, 0, this.canvas.offsetWidth, this.canvas.offsetHeight);
  }
}
