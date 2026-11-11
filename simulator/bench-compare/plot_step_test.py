# Each step of a GUI step test session CSV, one panel per step: the bench
# against simulator runs of the same steps.
#   python3 plot_step_test.py out.png bench.csv "label=sim.csv" ... [--title="..."]
import csv, sys
import numpy as np
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
# usage: plot_steps.py out.png bench.csv [label=sim.csv ...]
TITLE='Setpoint step test 11 Nov 17:26, each step: bench against the simulator. Grey band 5 % of the setpoint; valve % and heat exchanger K (red) below'
for a in [a for a in sys.argv if a.startswith('--title=')]:
    TITLE=a[8:]; sys.argv.remove(a)
out=sys.argv[1]
def load(path):
    rows=list(csv.DictReader(open(path)))
    def f(x):
        try: return float(x)
        except: return np.nan
    d={k:np.array([f(r.get(c)) for r in rows]) for k,c in [('t','t_s'),('sp','sp_mbar'),('p','p_mbar'),('v','valve_pct'),('hx','A_heat_exchanger_K')]}
    d['stage']=[r['stage'] for r in rows]
    segs=[]; i=0; n=len(rows)
    while i<n:
        s=d['stage'][i]
        if s.startswith('step test:') and '->' in s:
            j=i
            while j<n and d['stage'][j]==s: j+=1
            k=i
            while k<j and d['sp'][k]==d['sp'][i-1]: k+=1
            segs.append((s[11:].replace(' mbar',''),k,j)); i=j
        else: i+=1
    return d,segs
bench,bsegs=load(sys.argv[2])
sims=[]
for a in sys.argv[3:]:
    lab,path=a.split('=',1); sims.append((lab,)+load(path))
# bench run has 48->24 twice: keep the full second one
seen={}; order=[]
for name,a,b in bsegs:
    if name in seen and name=='48 -> 24':
        order[seen[name]]=(name,a,b)
    elif name not in seen:
        seen[name]=len(order); order.append((name,a,b))
    else:
        order.append((name,a,b))
# bench sequence order: run as 8>12,12>16,16>24,24>48,48>24,24>16,16>12,12>8,8>4,4>2,2>4,4>8
names=['8 -> 12','12 -> 16','16 -> 24','24 -> 48','48 -> 24','24 -> 16','16 -> 12','12 -> 8','8 -> 4','4 -> 2','2 -> 4','4 -> 8']
bmap={}
for name,a,b in bsegs:
    if name=='48 -> 24' and b-a<100: continue
    bmap.setdefault(name,(a,b))
fig=plt.figure(figsize=(18,22))
gs=GridSpec(8,3,figure=fig,height_ratios=[3,1.3]*4,hspace=0.45,wspace=0.22)
cols=['#1f5fa8','#d97b00','#2a9d55','#8a3fb0']
for n,name in enumerate(names):
    r,c=divmod(n,3)
    axp=fig.add_subplot(gs[2*r,c]); axv=fig.add_subplot(gs[2*r+1,c],sharex=axp)
    a,b=bmap[name]
    t0=bench['t'][a]; sp1=bench['sp'][a]; tt=bench['t'][a-30:b]-t0
    band=max(0.05*sp1,0.1)
    axp.axhspan(sp1-band,sp1+band,color='0.9')
    axp.axhline(sp1,color='k',lw=0.8,ls='--')
    axp.plot(tt,bench['p'][a-30:b],color=cols[0],lw=1.6,label='bench 17:26')
    axv.plot(tt,bench['v'][a-30:b],color=cols[0],lw=1.4)
    ax2=axv.twinx(); ax2.plot(tt,bench['hx'][a-30:b],color='#c0392b',lw=1,alpha=0.8); ax2.tick_params(axis='y',colors='#c0392b',labelsize=8)
    if c==2: ax2.set_ylabel('HX, K',color='#c0392b',fontsize=9)
    for k,(lab,d,segs) in enumerate(sims):
        for nm,sa,sb in segs:
            if nm==name:
                st=d['t'][sa-30:sb]-d['t'][sa]
                axp.plot(st,d['p'][sa-30:sb],color=cols[k+1],lw=1.2,ls='--' if k==0 else ':',label=lab)
                axv.plot(st,d['v'][sa-30:sb],color=cols[k+1],lw=1.0,ls='--' if k==0 else ':')
                break
    axp.set_title(name+' mbar',fontsize=12)
    axp.grid(alpha=0.4); axv.grid(alpha=0.4)
    axp.set_xlim(-30,370)
    if c==0: axp.set_ylabel('mbar'); axv.set_ylabel('valve %')
    if r==3: axv.set_xlabel('s from the setpoint change')
    if n==0: axp.legend(fontsize=9,loc='lower right')
fig.suptitle(sys.argv[0] and TITLE,fontsize=14,y=0.905)
plt.savefig(out,dpi=90,bbox_inches='tight')
