"""Tried and dropped: the physics model with every table trained.

    python3 grey.py <folder of CSVs> <group to leave out> [iters=300] [B=64] [lr=0.01] [res=0]

The equations are bench.c's (needle with an opening stick, pressure heading
for floor + F(needle) x gain(Thx) after a dead time through a lag set by the
heat exchanger and the pressure, heat exchanger heading for a temperature
set by the pressure). Every table and constant is a trainable parameter,
started from bench.c's numbers and fitted by gradient descent through the
simulation, with a valve offset per recording; `res` adds a small network
on the pressure target. Untrained it is the physics model. Trained on the
logs up to 10 Nov 2026 it got worse on the left-out 9 Nov 11:45 run (2.2
mbar rms against 1.1): the tables bend to fit the warm restarts of 5 and 6
Nov, whose heat exchanger the equations do not describe. Kept for the
record; fit.py does not use it.
"""
import os, sys, time
import numpy as np, torch, torch.nn as nn, torch.nn.functional as F
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from logs import load_runs  # noqa: E402

W, CAL = 120, 600
def errs(p, m):
    return dict(rmse=float(np.sqrt(np.mean((m - p) ** 2))), rel=float(np.median(np.abs(m - p) / p) * 100))

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'wasm'))
import fit_bench as fb  # noqa: E402

T = lambda a: torch.tensor(np.asarray(a, dtype=np.float32))

def interp(xk, yk, x):
    i = torch.clamp(torch.bucketize(x.contiguous(), xk) - 1, 0, len(xk) - 2)
    w = torch.clamp((x - xk[i]) / (xk[i + 1] - xk[i]), 0, 1)
    return yk[i] + w * (yk[i + 1] - yk[i])

