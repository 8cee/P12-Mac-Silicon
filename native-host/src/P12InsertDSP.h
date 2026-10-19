#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
#include "P12AutoWah.h"

namespace p12 {
// Shared pad/track/master insert processor. Configuration is control-thread only;
// process performs no allocation, locks, IPC or string comparisons.
class InsertDSP {
    enum Mode { Off, Filter, Formant, Ladder, Compress, Limiter, Gate, Deess,
        Tube, Tape, Fold, Excite, Clip, Rectify, Delay, Pingpong, Multitap,
        Reverb, Chorus, Flanger, Dimension, Wow, Phaser, Tremolo, Fuzz, Wah,
        Auto, Crush, Sampler, Vinyl };
    Mode mode=Off;
    std::string configuredType;
    struct Biquad {
        double b0=1,b1=0,b2=0,a1=0,a2=0,z1[2]{},z2[2]{};
        void set(int kind,double rate,double freq,double q,double gain=0) {
            const double w=6.283185307179586*std::clamp(freq,10.0,rate*.45)/rate;
            const double c=std::cos(w),s=std::sin(w),alpha=s/(2*std::max(.1,q)),A=std::pow(10.0,gain/40.0);
            double n0=1,n1=0,n2=0,d0=1,d1=-2*c,d2=1-alpha;
            switch(kind){
            case 0:n0=(1-c)/2;n1=1-c;n2=n0;d0=1+alpha;break;
            case 1:n0=(1+c)/2;n1=-(1+c);n2=n0;d0=1+alpha;break;
            case 2:n0=alpha;n1=0;n2=-alpha;d0=1+alpha;break;
            case 3:n0=1;n1=-2*c;n2=1;d0=1+alpha;break;
            case 4:n0=1+alpha*A;n1=-2*c;n2=1-alpha*A;d0=1+alpha/A;d2=1-alpha/A;break;
            case 5:case 6:{
                const double t=2*std::sqrt(A)*s/std::sqrt(2.0);
                if(kind==5){n0=A*((A+1)-(A-1)*c+t);n1=2*A*((A-1)-(A+1)*c);n2=A*((A+1)-(A-1)*c-t);d0=(A+1)+(A-1)*c+t;d1=-2*((A-1)+(A+1)*c);d2=(A+1)+(A-1)*c-t;}
                else {n0=A*((A+1)+(A-1)*c+t);n1=-2*A*((A-1)+(A+1)*c);n2=A*((A+1)+(A-1)*c-t);d0=(A+1)-(A-1)*c+t;d1=2*((A-1)-(A+1)*c);d2=(A+1)-(A-1)*c-t;}
                break;}
            }
            b0=n0/d0;b1=n1/d0;b2=n2/d0;a1=d1/d0;a2=d2/d0;
        }
        float run(int ch,float x) noexcept {const double y=b0*x+z1[ch];z1[ch]=b1*x-a1*y+z2[ch];z2[ch]=b2*x-a2*y;return static_cast<float>(y);}
    };
    std::array<Biquad,4> filters{};
    int filterCount=0;
    std::array<std::vector<float>,2> ring;
    std::array<std::array<std::vector<float>,4>,2> comb;
    std::array<std::array<size_t,4>,2> combCursor{};
    std::array<std::array<float,4>,2> combDamp{};
    size_t cursor=0;
    double sampleRate=48000,phase=0,phaseStep=0;
    float wet=1,drive=1,amount=.4f,depth=.5f,feedback=.3f,delayFrames=1,
          attack=.99f,release=.99f,threshold=-24,ratio=4,makeup=1,damping=.2f,
          env[2]{},hold[2]{},holdPhase=0,holdStep=1,quant=2048,noise=.02f,crackle=.03f,
          phaserState[2][4]{};
    uint32_t random=0x12cafe;
    AutoWah autoWah;
    float randomSample() noexcept {random^=random<<13;random^=random>>17;random^=random<<5;return static_cast<float>(random/4294967295.0*2-1);}
    float read(int ch,double frames) const noexcept {
        const auto size=ring[ch].size();
        const double pos=std::fmod(static_cast<double>(cursor+size)-std::clamp(frames,1.0,static_cast<double>(size-2)),static_cast<double>(size));
        const auto a=static_cast<size_t>(pos),b=(a+1)%size;
        return ring[ch][a]+static_cast<float>(pos-a)*(ring[ch][b]-ring[ch][a]);
    }
public:
    bool enabled=false;
    template<class Getter> bool configure(std::string_view type,bool on,double rate,Getter val) {
        const bool preserve=type==configuredType&&sampleRate==std::clamp(rate,8000.0,192000.0);
        configuredType=type;enabled=on;mode=Off;sampleRate=std::clamp(rate,8000.0,192000.0);
        filterCount=0;
        if(!preserve){filters={};cursor=0;phase=0;holdPhase=0;}
        if(!preserve)for(int ch=0;ch<2;++ch){env[ch]=hold[ch]=0;for(int i=0;i<4;++i){phaserState[ch][i]=0;combCursor[ch][i]=0;combDamp[ch][i]=0;comb[ch][i].clear();}ring[ch].clear();}
        wet=std::clamp(val("mix",100.f)/100.f,0.f,1.f);
        amount=val("amount",40)/100;depth=val("depth",50)/100;drive=1+val("drive",30)/100*15;
        feedback=std::clamp(val("feedback",30)/100,0.f,.9f);
        threshold=val("threshold",-24);ratio=std::max(1.f,val("ratio",4));makeup=std::pow(10.f,val("makeup",0)/20);
        attack=std::exp(-1.f/static_cast<float>(sampleRate*std::max(.0001f,val("attack",10)/1000)));
        release=std::exp(-1.f/static_cast<float>(sampleRate*std::max(.001f,val("release",150)/1000)));
        const auto freq=[&](const char* key,double low,double high){return low*std::pow(high/low,val(key,50)/100);};
        const auto add=[&](int kind,double hz,double q,double db=0){filters[filterCount++].set(kind,sampleRate,hz,q,db);};
        if(type=="off")return true;
        if(type=="filter_lp"||type=="filter_hp"){mode=Filter;add(type=="filter_lp"?0:1,type=="filter_lp"?freq("cutoff",80,10000):freq("cutoff",40,4000),.3+val("reso",20)*.14);}
        else if(type=="filter_bp"||type=="filter_notch"||type=="filter_statevar"){mode=Filter;add(type=="filter_notch"?3:2,freq("freq",100,8000),.5+val("q",50)*.2);}
        else if(type=="filter_ladder"){mode=Ladder;add(0,freq("cutoff",30,18000),.7+val("reso",35)/18);add(0,freq("cutoff",30,18000),.7);}
        else if(type=="filter_formant"){mode=Formant;add(2,500+val("vowel",50)*8,4+val("reso",60)/12);add(2,1100+(100-val("vowel",50))*14,5);}
        else if(type=="eq_tilt"){mode=Filter;const double pivot=freq("freq",200,5000),db=val("tilt",0)*.12;add(5,pivot,.7,-db);add(6,pivot,.7,db);}
        else if(type=="eq_low"||type=="eq_high"||type=="eq_para"){mode=Filter;add(type=="eq_low"?5:type=="eq_high"?6:4,type=="eq_low"?freq("freq",40,1000):type=="eq_high"?freq("freq",1000,12000):freq("freq",100,8000),.3+val("q",50)*.1,val("gain",0));}
        else if(type=="eq_3band"){mode=Filter;add(5,180,.7,val("low",0));add(4,1000,1,val("mid",0));add(6,6000,.7,val("high",0));}
        else if(type=="eq_graphic"){mode=Filter;add(4,120,1.1,val("low",0));add(4,500,1.1,val("lowmid",0));add(4,2000,1.1,val("highmid",0));add(4,8000,1.1,val("high",0));}
        else if(type=="comp_basic"||type=="comp_vca"||type=="comp_optical"){mode=Compress;if(type=="comp_optical")attack=std::exp(-1.f/static_cast<float>(sampleRate*.03));}
        else if(type=="limiter_peak"){mode=Limiter;ratio=1000;attack=0;}
        else if(type=="gate")mode=Gate;
        else if(type=="deesser"){mode=Deess;add(1,freq("freq",2500,12000),.7);threshold=-12-val("amount",45)*.35;ratio=8;attack=std::exp(-1.f/static_cast<float>(sampleRate*.002));}
        else if(type=="harmonic_tube"||type=="harmonic_tape"||type=="mod_fuzz"){mode=type=="harmonic_tube"?Tube:type=="mod_fuzz"?Fuzz:Tape;add(0,freq(type=="harmonic_tape"?"warmth":"tone",800,10000),.7);}
        else if(type=="harmonic_fold"){mode=Fold;drive=1+val("fold",30)*.12;}
        else if(type=="harmonic_exciter"){mode=Excite;add(1,freq("freq",1000,10000),.7);}
        else if(type=="harmonic_softclip")mode=Clip;
        else if(type=="harmonic_rectifier")mode=Rectify;
        else if(type.substr(0,6)=="delay_"){
            mode=type=="delay_pingpong"?Pingpong:type=="delay_multi"?Multitap:Delay;
            const float t=val("time",40)/100;
            delayFrames=static_cast<float>(sampleRate*(type=="delay_slap"?.02+t*.15:type=="delay_dub"?.04+t*1.1:mode==Multitap?.04+t*.36:.05+t*.45));
            feedback*=type=="delay_slap"?.3f:type=="delay_dub"?.88f:type=="delay_tape"?.7f:mode==Multitap?.5f:.6f;
            if(type=="delay_tape"||type=="delay_dub")add(0,freq("tone",800,9000),.7);
        }
        else if(type.substr(0,7)=="reverb_"){
            mode=Reverb;const double size=.5+val("size",50)*.015;
            feedback=.6f+val("decay",50)*.0035f;
            damping=type=="reverb_hall"?.4f:type=="reverb_plate"?.6f:.3f;
            constexpr double times[]={.0297,.0371,.0411,.0437};
            for(int ch=0;ch<2;++ch)for(int i=0;i<4;++i){const auto length=static_cast<size_t>(sampleRate*times[i]*size*(ch?1.013:1));if(comb[ch][i].size()!=length){comb[ch][i].assign(length,0);combCursor[ch][i]=0;combDamp[ch][i]=0;}}
        }
        else if(type=="mod_chorus"||type=="mod_flanger"||type=="mod_dimension"||type=="vintage_wow"){
            mode=type=="mod_chorus"?Chorus:type=="mod_flanger"?Flanger:type=="mod_dimension"?Dimension:Wow;
            delayFrames=static_cast<float>(sampleRate*(mode==Flanger?.003:mode==Wow?.02:.014));
            phaseStep=6.283185307179586*(.08+val("rate",30)/100*(mode==Dimension?.5:mode==Wow?2:3))/sampleRate;
        }
        else if(type=="mod_phaser"||type=="mod_tremolo"){mode=type=="mod_phaser"?Phaser:Tremolo;phaseStep=6.283185307179586*(.2+val("rate",40)*.05)/sampleRate;}
        else if(type=="mod_wah"){mode=Wah;add(2,400+val("position",50)*30,1+val("reso",60)*.14);}
        else if(type=="mod_autowah"){mode=Auto;autoWah.configure(sampleRate,val("rate",35),val("depth",60),val("reso",60),!preserve);}
        else if(type=="vintage_12bit"||type=="vintage_8bit"||type=="vintage_sampler"){
            mode=type=="vintage_sampler"?Sampler:Crush;
            quant=std::pow(2.f,type=="vintage_8bit"?7.f:mode==Sampler?9.f:11.f);
            holdStep=1.f/(1+amount*(type=="vintage_8bit"?16.f:8.f));
            add(0,mode==Sampler?7000-val("amount",45)*35:std::min(12000.0,sampleRate*holdStep*.45),.7);
        }
        else if(type=="vintage_vinyl"){mode=Vinyl;noise=val("noise",20)/100*.08f;crackle=val("crackle",20)/100*.15f;}
        else {enabled=false;return false;}
        if(mode==Delay||mode==Pingpong||mode==Multitap||mode==Chorus||mode==Flanger||mode==Dimension||mode==Wow)
            for(auto& r:ring){const auto length=static_cast<size_t>(sampleRate*(mode==Delay||mode==Pingpong||mode==Multitap?1.5:.1))+4;if(r.size()!=length)r.assign(length,0);}
        return true;
    }
    bool active() const noexcept{return enabled&&mode!=Off;}
    void process(float* left,float* right,int n) noexcept {
        if(!active()||wet==0)return;
        if(mode==Auto){autoWah.process(left,right,n,wet);return;}
        for(int i=0;i<n;++i){
            const float input[2]={left[i],right[i]};float output[2]{};
            const float lfo=static_cast<float>(std::sin(phase));
            const bool holdNow=holdPhase>=1;
            if(holdNow)holdPhase-=1;
            holdPhase+=holdStep;
            for(int ch=0;ch<2;++ch){
                const float dry=input[ch];float y=dry;
                switch(mode){
                case Filter:case Wah:for(int f=0;f<filterCount;++f)y=filters[f].run(ch,y);break;
                case Formant:y=filters[0].run(ch,dry)+filters[1].run(ch,dry);break;
                case Ladder:y=std::tanh(filters[1].run(ch,filters[0].run(ch,dry))*drive);break;
                case Compress:case Limiter:case Gate:case Deess:{
                    const float high=mode==Deess?filters[0].run(ch,dry):dry,level=std::abs(high);
                    const float c=level>env[ch]?attack:release;env[ch]=c*env[ch]+(1-c)*level;
                    const float db=20*std::log10(std::max(1.e-8f,env[ch]));
                    float dbGain=mode==Gate?std::min(0.f,(db-threshold)*3):std::min(0.f,(threshold-db)*(1-1/ratio));
                    const float gain=std::pow(10.f,dbGain/20)*makeup;
                    y=mode==Deess?dry+high*(gain-1):dry*gain;
                    break;}
                case Tube:case Tape:case Fuzz:y=filters[0].run(ch,std::tanh(dry*drive));break;
                case Fold:y=static_cast<float>(2/3.141592653589793*std::asin(std::sin(dry*drive*1.5707963267948966)));break;
                case Excite:{const float high=filters[0].run(ch,dry);y=dry+amount*(std::tanh(high*4)-high);break;}
                case Clip:y=std::tanh(dry*drive);break;
                case Rectify:y=(1-amount)*dry+amount*std::abs(dry);break;
                case Delay:case Pingpong:case Multitap:{
                    const float delayed=read(mode==Pingpong?1-ch:ch,delayFrames);
                    y=mode==Multitap?.8f*delayed+.55f*read(ch,delayFrames*1.5)+.35f*read(ch,delayFrames*2):delayed;
                    float fb=y;if(filterCount)fb=filters[0].run(ch,fb);
                    output[ch]=std::clamp(dry+feedback*fb,-4.f,4.f);break;}
                case Reverb:y=0;for(int f=0;f<4;++f){auto& r=comb[ch][f];auto& pos=combCursor[ch][f];const float z=r[pos];combDamp[ch][f]+=damping*(z-combDamp[ch][f]);r[pos]=std::clamp(dry+feedback*combDamp[ch][f],-4.f,4.f);pos=(pos+1)%r.size();y+=z*.25f;}break;
                case Chorus:case Flanger:case Dimension:case Wow:{
                    const double sweep=std::sin(phase+(ch&&mode!=Wow?1.5707963267948966:0));
                    const double frames=delayFrames*(1+depth*(mode==Wow?.05:.6)*sweep);
                    y=read(ch,frames);output[ch]=std::clamp(dry+(mode==Flanger?feedback*y:0),-4.f,4.f);break;}
                case Phaser:{const float a=.1f+.8f*(.5f+.5f*lfo)*depth;for(int f=0;f<4;++f){const float z=-a*y+phaserState[ch][f];phaserState[ch][f]=y+a*z;y=z;}break;}
                case Tremolo:y=dry*(1-depth*(.5f+.5f*lfo));break;
                case Crush:case Sampler:{const float filtered=filters[0].run(ch,dry);if(holdNow)hold[ch]=std::round(filtered*quant)/quant;y=hold[ch];break;}
                case Vinyl:y=dry+noise*randomSample()+(randomSample()>.9995f?crackle*randomSample():0);break;
                default:break;
                }
                (ch?right:left)[i]=dry+(y-dry)*wet;
            }
            if(!ring[0].empty()){ring[0][cursor]=output[0];ring[1][cursor]=output[1];cursor=(cursor+1)%ring[0].size();}
            phase+=phaseStep;if(phase>6.283185307179586)phase-=6.283185307179586;
        }
    }
};
}
