# Threshold sweep on recorded data (captures/grid_*.csv + grid2_sub.csv, tstd only): false alarms in a long empty recording vs still-person detection per threshold ratio.
# run from the repo root: python3 hub/tools/tune_threshold.py. Paths are the 2026-10-15 captures; edit them for new data.
import csv,collections,statistics as st
L=['3>4','4>3','h-3','h-4']
def load(p):
    d=collections.defaultdict(list)
    for x in csv.DictReader(open(p)):
        if x['link'] in L: d[x['link']].append((float(x['t_s']),float(x['tstd'])))
    return d
g1=load('captures/grid_20261015.csv')
q0={l:st.median(v for t,v in g1[l] if 320<=t<4500) for l in L}
print('q0',{l:round(q0[l],4) for l in L})
def votes(series,R,q):
    out=[];h=0
    for t,v in series:
        h=((h<<1)|(v>R*q))&7; out.append((t,bin(h).count('1')>=2))
    return out
def per_sec(v):
    s={}
    for t,b in v: s[int(t)]=s.get(int(t),False) or b
    return s
def fused(d,R,links,a,b):
    secs=collections.defaultdict(int)
    allsecs=set()
    for l in links:
        for t,on in votes([x for x in d[l]],R,q0[l]):
            if a<=t<b:
                allsecs.add(int(t))
                if on: secs[int(t)]+=1
    return allsecs,secs
labs=[(float(l.split(',')[0]),l.split(',')[2]) for l in open('captures/grid2_sub.csv') if ',L,' in l]
g2=load('captures/grid2_sub.csv')
end=5100
segs=[]
for i,(t,lb) in enumerate(labs):
    e=labs[i+1][0] if i+1<len(labs) else end
    if lb.endswith('still') or lb.endswith('walk'): 
        if e-1-(t+8)>=6: segs.append((lb,t+8,e-1))
print('R     FA%(any)  FA%(>=2) | still any% >=2% | walk(seg>=6s) any% ')
for R in [1.10,1.15,1.2,1.3,1.5,1.8]:
    a1,s1=fused(g1,R,L,330,4500)
    fa1=100*sum(1 for s in a1 if s1[s]>=1)/len(a1); fa2=100*sum(1 for s in a1 if s1[s]>=2)/len(a1)
    res=collections.defaultdict(lambda:[0,0,0])
    for lb,a,b in segs:
        al,sc=fused(g2,R,L,a,b); k=lb.split('_')[1]
        res[k][0]+=len(al); res[k][1]+=sum(1 for s in al if sc[s]>=1); res[k][2]+=sum(1 for s in al if sc[s]>=2)
    r=lambda k,i:100*res[k][i]/max(1,res[k][0])
    print(f'{R:4.2f} {fa1:8.2f} {fa2:8.2f} |  {r("still",1):6.0f} {r("still",2):6.0f} | {r("walk",1):6.0f}')
