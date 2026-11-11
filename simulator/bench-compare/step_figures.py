# Rise (10 to 90 %), settling (5 % band), overshoot, end error and valve
# kick of each step in a GUI step test session CSV; table as written in it.
#   python3 step_figures.py session.csv out.json
import csv, sys, json
import numpy as np
rows=list(csv.DictReader(open(sys.argv[1])))
def f(x):
    if x is None: return np.nan
    try: return float(x)
    except: return np.nan
t=np.array([f(r['t_s']) for r in rows]); sp=np.array([f(r['sp_mbar']) for r in rows]); p=np.array([f(r['p_mbar']) for r in rows])
v=np.array([f(r['valve_pct']) for r in rows]); hx=np.array([f(r.get('A_heat_exchanger_K')) for r in rows])
mode=[r['mode'] for r in rows]; stage=[r['stage'] for r in rows]
SETS={1:(0.06,0.70),2:(0.14,1.50),3:(0.30,0.25)}
B1,B2,H=6.0,14.0,0.10
def pick(now,x):
    if now not in (1,2,3): return 1 if x<B1 else 2 if x<B2 else 3
    if now==1 and x>B1*(1+H): now=2
    if now==2 and x>B2*(1+H): now=3
    if now==3 and x<B2*(1-H): now=2
    if now==2 and x<B1*(1-H): now=1
    return now
# set trace over AUTO
st=np.zeros(len(t),int); cur=0
for i in range(len(t)):
    if mode[i]=='AUTO' and not np.isnan(p[i]):
        cur=pick(cur if (i>0 and mode[i-1]=='AUTO') else 0, min(p[i],sp[i])); st[i]=cur
    else: cur=0
# segments: where stage starts with 'step test: ' and contains '->' ; key by sp change
segs=[]
i=0
n=len(t)
while i<n:
    if stage[i].startswith('step test:') and '->' in stage[i]:
        j=i
        while j<n and stage[j]==stage[i]: j+=1
        # the setpoint changes 0-1 rows after the stage label
        k=i
        while k<j and sp[k]==sp[i-1]: k+=1
        segs.append((stage[i][11:],k,j))
        i=j
    else: i+=1
out=[]
for name,a,b in segs:
    tt=t[a:b]-t[a]; pp=p[a:b]; vv=v[a:b]; s1=sp[a]; p0=p[a-1]; d=s1-p0
    frac=(pp-p0)/d
    def first(x):
        k=np.argmax(frac>=x); return tt[k] if frac[k]>=x else None
    t10,t90,t63=first(0.1),first(0.9),first(0.63)
    band=max(0.05*s1,0.1)
    inb=np.abs(pp-s1)<=band
    # settling: first time after which it stays in band to the end
    settle=None
    if inb[-1]:
        k=len(inb)-1
        while k>0 and inb[k-1]: k-=1
        settle=tt[k]
    band2=0.02*s1; inb2=np.abs(pp-s1)<=band2; settle2=None
    if inb2[-1]:
        k=len(inb2)-1
        while k>0 and inb2[k-1]: k-=1
        settle2=tt[k]
    peak=np.max(frac); over=max(0,(peak-1))*100
    tpk=tt[np.argmax(frac)] if peak>1 else None
    last=tt>=tt[-1]-30
    ess=np.mean(pp[last]-s1)
    # valve rate last 60 s
    l60=tt>=tt[-1]-60
    vr=np.polyfit(tt[l60],vv[l60],1)[0]*60
    sets=sorted(set(st[a:b].tolist()))
    # integral-only rate predicted for the final error with the set at the end
    kp,ti=SETS[st[b-1]]
    pred=-kp*ess/ti if True else 0
    # rate of p last 60 s
    pr=np.polyfit(tt[l60],pp[l60],1)[0]*60
    out.append(dict(step=name,dur=round(tt[-1]),p0=round(p0,2),sp=s1,d=round(d,2),sets=sets,kick=round(vv[min(3,len(vv)-1)]-v[a-1],2),
        t10=t10,t63=t63,t90=t90,rise=(round(t90-t10) if (t10 is not None and t90 is not None) else None),
        settle5=settle,settle2=settle2,over=round(over,1),tpk=tpk,reached=round(frac[-1]*100),pend=round(pp[-1],2),ess=round(ess,2),ess_pct=round(ess/s1*100,1),
        v0=v[a-1],v1=vv[-1],vrate=round(vr,3),pred_rate=round(pred,3),prate=round(pr,3),hx0=round(hx[a],2),hx1=round(hx[b-1],2),hxmin=round(np.nanmin(hx[a:b]),2),hxmax=round(np.nanmax(hx[a:b]),2),
        iae=round(np.trapezoid(np.abs(pp-s1),tt)/60,1)))
for o in out: print(json.dumps(o))
json.dump(out,open(sys.argv[2],'w'),indent=1)
