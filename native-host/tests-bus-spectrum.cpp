#include "src/P12BusCompressor.h"
#include "src/P12SpectrumTap.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
static bool audio=false;
void* operator new(std::size_t n){assert(!audio);if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}void operator delete(void* p,std::size_t) noexcept {std::free(p);}
static double run(p12::BusCompressor& bus,float level,int blocks=400){float l[256],r[256];double energy=0;for(int block=0;block<blocks;block++){for(int i=0;i<256;i++){l[i]=level;r[i]=level*.5f;}audio=true;bus.process(l,r,256);audio=false;for(int i=0;i<256;i++){assert(std::isfinite(l[i]));assert(std::abs(r[i]-.5f*l[i])<1.e-5);if(block>=blocks-20)energy+=l[i]*l[i];}}return std::sqrt(energy/(20*256));}
int main(){
 p12::BusCompressor bus;bus.prepare(48000);assert(std::abs(run(bus,.8f)-.8)<1.e-5);
 bus.configure(true,-20,4,.1f,300,0,100);const auto compressed=run(bus,.8f);assert(compressed<.2&&compressed>.1);
 bus.configure(true,-20,4,.1f,300,6,100);const auto makeup=run(bus,.8f);assert(makeup>compressed*1.98&&makeup<compressed*2.02);
 bus.configure(true,-20,4,.1f,300,0,50);const auto parallel=run(bus,.8f);assert(parallel>.4&&parallel<.55);
 bus.configure(true,-20,4,.1f,300,0,0);assert(std::abs(run(bus,.8f)-.8)<1.e-4);
 bus.configure(false,-20,4,.1f,300,20,100);assert(std::abs(run(bus,.8f)-.8)<1.e-5);
 auto attack=[](float ms){p12::BusCompressor b;b.prepare(48000);b.configure(true,-20,4,ms,300,0,100);return run(b,.8f,20);};assert(attack(.1f)<attack(30));
 auto release=[](float ms){p12::BusCompressor b;b.prepare(48000);b.configure(true,-20,4,.1f,ms,0,100);run(b,.8f);return run(b,.08f,40);};assert(release(30)>release(1200));
 p12::SpectrumTap tap;float l[1024],r[1024];auto tone=[&](double hz){for(int i=0;i<1024;i++)l[i]=r[i]=float(.5*std::sin(6.283185307179586*hz*i/48000));audio=true;tap.capture(l,r,1024);audio=false;const auto bins=tap.read(48000);return int(std::max_element(bins.begin(),bins.end())-bins.begin());};assert(tone(220)<tone(4000));
 audio=true;tap.capture(nullptr,nullptr,1024);audio=false;for(int value:tap.read(48000))assert(value==0);
 std::puts("PASS native SSL: compression, stereo link, makeup, wet mix, bypass, attack/release, zero audio allocations; real FFT distinguishes 220Hz / 4kHz and clears silence");
}
