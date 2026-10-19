#include "src/P12NativeSequence.h"
#include <cassert>
#include <iostream>
#include <tuple>
int main(){
 p12::SequencePlan p;p.id=1;p.duration=.173;p.bpm=96;
 for(double t:{0.,.02,.079,.14}){p12::SequenceEvent e;e.seconds=t;p.events.push_back(e);}
 p12::SequenceClock c;c.published.store(&p);c.start();
 std::vector<long long> onset;long long frame=0;
 // Five seconds of callbacks, with NO renderer/IPC calls after Play. The
 // renderer may be stalled for 742ms or absent for the whole run.
 for(int b=0;b<500;b++){c.process(480,48000,10000+b*10,[&](const auto&,int at){onset.push_back(frame+at);});frame+=480;}
 assert(onset.size()>100);
 for(size_t i=0;i<onset.size();i++){
   double expected=(i/4)*p.duration+p.events[i%4].seconds;
   assert(std::llabs(onset[i]-std::llround(expected*48000))<=1);
 }
 auto count=onset.size();c.stop();c.process(480,48000,0,[&](const auto&,int){onset.push_back(0);});assert(onset.size()==count);
 // New plan is adopted at the original loop boundary, never mid-pattern.
 p12::SequencePlan q;q.id=2;q.duration=.4;q.bpm=120;q.events.push_back({0.});
 c.published.store(&p);c.start();c.process(480,48000,0,[](const auto&,int){});c.published.store(&q);
 assert(c.planId.load()==1);
 for(int i=0;i<20;i++)c.process(480,48000,0,[](const auto&,int){});
 assert(c.planId.load()==2);
 // An immediate edit adopts the new plan without resetting the cursor.
 p12::SequencePlan edit=q;edit.id=3;edit.immediate=true;
 const auto before=c.position.load();c.published.store(&edit);
 c.process(480,48000,0,[](const auto&,int){});
 assert(c.planId.load()==3);assert(std::abs(c.position.load()-before-.01)<1e-8);
 // Start deadline: no premature note, exact callback sample offset.
 c.stop();c.published.store(&p);c.start(1015);int first=-1;
 c.process(480,48000,1000,[&](const auto&,int at){first=at;});assert(first==-1);
 c.process(480,48000,1010,[&](const auto&,int at){if(first<0)first=at;});assert(first==240);
 std::cout<<"PASS native clock: 5s without renderer, <=1 sample error, stop, boundary swap, start deadline\n";
}
