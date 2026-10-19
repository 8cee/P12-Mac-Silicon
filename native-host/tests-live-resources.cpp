#include "src/P12NativeSequence.h"
#include "src/P12PluginResources.h"
#include "src/P12PluginStartup.h"
#include <cassert>
#include <fstream>
#include <iostream>
int main(){
 p12::Sampler s;auto sample=std::make_unique<p12::Sample>();sample->rate=48000;sample->left.assign(4096,.7f);sample->right=sample->left;assert(s.load(0,std::move(sample)));
 p12::Event hit;hit.pad=0;hit.track=0;hit.ratio=1;hit.submittedEpochMs=1000;
 assert(s.submit(hit));s.beginBlock(1010,48000);assert(s.liveEvents()==1);assert(s.liveWorstQueueMs()==10);assert(s.lateEvents()==0);
 float l[480]{},r[480]{};s.renderBus(0,0,l,r,480,48000);assert(l[100]>.1f);
 // Genuine missed scheduled deadline still counts and remains audible.
 hit.targetEpochMs=1005;assert(s.submit(hit));s.beginBlock(1020,48000);assert(s.lateEvents()==1);assert(s.liveEvents()==1);
 // Transport STOP flushes queued future hits; later live hits remain playable.
 hit.targetEpochMs=2000;assert(s.submit(hit));s.requestStop();s.beginBlock(1030,48000);assert(s.activeVoices()==0);
 hit.targetEpochMs=0;hit.submittedEpochMs=1040;assert(s.submit(hit));s.beginBlock(1050,48000);assert(s.activeVoices()==1);assert(s.lateEvents()==1);
 s.resetDiagnostics();assert(s.liveEvents()==0&&s.liveWorstQueueMs()==0&&s.lateEvents()==0);
 // STOP during dense callback cancels all remaining notes in the same block.
 p12::SequencePlan plan;plan.duration=.5;for(int i=0;i<50;++i){p12::SequenceEvent e;e.seconds=i*.0001;plan.events.push_back(e);}
 p12::SequenceClock clock;clock.published.store(&plan);clock.start();int fired=0;
 clock.process(480,48000,0,[&](const auto&,int){++fired;clock.stop();});assert(fired==1);
 clock.process(480,48000,0,[&](const auto&,int){++fired;});assert(fired==1);
 // Private processor warm-up gets silence on every call, including after tail output.
 float input=0;int clears=0,blocks=0;p12::warmPlugin(4,[&]{input=0;++clears;},[&]{assert(input==0);input=.9f;++blocks;});assert(clears==4&&blocks==4);
 // Single-file VST3 + differently suffixed vendor folder, with wrong-version decoy.
 namespace fs=std::filesystem;
 auto root=fs::temp_directory_path()/"p12-resource-regression";fs::remove_all(root);
 auto plugin=root/"VST3/iZotope/Ozone 12.vst3";fs::create_directories(plugin.parent_path());std::ofstream(plugin)<<"fixture";
 auto vendor=root/"Program Files/iZotope";auto correct=vendor/"Ozone 12 Advanced/resources";auto wrong=vendor/"Ozone 11/resources";fs::create_directories(correct);fs::create_directories(wrong);
 std::ofstream(wrong/"icudtl.dat")<<"wrong version";std::ofstream(correct/"icudtl.dat")<<"correct version";std::ofstream(correct/"OzonePlatform.dll")<<"fixture";
 auto found=p12::discoverPluginResources(plugin,{vendor});assert(found.icu==correct/"icudtl.dat");assert(found.platformLibraries.size()==1);assert(found.platformLibraries[0]==correct/"OzonePlatform.dll");
 fs::remove_all(vendor/"Ozone 12 Advanced");found=p12::discoverPluginResources(plugin,{vendor});assert(found.icu.empty());assert(found.platformLibraries.empty());
 auto custom=root/"CustomInstall/data";fs::create_directories(custom);std::ofstream(custom/"icudtl.dat")<<"custom";found=p12::discoverPluginResources(plugin,{vendor},custom.parent_path());assert(found.icu==custom/"icudtl.dat");
 assert(!p12::sameResourceProduct("Ozone 120","Ozone 12"));assert(!p12::sameResourceProduct("Ozone 11","Ozone 12"));
 fs::remove_all(root);std::cout<<"PASS live latency, missed deadlines, stop flush/cancellation, private warmup, version-specific resource lookup\n";
}
