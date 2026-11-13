"""Tried and dropped: a free-form recurrent network (GRU) for the cryostat.

    python3 gru.py <folder of CSVs> <group to leave out> [iters=1500] [hid=64] [zdim=0]

Each second it takes the valve and its own last pressure, heat exchanger and
cold head readings and gives the next ones; for the first W s of a window it
is fed the logged readings. It learns a valve offset per recording. On the
logs up to 10 Nov 2026 it fitted the training runs to a few percent and the
left-out 9 Nov 11:45 run (2 to 24 mbar autotune) to 1.9 to 5.4 mbar rms,
against 1.1 mbar for the physics model: with a handful of recordings it
memorises them. Kept for the record; fit.py does not use it.
"""
import os, sys, time
import numpy as np, torch, torch.nn as nn, torch.nn.functional as F
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from logs import load_runs  # noqa: E402

W, CAL = 120, 600
def errs(p, m):
    return dict(rmse=float(np.sqrt(np.mean((m - p) ** 2))), rel=float(np.median(np.abs(m - p) / p) * 100))


MU = np.array([np.log(8.0), np.log(5.0), np.log(4.0)]); SD = np.array([1.5, 1.2, 0.5])

def to_y(p, A, D):
    return ((np.log(np.stack([p, A, D], -1)) - MU) / SD).astype(np.float32)

def from_y(y):
    return np.exp(y * SD + MU)

def vfeat(v, vprev):
    return torch.stack([v / 10, (torch.clamp(v, 0, 8) - 4) / 2, torch.clamp(v - vprev, -1, 1) * 5], -1)

class Net(nn.Module):
    def __init__(self, hid=64, zdim=8):
        super().__init__()
        self.hid, self.zdim = hid, zdim
        self.cell = nn.GRUCell(3 + 3 + zdim, hid)
        self.out = nn.Sequential(nn.Linear(hid, hid), nn.Tanh(), nn.Linear(hid, 3))
        self.toz = nn.Linear(hid, zdim)

    def forward(self, v, y, warm=W, tf_noise=0.0):
        """v: (B, T) valve %, y: (B, T, 3) logged readings (only the first
        `warm` steps are used as inputs). Returns the predictions (B, T, 3):
        one step ahead in the warm-up, free-running after it."""
        B, T = v.shape
        h = torch.zeros(B, self.hid); z = torch.zeros(B, self.zdim)
        prev = y[:, 0]; out = [y[:, 0]]
        for t in range(1, T):
            x = torch.cat([vfeat(v[:, t], v[:, t - 1]), prev, z], -1)
            h = self.cell(x, h)
            pred = prev + 0.1 * self.out(h)
            out.append(pred)
            if t < warm:
                prev = y[:, t]
                if tf_noise: prev = prev + tf_noise * torch.randn_like(prev)
            else:
                prev = pred
            if t == warm - 1 and self.zdim:
                z = self.toz(h)
        return torch.stack(out, 1)



def prep(runs):
    for r in runs:
        A = np.where(np.isnan(r['A']), 5.0, r['A']); D = np.where(np.isnan(r['D']), 4.0, r['D'])
        r['y'] = to_y(np.maximum(r['p'], 0.05), A, D); r['vf'] = r['v'].astype(np.float32)
    return runs

def sampler(runs, L):
    """Window starts, weighted towards windows where something happens."""
    items, w = [], []
    for i, r in enumerate(runs):
        n = len(r['t']) - L
        if n <= 0: continue
        lp = r['y'][:, 0]; act = np.abs(np.diff(lp, prepend=lp[0])) + 0.05 * np.abs(np.diff(r['vf'], prepend=r['vf'][0]))
        c = np.concatenate([[0], np.cumsum(act)])
        a = c[L:L + n] - c[:n]
        for s in range(0, n, 10):
            items.append((i, s)); w.append(0.3 + a[s])
    w = np.array(w); w /= w.sum()
    return items, w

def batch(runs, items, w, L, B, rng, aug):
    idx = rng.choice(len(items), B, p=w)
    v = np.stack([runs[items[k][0]]['vf'][items[k][1]:items[k][1] + L] for k in idx])
    y = np.stack([runs[items[k][0]]['y'][items[k][1]:items[k][1] + L] for k in idx])
    rec = np.array([runs[items[k][0]]['rec'] for k in idx])
    v = v + rng.uniform(-aug, aug, (B, 1)).astype(np.float32)
    return torch.from_numpy(v), torch.from_numpy(y), torch.from_numpy(rec)

WTS = torch.tensor([1.0, 0.4, 0.15])
def loss_fn(pred, y, warm):
    e = F.huber_loss(pred, y, reduction='none', delta=0.3) * WTS
    return e[:, warm:].mean() + 0.3 * e[:, 1:warm].mean()

def calibrate(net, r):
    """The valve offset for a run the model has not seen: the one that fits its first CAL s best."""
    dvs = torch.arange(-0.8, 0.8001, 0.05)
    v = torch.from_numpy(r['vf'][:CAL])[None] + dvs[:, None]; y = torch.from_numpy(r['y'][:CAL])[None].expand(len(dvs), -1, -1)
    with torch.no_grad():
        pred = net(v, y)
        e = ((pred[:, W:, 0] - y[:, W:, 0]) ** 2).mean(1) + 1e-4 * dvs.abs()
    return float(dvs[e.argmin()])

def rollout(net, r, dv):
    with torch.no_grad():
        pred = net(torch.from_numpy(r['vf'])[None] + dv, torch.from_numpy(r['y'])[None])[0].numpy()
    return from_y(pred)

def train(folder, fold, iters=1500, hid=64, zdim=0, B=32, seed=0, log=print, H0=240, H1=900, aug=0.05):
    torch.manual_seed(seed); rng = np.random.default_rng(seed); torch.set_num_threads(1)
    runs = prep(load_runs(folder)[0])
    tr = [r for r in runs if r['group'] != fold]; te = [r for r in runs if r['group'] == fold]
    net = Net(hid, zdim); off = torch.nn.Parameter(torch.zeros(1 + max(r['rec'] for r in runs)))
    opt = torch.optim.Adam([{'params': net.parameters()}, {'params': [off], 'lr': 5e-3}], 2e-3)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, [4e-3, 1e-2], total_steps=iters, pct_start=0.1)
    cache = {}; t0 = time.time()
    for it in range(iters):
        H = int(H0 + (H1 - H0) * min(1, it / (0.6 * iters))); L = ((W + H) // 60) * 60
        if L not in cache: cache[L] = sampler(tr, L)
        v, y, rec = batch(tr, *cache[L], L, B, rng, aug)
        pred = net(v + off[rec][:, None], y, tf_noise=0.02); loss = loss_fn(pred, y, W) + 0.01 * (off ** 2).mean()
        opt.zero_grad(); loss.backward(); torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0); opt.step(); sched.step()
        if it % 200 == 0 or it == iters - 1:
            log('fold %d it %4d H %4d loss %.4f  %.0f s  offsets %s' % (fold, it, H, loss.item(), time.time() - t0, np.round(off.detach().numpy(), 2)))
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
    kw = {k: (float(v) if '.' in v else int(v)) for k, v in (a.split('=') for a in sys.argv[3:])}
    net, te = train(sys.argv[1], int(sys.argv[2]), **kw)
    evaluate(net, te)