class Grey(nn.Module):
    FY = [0, 2.0, 2.2, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.0, 3.1, 3.2, 3.3, 3.45, 3.6, 3.7, 4.0, 4.3, 4.6, 5.2, 6, 7, 8, 9.4, 10.7, 12, 13.5, 16, 20, 26, 32, 40, 50]
    GK = [1.5, 2, 2.5, 3, 4, 5, 6, 8, 12, 16, 24, 40, 60, 100]          # K: gain against Thx
    LT = [1.8, 2.0, 2.75, 3.5, 4.5, 5.7, 7, 12, 30, 100]                # K: lag against Thx
    LP = [0.7, 1, 2, 4, 6, 8, 12, 20]                                   # mbar: the most the lag is
    HP = [0.67, 1, 2, 4, 6, 8, 12, 16, 20, 28, 50]                      # mbar: steady Thx
    SK = [2, 5, 10, 20, 40, 100]                                        # K: slow Thx time constant

    def __init__(self, nrec=6, res=0, play=False):
        super().__init__()
        fy = np.array(self.FY, dtype=np.float64); f0 = np.interp(fy, fb.F_Y, fb.F_P)
        inc = np.maximum(np.diff(f0), 0.02)
        self.register_buffer('fy', T(fy))
        self.f_raw = nn.Parameter(T(np.log(np.expm1(inc))))
        self.register_buffer('gk', T(self.GK)); self.g = nn.Parameter(T(-(np.array(self.GK) - 4.0) / fb.KA).clamp(min=-4))
        self.register_buffer('lt', T(self.LT)); self.lagT = nn.Parameter(T(np.log(np.interp(self.LT, fb.LAG_T, fb.LAG_S))))
        self.register_buffer('lp', T(self.LP)); self.lagP = nn.Parameter(T(np.log(np.minimum(np.interp(self.LP, fb.DRY_P, fb.DRY_S), 300))))
        self.register_buffer('hp', T(np.log(self.HP))); self.hss = nn.Parameter(T(np.log(np.interp(self.HP, fb.HX_P, fb.HX_T))))
        self.register_buffer('sk', T(self.SK)); self.taus = nn.Parameter(T(np.log([fb.HXTAU] * len(self.SK))))
        self.floor = nn.Parameter(T(np.log(fb.FLOOR))); self.cap = nn.Parameter(T(np.log(fb.HXCAP))); self.tauf = nn.Parameter(T(np.log(fb.HXFAST)))
        self.stick = nn.Parameter(T([0.37, 0.05]))
        self.delay = nn.Parameter(T([0, 0, 4, 0, 0, 0]))               # logits over 0..5 s of dead time
        self.off = nn.Parameter(torch.zeros(nrec))                      # valve offset per recording, %
        self.res = None
        if res:
            self.res = nn.Sequential(nn.Linear(6, res), nn.Tanh(), nn.Linear(res, res), nn.Tanh(), nn.Linear(res, 2))
            nn.init.zeros_(self.res[-1].weight); nn.init.zeros_(self.res[-1].bias)

    def F(self, y):
        inc = F.softplus(self.f_raw)
        seg = torch.clamp((y[:, None] - self.fy[:-1]) / (self.fy[1:] - self.fy[:-1]), 0, 1)
        return (seg * inc).sum(-1)

    def forward(self, v, p_log, a_log, warm=W, own_hx=True, eps=0.05):
        """v (B, T) valve % (offset already added), p_log, a_log (B, T) logged
        pressure and Thx: inputs for the first `warm` s only (a_log all the
        way if own_hx is False). Returns pressure and Thx, (B, T) each."""
        B, n = v.shape
        y = v[:, 0]; p = p_log[:, 0]; a = a_log[:, 0]
        cap = self.cap.exp(); floor = self.floor.exp(); wd = torch.softmax(self.delay, 0)
        hf = torch.minimum(a, cap); hs = a - hf
        buf = [None] * 6; P, A = [p], [a]; s0, s1 = self.stick.abs()
        vprev = v[:, 0]
        for t in range(1, n):
            x = v[:, t]
            f = torch.clamp((y - 5) / 5, 0, 1); st = s0 * (1 - f) + s1 * f
            gap = x - y
            soft = torch.sigmoid((gap - st) / eps)                     # slips once the gap passes the stick:
            slip = soft + ((gap > st).float() - soft).detach()         # hard in the forward pass, soft for the gradient
            y = torch.where(gap < 0, x, y + slip * gap)
            lg = interp(self.gk, self.g, a)
            tgt = self.F(y) * torch.exp(lg)
            ltau = torch.minimum(interp(self.lt, self.lagT, a), interp(self.lp, self.lagP, p))
            if self.res is not None:
                feat = torch.stack([y / 10, (torch.clamp(y, 0, 8) - 4) / 2, torch.log(a) - 1.6, torch.log(p) - 2, torch.clamp(x - vprev, -1, 1) * 5, (x - y) * 3], -1)
                r = self.res(feat)
                tgt = tgt * torch.exp(torch.tanh(r[:, 0])); ltau = ltau + torch.tanh(r[:, 1])
            tgt = floor + tgt
            buf = [tgt] + buf[:-1]
            d = sum(wd[k] * (buf[k] if buf[k] is not None else tgt) for k in range(6))
            p = torch.clamp(p + (d - p) * (1 - torch.exp(-torch.exp(-ltau))), min=0.05)
            Tss = torch.exp(interp(self.hp, self.hss, torch.log(p)))
            Tf = torch.minimum(Tss, cap)
            hf = hf + (Tf - hf) * (1 - torch.exp(-torch.exp(-self.tauf)))
            hs = hs + (Tss - Tf - hs) * (1 - torch.exp(-torch.exp(-interp(self.sk, self.taus, a))))
            a = torch.clamp(hf + hs, min=0.8)
            if t < warm:
                p = p_log[:, t]; a = a_log[:, t]; hf = torch.minimum(a, cap); hs = a - hf
            elif not own_hx:
                a = a_log[:, t]
            P.append(p); A.append(a); vprev = x
        return torch.stack(P, 1), torch.stack(A, 1)


def prep(runs):
    for r in runs:
        r['pf'] = np.maximum(r['p'], 0.05).astype(np.float32); r['af'] = np.where(np.isnan(r['A']), 5.0, r['A']).astype(np.float32); r['vf'] = r['v'].astype(np.float32)
    return runs

def sampler(runs, L):
    items, w = [], []
    for i, r in enumerate(runs):
        n = len(r['t']) - L
        if n <= 0: continue
        lp = np.log(r['pf']); act = np.abs(np.diff(lp, prepend=lp[0])) / 1.5 + 0.05 * np.abs(np.diff(r['vf'], prepend=r['vf'][0]))
        c = np.concatenate([[0], np.cumsum(act)]); a = c[L:L + n] - c[:n]
        for s in range(0, n, 10):
            items.append((i, s)); w.append(0.3 + a[s])
    w = np.array(w); return items, w / w.sum()

