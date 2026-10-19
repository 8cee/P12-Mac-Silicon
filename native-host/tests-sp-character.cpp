#include "src/P12NativeSampler.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static std::unique_ptr<p12::Sample> sineSample(int frames,double hz,int rate=48000){
    auto s=std::make_unique<p12::Sample>();
    s->rate=rate;s->left.resize(frames);s->right.resize(frames);
    for(int i=0;i<frames;++i){
        const float v=0.72f*std::sin(float(2.0*3.14159265358979323846*hz*i/rate));
        s->left[i]=v;s->right[i]=v;
    }
    return s;
}
static double rmsDiff(const float* a,const float* b,int n){
    double e=0;for(int i=0;i<n;++i){const double d=a[i]-b[i];e+=d*d;}return std::sqrt(e/n);
}
static bool finiteBlock(const float* a,int n){for(int i=0;i<n;++i)if(!std::isfinite(a[i]))return false;return true;}

int main(){
    constexpr int n=1024;
    p12::Sampler classic,hifi;
    assert(classic.load(0,sineSample(8192,17000.0)));
    assert(hifi.load(0,sineSample(8192,17000.0)));
    p12::Event e;e.pad=0;e.track=0;e.velocity=1.0f;e.ratio=1.0f;e.trimStart=0;e.trimEnd=1;
    e.spCharacter=true;assert(classic.submit(e));classic.beginBlock();
    e.spCharacter=false;assert(hifi.submit(e));hifi.beginBlock();
    float cl[n]{},cr[n]{},hl[n]{},hr[n]{};
    assert(classic.renderBus(0,0,cl,cr,n,48000));
    assert(hifi.renderBus(0,0,hl,hr,n,48000));
    assert(finiteBlock(cl,n)&&finiteBlock(cr,n)&&finiteBlock(hl,n)&&finiteBlock(hr,n));
    // Character path must be audibly different from transparent cubic SRC.
    assert(rmsDiff(cl,hl,n)>0.02);
    // Stereo remains coherent for a mono source; playback DSP does not fold/route channels.
    assert(rmsDiff(cl,cr,n)<1.0e-6);

    // Pitch changes alter the virtual DAC cadence without destabilising output.
    p12::Sampler pitched;
    assert(pitched.load(0,sineSample(8192,6000.0)));
    e.spCharacter=true;e.ratio=0.5f;assert(pitched.submit(e));pitched.beginBlock();
    float pl[n]{},pr[n]{};assert(pitched.renderBus(0,0,pl,pr,n,48000));
    assert(finiteBlock(pl,n));

    // Clean Hi-Fi mode retains the legacy cubic engine and stays free of NaN/Inf.
    p12::Sampler clean;
    assert(clean.load(0,sineSample(4096,1000.0)));
    e.spCharacter=false;e.ratio=1.0f;assert(clean.submit(e));clean.beginBlock();
    float xl[256]{},xr[256]{};assert(clean.renderBus(0,0,xl,xr,256,48000));
    assert(finiteBlock(xl,256));
    std::puts("PASS: playback-only SP character / Hi-Fi isolation / finite DSP");
}
