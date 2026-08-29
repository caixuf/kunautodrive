/**
 * KunQuant 前端 REST API 统一客户端
 * 对接后端 KunAutoDrive /api/v1 契约
 */

export interface ContractInfo {
  symbol: string;
  name: string;
  last_price: number;
  open_interest: number;
  volume: number;
  bid_price1: number;
  ask_price1: number;
}

export interface AccountInfo {
  account_id: string;
  balance: number;
  available: number;
  frozen_margin: number;
  floating_pnl: number;
  today_commission: number;
}

export interface ReconcileResult {
  status: string;
  unmatched_trades: number;
  position_diff_count: number;
  fee_diff_amount: number;
  passed: boolean;
}

const BASE_URL = import.meta.env.VITE_API_BASE || '';

export const quantApi = {
  async getStatus(): Promise<any> {
    const res = await fetch(`${BASE_URL}/api/status`);
    return res.json();
  },

  async getContracts(): Promise<ContractInfo[]> {
    const res = await fetch(`${BASE_URL}/api/v1/contracts`);
    return res.json();
  },

  async getAccounts(): Promise<AccountInfo[]> {
    const res = await fetch(`${BASE_URL}/api/v1/accounts`);
    return res.json();
  },

  async triggerReconciliation(): Promise<ReconcileResult> {
    const res = await fetch(`${BASE_URL}/api/reconcile`);
    return res.json();
  }
};