def batch(runs, items, w, L, B, rng):
    idx = rng.choice(len(items), B, p=w)
    g = lambda k: torch.from_numpy(np.stack([runs[items[j][0]][k][items[j][1]:items[j][1] + L] for j in idx]))
    return g('vf'), g('pf'), g('af'), torch.from_numpy(np.array([runs[items[j][0]]['rec'] for j in idx]))

def loss_fn(P, A, p, a, warm=W):
    ep = F.huber_loss(torch.log(P[:, warm:]) / 1.5, torch.log(p[:, warm:]) / 1.5, delta=0.3)
    ea = F.huber_loss(torch.log(A[:, warm:]) / 1.2, torch.log(a[:, warm:]) / 1.2, delta=0.3)
    return ep + 0.4 * ea

def calibrate(net, r):
    dvs = torch.arange(-0.8, 0.8001, 0.05); n = len(dvs)
    v = torch.from_numpy(r['vf'][:CAL])[None] + dvs[:, None]
    p = torch.from_numpy(r['pf'][:CAL])[None].expand(n, -1); a = torch.from_numpy(r['af'][:CAL])[None].expand(n, -1)
    with torch.no_grad():
        P, _ = net(v, p, a)
        e = ((torch.log(P[:, W:]) - torch.log(p[:, W:])) ** 2).mean(1) / 1.5 ** 2 + 1e-4 * dvs.abs()
    return float(dvs[e.argmin()])

def rollout(net, r, dv):
    with torch.no_grad():
        P, A = net(torch.from_numpy(r['vf'])[None] + dv, torch.from_numpy(r['pf'])[None], torch.from_numpy(r['af'])[None])
    return np.stack([P[0].numpy(), A[0].numpy()], -1)

def train(folder, fold, iters=300, res=0, B=64, seed=0, log=lambda *a: print(*a, flush=True), H0=240, H1=900, lr=0.01, init=None, wd=0.0):
    torch.manual_seed(seed); rng = np.random.default_rng(seed); torch.set_num_threads(1)
    runs = prep(load_runs(folder)[0])
    tr = [r for r in runs if r['group'] != fold]; te = [r for r in runs if r['group'] == fold]
    net = Grey(nrec=1 + max(r['rec'] for r in runs), res=res)
    if init: net.load_state_dict(torch.load(init), strict=False)
    phys = [q for n, q in net.named_parameters() if not n.startswith('res')]; resp = [q for n, q in net.named_parameters() if n.startswith('res')]
    opt = torch.optim.Adam([{'params': phys, 'lr': lr}, {'params': resp, 'lr': lr / 5, 'weight_decay': wd}])
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, [lr, lr / 5], total_steps=iters, pct_start=0.1)
    cache = {}; t0 = time.time()
    for it in range(iters):
        H = int(H0 + (H1 - H0) * min(1, it / (0.5 * iters))); L = ((W + H) // 60) * 60
        if L not in cache: cache[L] = sampler(tr, L)
        v, p, a, rec = batch(tr, *cache[L], L, B, rng)
        P, A = net(v + net.off[rec][:, None], p, a); loss = loss_fn(P, A, p, a) + 0.01 * (net.off ** 2).mean()
        if not torch.isfinite(loss): log('skip nan'); continue
        opt.zero_grad(); loss.backward(); torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0); opt.step(); sched.step()
        if it % 50 == 0 or it == iters - 1:
            log('fold %d it %4d H %4d loss %.4f  %.0f s  offsets %s stick %s' % (fold, it, H, loss.item(), time.time() - t0, np.round(net.off.detach().numpy(), 2), np.round(net.stick.detach().numpy(), 2)))
    return net, te

def evaluate(net, te, log=print):
    out = []
    for r in te:
        if len(r['t']) < CAL + 600 or r['v'].max() == 0: continue
        dv = calibrate(net, r); m = rollout(net, r, dv); e = errs(r['p'][CAL:], m[CAL:, 0])
        log('  held-out %s n %5d offset %+.2f rmse %.2f mbar, median rel %.0f %%, HX rmse %.2f K' % (r['name'], len(r['t']), dv, e['rmse'], e['rel'], np.sqrt(np.nanmean((m[CAL:, 1] - r['A'][CAL:]) ** 2))))
        out.append((r['name'], m, dv))
    return out

if __name__ == '__main__':
    kw = {k: (v if k == 'init' else float(v) if '.' in v else int(v)) for k, v in (a.split('=') for a in sys.argv[3:])}
    net, te = train(sys.argv[1], int(sys.argv[2]), **kw)
    evaluate(net, te)
