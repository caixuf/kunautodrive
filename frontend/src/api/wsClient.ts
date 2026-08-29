/**
 * RFC6455 WebSocket 极速行情与回报流客户端
 */

export type MarketTickHandler = (tick: any) => void;
export type TradeRtnHandler = (trade: any) => void;

export class QuantWsClient {
  private ws: WebSocket | null = null;
  private url: string;
  private tickHandlers: Set<MarketTickHandler> = new Set();
  private tradeHandlers: Set<TradeRtnHandler> = new Set();

  constructor(url?: string) {
    const loc = window.location;
    this.url = url || `ws://${loc.host}/ws`;
  }

  connect() {
    this.ws = new WebSocket(this.url);

    this.ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.topic && msg.topic.startsWith('market/tick/')) {
          this.tickHandlers.forEach(h => h(msg.payload));
        } else if (msg.topic && msg.topic.includes('/trade_rtn')) {
          this.tradeHandlers.forEach(h => h(msg.payload));
        }
      } catch (e) {
        // drop malformed frame
      }
    };

    this.ws.onclose = () => {
      setTimeout(() => this.connect(), 2000); // 自动重连
    };
  }

  onTick(handler: MarketTickHandler) {
    this.tickHandlers.add(handler);
  }

  onTrade(handler: TradeRtnHandler) {
    this.tradeHandlers.add(handler);
  }

  subscribe(topic: string) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ action: 'sub', topic }));
    }
  }
}
