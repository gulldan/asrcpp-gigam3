// Canvas frequency bar visualizer

export class Visualizer {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private running = false;

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d')!;
    this.resize();
  }

  private resize() {
    this.canvas.width = this.canvas.offsetWidth * 2;
    this.canvas.height = this.canvas.offsetHeight * 2;
    this.ctx.scale(2, 2);
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
      this.ctx.fillStyle = '#12121a';
      this.ctx.fillRect(0, 0, w, h);

      const barW = (w / bufferLength) * 2;
      let x = 0;
      for (let i = 0; i < bufferLength; i++) {
        const barH = (dataArray[i] / 255) * h;
        const gradient = this.ctx.createLinearGradient(0, h - barH, 0, h);
        gradient.addColorStop(0, '#6366f1');
        gradient.addColorStop(1, '#8b5cf6');
        this.ctx.fillStyle = gradient;
        this.ctx.fillRect(x, h - barH, barW - 1, barH);
        x += barW;
      }
    };
    draw();
  }

  stop() {
    this.running = false;
  }
}
