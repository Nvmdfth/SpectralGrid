// Replays recorded link-window SUMMARIES (not raw CSI: for raw captures use hub/host_test/replay_presence.cpp) ("t link tstd rssi" on stdin, e.g. captures/*.csv with commas turned into spaces) through the REAL shared/presence.h.
// build: g++ -O1 -std=c++17 -o /tmp/replay hub/tools/replay_windows.cpp   run: tail -n +2 CSV | tr "," " " | /tmp/replay CAL_T0 CAL_T1 0.22 > states.txt
// args: calibration window start/end (s), g (0.16 high / 0.22 normal / 0.32 low); stderr = calibration per link, stdout = "t link state" (1 clear 2 motion 3 on-line)
#include "../../shared/presence.h"
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
struct S{double t;std::string l;float ts,rs;};
int main(int argc,char**argv){
  double c0=atof(argv[1]),c1=atof(argv[2]);float g=atof(argv[3]);
  std::vector<S> v;char l[32];double t;float ts;int rs;
  while(scanf("%lf %31s %f %d",&t,l,&ts,&rs)==4)v.push_back({t,l,ts,(float)rs});
  std::map<std::string,PresCalAcc> acc;std::map<std::string,PresLink> pl;std::map<std::string,uint32_t> seq;
  for(auto&s:v)if(s.t>=c0&&s.t<c1){auto&a=acc[s.l];if(!a.n&&!a.seq_valid)pres_cal_reset(&a);pres_cal_add(&a,s.ts,s.rs,++seq[s.l]);}
  for(auto&k:acc){auto r=pres_cal_result(&k.second,g);fprintf(stderr,"%s q0=%.4f thr=%.4f rssi=%.1f valid=%d warn=%d n=%u\n",k.first.c_str(),r.q0,r.thr,r.rssi_base,r.valid,r.warn,r.n);
    PresLink p;pres_link_set_cal(&p,r.q0,r.thr,r.rssi_base);pl[k.first]=p;}
  for(auto&s:v){if(s.t<c1||!pl.count(s.l))continue;auto&p=pl[s.l];uint32_t ms=(uint32_t)(s.t*1000)+1;pres_link_update(&p,s.ts,s.rs,++seq[s.l]+1000,ms);printf("%.1f %s %d\n",s.t,s.l.c_str(),(int)p.state);}
}
