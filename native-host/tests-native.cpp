#include "src/P12NativeSampler.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
static std::unique_ptr<p12::Sample> mk(int len, float gain=1.0f){
  auto s=std::make_unique<p12::Sample>();s->rate=48000;
  s->left.assign(len,gain);s->right.assign(len,gain);return s;
}
static float magnitude(const float *s,int n){float m=0;for(int i=0;i<n;i++)m=std::max(m,std::fabs(s[i]));return m;}
int main(){
 p12::Sampler engine;
 assert(!engine.load(-1,mk(64)));
 assert(engine.load(0,mk(512)));
 assert(engine.load(1,mk(1024,0.25f)));
 p12::Event a; a.pad=0; a.track=1; a.velocity=1; a.ratio=1; a.trimStart=0;a.trimEnd=1;
 assert(engine.submit(a));engine.beginBlock();
 assert(engine.hasActiveRoute(0,1));
 assert(!engine.hasActiveRoute(0,0));
 assert(!engine.hasActiveRoute(-1,1));
 float l[128]{},r[128]{};
 assert(!engine.renderBus(0,0,l,r,128,48000));
 assert(magnitude(l,128)==0);
 assert(engine.renderBus(0,1,l,r,128,48000));
 assert(magnitude(l,128)>0.6f && magnitude(r,128)>0.6f);
 // Sample rate conversion: 2x means source advances twice as quickly.
 a.ratio=2;assert(engine.submit(a));engine.beginBlock();
 float l2[128]{},r2[128]{};
 engine.renderBus(0,1,l2,r2,128,48000);
 assert(engine.activeVoices()==1);
 engine.renderBus(0,1,l2,r2,128,48000);
 engine.renderBus(0,1,l2,r2,128,48000);
 assert(engine.activeVoices()==0);
 // Native pad stop/choke avoids overlapping tails.
 a.pad=1;a.track=2;a.ratio=1;a.velocity=1;assert(engine.submit(a));
 engine.beginBlock();assert(engine.activeVoices()==1);
 assert(engine.hasActiveRoute(1,2));
 p12::Event off;off.type=p12::Event::StopPad;off.pad=1;
 assert(engine.submit(off));engine.beginBlock();assert(engine.activeVoices()==0);
 // Offset: sample begins only after delay in the device callback.
 a.pad=0;a.track=0;a.delayFrames=50;a.ratio=1;
 assert(engine.submit(a));engine.beginBlock();
 float l3[128]{},r3[128]{};
 assert(engine.renderBus(0,0,l3,r3,128,48000));
 for(int i=0;i<50;i++) assert(l3[i]==0);
 assert(magnitude(l3+60,68)>0.2f);
 p12::Event all;all.type=p12::Event::StopAll;
 assert(engine.submit(all));engine.beginBlock();assert(engine.activeVoices()==0);
 assert(!engine.hasActiveRoute(0,0));
 // New immutable sample swaps safely; old voices keep their old source.
 assert(engine.load(0,mk(100,0.1f)));
 a.delayFrames=0;assert(engine.submit(a));engine.beginBlock();
 float l4[128]{},r4[128]{};engine.renderBus(0,0,l4,r4,128,48000);
 assert(magnitude(l4,128)<0.11f);
 // Project changes must NOT expose old pad samples through native routing.
 assert(engine.submit(a)); // pending old-project note must be discarded
 engine.clearPads();
 assert(!engine.hasLoadedPad(0) && !engine.hasLoadedPad(1));
 assert(!engine.load(-1,mk(32)));
 assert(engine.load(0,mk(64)) && engine.hasLoadedPad(0));
 engine.beginBlock();
 assert(!engine.hasActiveRoute(0,0));
 std::puts("PASS: native stereo sampler, routing, resampling, choke, offsets, sample swaps");
}
