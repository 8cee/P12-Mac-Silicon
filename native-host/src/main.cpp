#ifndef NOMINMAX
 #define NOMINMAX 1
#endif
#include <JuceHeader.h>
#include "P12NativeSampler.h"
#include "P12NativePadControls.h"
#include "P12MixerGain.h"
#include "P12BusCompressor.h"
#include "P12SpectrumTap.h"
#include "P12PluginStartup.h"
#include "P12PluginResources.h"
#include "P12AutoWah.h"
#include "P12InsertDSP.h"
#include "P12NativeSequence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if JUCE_WINDOWS
 #include <ole2.h>
#endif

namespace
{
constexpr int kDefaultChannel = 1;
constexpr int kCommandTimeoutMs = 45000;

juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> values)
{
    auto* obj = new juce::DynamicObject();
    for (const auto& [key, value] : values)
        obj->setProperty (key, value);
    return juce::var (obj);
}

juce::var property (const juce::var& value, const char* name)
{
    if (auto* obj = value.getDynamicObject())
        return obj->getProperty (name);
    return {};
}

juce::String stringProperty (const juce::var& value, const char* name, const juce::String& fallback = {})
{
    const auto v = property (value, name);
    return v.isVoid() || v.isUndefined() ? fallback : v.toString();
}

int intProperty (const juce::var& value, const char* name, int fallback = 0)
{
    const auto v = property (value, name);
    return v.isVoid() || v.isUndefined() ? fallback : static_cast<int> (v);
}

bool boolProperty (const juce::var& value, const char* name, bool fallback = false)
{
    const auto v = property (value, name);
    return v.isVoid() || v.isUndefined() ? fallback : static_cast<bool> (v);
}

juce::String mixerKeyFor (const juce::var& payload)
{
    const auto target = stringProperty (payload, "target");
    const auto id = stringProperty (payload, "id");
    const auto slot = juce::jmax (1, intProperty (payload, "slot", 1));
    return "mixer:" + target + ":" + id + ":" + juce::String (slot);
}

juce::String hostKeyFor (const juce::var& payload)
{
    return "host:" + juce::String (juce::jmax (1, intProperty (payload, "slot", 1)));
}

class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    PluginEditorWindow (juce::AudioPluginInstance& processor, const juce::String& title, bool forceGeneric)
        : juce::DocumentWindow (title, juce::Colours::black, juce::DocumentWindow::allButtons, true)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);

        if (! forceGeneric)
            if (auto* custom = processor.createEditorIfNeeded())
                setContentOwned (custom, true);

        // Some older/problematic plug-ins either have no custom editor or have a
        // manufacturer GUI that is not compatible with this host. The generic
        // JUCE parameter editor gives P-12 a reliable fallback path.
        if (getContentComponent() == nullptr)
            setContentOwned (new juce::GenericAudioProcessorEditor (processor), true);

        auto bounds = getContentComponent() != nullptr ? getContentComponent()->getBounds()
                                                       : juce::Rectangle<int> (0, 0, 640, 480);
        const auto w = juce::jlimit (360, 1600, juce::jmax (bounds.getWidth(), 640));
        const auto h = juce::jlimit (260, 1100, juce::jmax (bounds.getHeight(), 420));
        centreWithSize (w, h);
        // The host process is launched hidden by Electron so its console does not
        // flash on screen. Explicitly promote the plug-in editor to a real,
        // foreground top-level window so STARTF_USESHOWWINDOW/SW_HIDE from the
        // child process cannot make the first editor appear invisible.
        // Do not keep third-party editors always-on-top. Several commercial
        // plug-ins open separate activation/login/helper windows; forcing the
        // editor above every other window can hide those dialogs and make the
        // plug-in look frozen. Bring the editor forward once, then let Windows
        // manage normal z-order/modal dialogs.
        setAlwaysOnTop (false);
        if (auto* content = getContentComponent())
            content->setVisible (true);
        setVisible (true);
        toFront (true);
        grabKeyboardFocus();
    }

    void closeButtonPressed() override
    {
        setVisible (false);
    }
};

// Resource work stays on the command/message thread. Register vendor DLL
// directories for delayed loading, and use the vendor ICU directory during
// creation. Never borrow icudtl.dat from another Chromium/CEF version.
class PluginResourceScope {
public:
    explicit PluginResourceScope(const juce::String& pluginPath)
      : previous(juce::File::getCurrentWorkingDirectory())
    {
       #if JUCE_MAC
        // VST3 bundles resolve their own frameworks/resources. Never stage Windows
        // ICU/DLL files beside a signed executable or change the installed bundle.
        const auto resources=juce::File(pluginPath).getChildFile("Contents/Resources");
        if(resources.isDirectory())resources.setAsCurrentWorkingDirectory();
       #else
        const juce::File plugin(pluginPath);
        auto root=plugin.isDirectory()?plugin:plugin.getParentDirectory();
        const bool ozone=pluginPath.containsIgnoreCase("ozone 12");
        std::vector<p12::fs::path> vendorParents;
       #if JUCE_WINDOWS
        if(ozone){
            for(const auto* name:{"ProgramFiles","ProgramW6432","ProgramFiles(x86)","CommonProgramFiles","CommonProgramW6432","ProgramData"}){
                const auto base=juce::SystemStats::getEnvironmentVariable(name,{});
                if(base.isNotEmpty())vendorParents.push_back(p12::fs::path(base.toWideCharPointer())/"iZotope");
            }
            // Custom installer locations are published through Windows uninstall metadata.
            for(auto hive:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER}){
                HKEY uninstall=nullptr;
                if(RegOpenKeyExW(hive,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",0,KEY_READ|KEY_WOW64_64KEY,&uninstall)!=ERROR_SUCCESS)continue;
                for(DWORD index=0;;++index){
                    wchar_t keyName[512];DWORD length=512;
                    const auto status=RegEnumKeyExW(uninstall,index,keyName,&length,nullptr,nullptr,nullptr,nullptr);
                    if(status==ERROR_NO_MORE_ITEMS)break;
                    if(status!=ERROR_SUCCESS)continue;
                    HKEY entry=nullptr;
                    if(RegOpenKeyExW(uninstall,keyName,0,KEY_READ,&entry)!=ERROR_SUCCESS)continue;
                    auto value=[&](const wchar_t* name){wchar_t data[4096]{};DWORD bytes=sizeof(data),type=0;
                        if(RegQueryValueExW(entry,name,nullptr,&type,reinterpret_cast<BYTE*>(data),&bytes)!=ERROR_SUCCESS||type!=REG_SZ)return juce::String{};
                        data[4095]=0;return juce::String(data);};
                    const auto display=value(L"DisplayName"),location=value(L"InstallLocation");
                    if(display.containsIgnoreCase("Ozone 12")&&location.isNotEmpty()){
                        vendorParents.push_back(p12::fs::path(location.toWideCharPointer()).parent_path());
                        installedRoots.push_back(p12::fs::path(location.toWideCharPointer()));
                    }
                    RegCloseKey(entry);
                }
                RegCloseKey(uninstall);
            }
        }
       #endif
        const auto overrideRoot=ozone?juce::SystemStats::getEnvironmentVariable("P12_OZONE_RESOURCE_DIR",{}):juce::String{};
        auto locations=p12::discoverPluginResources(p12::fs::path(pluginPath.toWideCharPointer()),vendorParents,
            overrideRoot.isEmpty()?p12::fs::path{}:p12::fs::path(overrideRoot.toWideCharPointer()));
        for(const auto& installed:installedRoots){
            auto extra=p12::discoverPluginResources(installed,{},installed);
            locations.directories.insert(locations.directories.end(),extra.directories.begin(),extra.directories.end());
            locations.platformLibraries.insert(locations.platformLibraries.end(),extra.platformLibraries.begin(),extra.platformLibraries.end());
            if(locations.icu.empty())locations.icu=extra.icu;
        }
        juce::File icu;
        if(!locations.icu.empty())icu=juce::File(juce::String(locations.icu.wstring().c_str()));
        for(const auto& dir:locations.directories)registerDllDirectory(juce::File(juce::String(dir.wstring().c_str())));
       #if JUCE_WINDOWS
        if(ozone){
            // Some Boost loaders use an altered search path. Load the vendor
            // platform by absolute path so its own adjacent DLLs resolve too.
            static juce::StringArray loaded;
            for(const auto& dll:locations.platformLibraries){
                const juce::String name(dll.wstring().c_str());
                if(loaded.contains(name))continue;
                if(LoadLibraryExW(dll.c_str(),nullptr,0x100|0x1000))loaded.add(name);
                else{platformFailed=true;std::cerr<<"[P12] PLATFORM DEPENDENCY FAILED path="<<name<<" winError="<<GetLastError()<<std::endl;}
            }
        }
       #endif
        if(icu.existsAsFile()&&pluginPath.containsIgnoreCase("ozone")){
            const auto hostDir=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
            const auto adjacent=hostDir.getChildFile("icudtl.dat");
            // SkIcuLoader in the user's log explicitly probes beside the host
            // executable. Use ONLY the ICU shipped by this installed Ozone.
            static juce::String stagedIcuSource;
            if(icu!=adjacent && (!adjacent.existsAsFile()||stagedIcuSource!=icu.getFullPathName())){
                if(icu.copyFileTo(adjacent))stagedIcuSource=icu.getFullPathName();
                else{platformFailed=true;std::cerr<<"[P12] VENDOR ICU COPY FAILED "<<adjacent.getFullPathName()<<std::endl;}
            }
        }
        const auto cwd=icu.existsAsFile()?icu.getParentDirectory():root.getChildFile("Contents/x86_64-win").isDirectory()?root.getChildFile("Contents/x86_64-win"):root;
        cwd.setAsCurrentWorkingDirectory();registerDllDirectory(cwd);
       #if JUCE_WINDOWS
        customEditorResourcesMissing=ozone&&(!icu.existsAsFile()||platformFailed);
       #endif
        std::cerr<<"[P12] PLUGIN RESOURCES path="<<pluginPath<<" cwd="<<cwd.getFullPathName()<<" vendorICU="<<(icu.existsAsFile()?icu.getFullPathName():"missing")<<std::endl;
       #endif
    }
    ~PluginResourceScope(){previous.setAsCurrentWorkingDirectory();}
    bool useGenericEditor() const noexcept{return customEditorResourcesMissing;}
private:
    juce::File previous;
    bool customEditorResourcesMissing=false,platformFailed=false;
    std::vector<p12::fs::path> installedRoots;
    static void registerDllDirectory(const juce::File& dir){
       #if JUCE_WINDOWS
        // Keep registered directories for the process lifetime: plug-ins may
        // load platform dependencies on a worker thread after editor creation.
        using AddFn=void* (WINAPI*)(PCWSTR);
        const auto fn=reinterpret_cast<AddFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"AddDllDirectory"));
        using DefaultFn=BOOL (WINAPI*)(DWORD);
        static bool configured=false;static juce::StringArray registered;
        if(!configured){const auto setup=reinterpret_cast<DefaultFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"SetDefaultDllDirectories"));if(setup)setup(0x1000);configured=true;}
        const auto name=dir.getFullPathName();
        if(fn&&!registered.contains(name)){if(fn(name.toWideCharPointer()))registered.add(name);}
       #else
        juce::ignoreUnused(dir);
       #endif
    }
};

struct HostedPlugin
{
    juce::String key;
    juce::String path;
    juce::String format;
    juce::String displayName;
    juce::String target;
    juce::String targetId;
    int slot = 1;
    int midiChannel = kDefaultChannel;
    bool mixerInsert = false;
    bool isInstrument = false;
    bool enabled = true;
    bool editorIsGeneric = false;

    std::unique_ptr<juce::AudioPluginInstance> processor;
    std::unique_ptr<PluginEditorWindow> editor;
    juce::MidiBuffer pendingMidi;
    juce::AudioBuffer<float> scratch;
    double pcmSampleRate = 0.0;
    int pcmBlockSize = 0;
};

// Transport information for VST3 hosts (Ozone, synced delays, compressors).
// Only atomic reads are allowed in the audio callback.
class P12TransportClock final : public juce::AudioPlayHead
{
public:
    std::atomic<double> bpm { 96.0 }, sampleRate { 48000.0 };
    std::atomic<juce::int64> samples { 0 };
    std::atomic<bool> playing { false };
    std::atomic<int> soundingVoices { 0 };
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        const double tempo = bpm.load (std::memory_order_relaxed);
        const double sr = sampleRate.load (std::memory_order_relaxed);
        const auto at = samples.load (std::memory_order_relaxed);
        p.setBpm (tempo);
        juce::AudioPlayHead::TimeSignature timeSignature;
        timeSignature.numerator = 4;
        timeSignature.denominator = 4;
        p.setTimeSignature (timeSignature);
        p.setTimeInSamples (static_cast<int64_t>(at));
        p.setTimeInSeconds (static_cast<double> (at) / juce::jmax(8000.0,sr));
        p.setPpqPosition (static_cast<double> (at) * tempo / (60.0 * juce::jmax(8000.0,sr)));
        p.setIsPlaying (playing.load (std::memory_order_relaxed) ||
                        soundingVoices.load (std::memory_order_relaxed) > 0);
        return p;
    }
    void advance (int frames) noexcept
    {
        if (playing.load (std::memory_order_relaxed))
            samples.fetch_add (frames, std::memory_order_relaxed);
    }
};

class HostEngine final : public juce::AudioIODeviceCallback
{
public:
    HostEngine()
    {
        juce::addDefaultFormatsToManager (formatManager);
        // Precompute every mixer VST slot key on the CONTROL thread.
        // The real-time audio callback must not allocate JUCE strings.
        static constexpr const char* names[] = { "kick","snare","clap","rim","chat","ohat","cow","tom" };
        for (int pad = 0; pad < p12::kPads; ++pad) {
            const auto bank=juce::String::charToString(static_cast<juce_wchar>('A'+pad/8));
            for (int slot=0;slot<4;++slot)
                nativePadKeys[pad][slot] = "mixer:pad:" + bank + "_" + names[pad%8] + ":" + juce::String(slot+1);
        }
        for(auto& gain:nativePadControlGain)gain.store(.9f,std::memory_order_relaxed);
        for (int track=0;track<p12::kTracks;++track)
            for (int slot=0;slot<4;++slot)
                nativeTrackKeys[track][slot] = "mixer:track:" + juce::String(track) + ":" + juce::String(slot+1);
        for (int slot=0;slot<4;++slot)
            nativeMasterKeys[slot] = "mixer:master:0:"+juce::String(slot+1);
    }

    ~HostEngine() override
    {
        shutdown();
    }

    juce::var initialise()
    {
        const auto error = deviceManager.initialise (0, 2, nullptr, true);
        audioError = error;
        if (error.isEmpty())
        {
            deviceManager.addAudioCallback (this);
            audioCallbackAttached = true;
        }

        return statusObject (error.isEmpty(), error.isEmpty() ? "P-12 native VST host ready."
                                                               : "Native host started, but the default audio output could not be opened: " + error);
    }

    void shutdown()
    {
        if (audioCallbackAttached)
        {
            deviceManager.removeAudioCallback (this);
            audioCallbackAttached = false;
        }

        const juce::ScopedLock lock (instanceLock);
        for (auto& [key, inst] : instances)
        {
            inst->editor.reset();
            if (inst->processor != nullptr)
                inst->processor->releaseResources();
        }
        instances.clear();
    }

    juce::var handleCommand (const juce::String& command, const juce::var& payload)
    {
        if (command != "processPCM" && command != "nativeStatus" && command != "nativeMeters" && command != "nativeTrigger" && command != "nativeStop" && command != "midi")
            std::cerr << "[P12DIAG] command " << command << std::endl;
        if (command == "ping" || command == "status")
            return statusObject (true, "P-12 native VST host is running.");

        if (command == "loadVST3")
            return loadStandalone (payload, "VST3");

        if (command == "loadVST2")
            return loadStandalone (payload, "VST2");

        if (command == "renderVSTToWav") return renderVSTToWav (payload);
        if (command == "nativeSampleLoad") return nativeSampleLoad (payload);
        if (command == "nativeSampleClear") return nativeSampleClear();
        if (command == "nativeMode") return nativeModeSet (payload);
        if (command == "nativeTrigger") return nativeTrigger (payload);
        if (command == "nativeStop") return nativeStop (payload);
        if (command == "nativeMeters") return nativeMeters();
        if (command == "nativeStatus") return nativeStatus();
        if (command == "nativeBuiltinSync") return nativeBuiltinSync(payload);
        if (command == "nativePadControlsSync") return nativePadControlsSync(payload);
        if (command == "nativeTransport") return nativeTransport(payload);
        if (command == "nativeSequence") return nativeSequenceLoad(payload);
        if (command == "nativeBufferSize") return nativeBufferSize(payload);

        if (command == "processPCM")
            return processPCM (payload);

        if (command == "setMixerInsertVST")
            return setMixerInsert (payload);

        if (command == "unloadVST")
            return unloadStandalone (payload);

        if (command == "openEditor")
            return openEditor (payload);

        if (command == "closeEditor")
            return closeEditor (payload);

        if (command == "midi")
            return routeMidi (payload);

        if (command == "inspectInstrument")
            return inspectInstrument (payload);

        if (command == "allNotesOff")
            return allNotesOff();

        if (command == "getPluginState")
            return getPluginState (payload);

        if (command == "setPluginState")
            return setPluginState (payload);

        if (command == "testTone")
            return startTestTone();

        if (command == "listAudioOutputs")
            return listAudioOutputs();

        if (command == "setAudioOutput")
            return setAudioOutput (payload);

        return makeObject ({ { "ok", false }, { "message", "Unknown command: " + command } });
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        if (device == nullptr)
            return;

        const juce::ScopedLock lock (instanceLock);
        currentSampleRate = device->getCurrentSampleRate();
        nativePlayhead.sampleRate.store (currentSampleRate,std::memory_order_relaxed);
        currentBlockSize = device->getCurrentBufferSizeSamples();
        for (auto& [key, inst] : instances)
            prepareInstance (*inst);
        for(auto& controls:nativePadControls)controls.prepare(currentSampleRate);
        nativeSslBus.prepare(currentSampleRate);
        nativeBuiltinConfigs.clear();
        nativePadScratch.setSize (2, juce::jmax (currentBlockSize, 8192));
        nativeTrackScratch.setSize (2, juce::jmax (currentBlockSize, 8192));
        nativeMasterScratch.setSize (2, juce::jmax (currentBlockSize, 8192));
    }

    void audioDeviceStopped() override
    {
        const juce::ScopedLock lock (instanceLock);
        for (auto& [key, inst] : instances)
            if (inst->processor != nullptr)
                inst->processor->releaseResources();
    }

    void audioDeviceError (const juce::String& errorMessage) override
    {
        audioError = errorMessage;
    }

    void audioDeviceIOCallbackWithContext (const float* const* /*inputChannelData*/,
                                           int /*numInputChannels*/,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& /*context*/) override
    {
        const auto audioStarted=std::chrono::steady_clock::now();
        // Distinguish device/OS scheduling stalls from DSP overruns. A late
        // callback start with otherwise cheap DSP points at the Windows audio
        // driver/DPC scheduler rather than P-12's mixer or hosted plug-ins.
        if (lastCallbackStart.time_since_epoch().count() != 0 && currentSampleRate > 0.0)
        {
            const double intervalMs = std::chrono::duration<double,std::milli>(
                audioStarted - lastCallbackStart).count();
            const double expectedMs = 1000.0 * juce::jmax(1, lastCallbackSamples) / currentSampleRate;
            const double excessMs = juce::jmax(0.0, intervalMs - expectedMs);
            double old = nativeCallbackGapPeakMs.load(std::memory_order_relaxed);
            while (excessMs > old && ! nativeCallbackGapPeakMs.compare_exchange_weak(
                old, excessMs, std::memory_order_relaxed)) {}
            if (excessMs > expectedMs * 0.50)
                nativeLateCallbackStarts.fetch_add(1, std::memory_order_relaxed);
        }
        lastCallbackStart = audioStarted;
        lastCallbackSamples = numSamples;
        // Shared epoch used only for scheduling event onset within this block.
        // Capture it BEFORE plug-in DSP to avoid shifting notes by DSP time.
        const auto callbackEpochMs=juce::Time::currentTimeMillis();
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

        // A mode switch can request sampler stop while native rendering is
        // disabled. Service that request on the real audio thread even when
        // the native mixer is OFF so queued future events/voices cannot survive
        // and reappear when Native Audio is enabled again.
        if (! nativeMode.load (std::memory_order_acquire))
        {
            nativeSampler.serviceStopRequest();
            nativeVoiceCount.store (nativeSampler.activeVoices(), std::memory_order_relaxed);
            nativePlayhead.soundingVoices.store (nativeSampler.activeVoices(), std::memory_order_relaxed);
        }

        const juce::ScopedNoDenormals noDenormals;
        // Never wait for plugin configuration/UI work on the audio thread.
        // When the insert graph is momentarily unavailable, keep the native
        // sampler running through a dry path.  A missed lock must NOT discard
        // sequencer events or mute a whole hardware buffer.
        if (!instanceLock.tryEnter()) {
            nativeLockMisses.fetch_add(1, std::memory_order_relaxed);
            if (nativeMode.load(std::memory_order_acquire)) {
                prepareNativeSequence(numSamples,callbackEpochMs,true);
                renderNativeMixer(outputChannelData, numOutputChannels, numSamples, true, callbackEpochMs);
                nativeDryFallbacks.fetch_add(1, std::memory_order_relaxed);
            }
            nativePlayhead.advance(numSamples);
            // Include dry-fallback buffers in the callback deadline metrics.
            // They previously returned early and hid audio work from peak/slow.
            if(nativeMode.load(std::memory_order_relaxed)) {
                const double elapsed=std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-audioStarted).count();
                const double budget=1000.0*numSamples/juce::jmax(8000.0,currentSampleRate);
                if(elapsed>budget) nativeSlowCallbacks.fetch_add(1,std::memory_order_relaxed);
                if(elapsed>budget*0.75) nativeNearDeadlineCallbacks.fetch_add(1,std::memory_order_relaxed);
                const double overrun=juce::jmax(0.0,elapsed-budget);
                double oldOver=nativeWorstOverrunMs.load(std::memory_order_relaxed);
                while(overrun>oldOver && !nativeWorstOverrunMs.compare_exchange_weak(
                    oldOver,overrun,std::memory_order_relaxed)){}
                double prev=nativeWorstCallbackMs.load(std::memory_order_relaxed);
                while(elapsed>prev && !nativeWorstCallbackMs.compare_exchange_weak(
                    prev,elapsed,std::memory_order_relaxed)) {}
            }
            return;
        }
        struct UnlockOnReturn {
            juce::CriticalSection& cs;
            ~UnlockOnReturn() noexcept { cs.exit(); }
        } unlock {instanceLock};

        if(nativeMode.load()) prepareNativeSequence(numSamples,callbackEpochMs,false);
        processedBlocks.fetch_add (1, std::memory_order_relaxed);

        for (auto& [key, instPtr] : instances)
        {
            auto& inst = *instPtr;
            if (inst.mixerInsert || ! inst.enabled || inst.processor == nullptr || inst.processor->isSuspended())
                continue;

            const int pluginOutputs = juce::jmax (1, inst.processor->getTotalNumOutputChannels());
            const int pluginInputs = juce::jmax (0, inst.processor->getTotalNumInputChannels());
            const int channels = juce::jmax (2, juce::jmax (pluginInputs, pluginOutputs));

            // Never resize on the audio thread. Windows/ASIO/WASAPI can deliver
            // a callback SHORTER than the configured buffer size during device
            // transitions. Process that real frame count through a non-owning
            // view instead of dropping the entire VST block. A callback larger
            // than the prepared scratch capacity is still rejected safely.
            if (inst.scratch.getNumChannels() < channels ||
                inst.scratch.getNumSamples() < numSamples)
            {
                nativeOversizePluginCallbacks.fetch_add (1, std::memory_order_relaxed);
                continue;
            }
            juce::AudioBuffer<float> pluginBlock (inst.scratch.getArrayOfWritePointers(), channels, numSamples);
            pluginBlock.clear();
            inst.processor->processBlock (pluginBlock, inst.pendingMidi);
            inst.pendingMidi.clear();

            const int copyChannels = juce::jmin (numOutputChannels, pluginOutputs);
            bool blockHasAudio = false;
            for (int ch = 0; ch < copyChannels; ++ch)
            {
                if (outputChannelData[ch] != nullptr)
                {
                    const auto* src = pluginBlock.getReadPointer (ch);
                    if (pluginBlock.getMagnitude (ch, 0, numSamples) > 1.0e-7f)
                        blockHasAudio = true;
                    juce::FloatVectorOperations::add (outputChannelData[ch], src, numSamples);
                }
            }
            // Mono plug-ins should be audible in both speakers. The previous
            // standalone path copied only channel 0, unlike mixer inserts.
            if (pluginOutputs == 1 && numOutputChannels > 1 && outputChannelData[1] != nullptr)
            {
                const auto* src = pluginBlock.getReadPointer (0);
                juce::FloatVectorOperations::add (outputChannelData[1], src, numSamples);
            }
            if (blockHasAudio)
                nonZeroPluginBlocks.fetch_add (1, std::memory_order_relaxed);
        }

        // P-12 phase-1 native sampler: all sample voices, pad/track/master VST3
        // inserts and output share this device callback — no PCM IPC roundtrip.
        if (nativeMode.load (std::memory_order_acquire))
        {
            const auto mixerStart = std::chrono::steady_clock::now();
            renderNativeMixer (outputChannelData, numOutputChannels, numSamples, false, callbackEpochMs);
            const double mixerMs = std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now() - mixerStart).count();
            double old = nativeMixerPeakMs.load(std::memory_order_relaxed);
            while (mixerMs > old && ! nativeMixerPeakMs.compare_exchange_weak(
                old, mixerMs, std::memory_order_relaxed)) {}
        }

        nativePlayhead.advance(numSamples);
        // Built-in diagnostic tone. This proves the native host has a live
        // Windows output device independently of any third-party plug-in.
        int remaining = testToneSamplesRemaining.load (std::memory_order_relaxed);
        if (remaining > 0 && currentSampleRate > 0.0)
        {
            const int count = juce::jmin (remaining, numSamples);
            const double phaseStep = juce::MathConstants<double>::twoPi * 440.0 / currentSampleRate;
            for (int i = 0; i < count; ++i)
            {
                const float v = static_cast<float> (std::sin (testTonePhase) * 0.12);
                testTonePhase += phaseStep;
                if (testTonePhase >= juce::MathConstants<double>::twoPi)
                    testTonePhase -= juce::MathConstants<double>::twoPi;
                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (outputChannelData[ch] != nullptr)
                        outputChannelData[ch][i] += v;
            }
            testToneSamplesRemaining.fetch_sub (count, std::memory_order_relaxed);
        }
        if(nativeMode.load(std::memory_order_relaxed)) {
            const auto elapsed=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-audioStarted).count();
            const double budget=1000.0*numSamples/juce::jmax(8000.0,currentSampleRate);
            if(elapsed>budget)
                nativeSlowCallbacks.fetch_add(1,std::memory_order_relaxed);
            if(elapsed>budget*0.75)
                nativeNearDeadlineCallbacks.fetch_add(1,std::memory_order_relaxed);
            const double overrun=juce::jmax(0.0,elapsed-budget);
            double oldOver=nativeWorstOverrunMs.load(std::memory_order_relaxed);
            while(overrun>oldOver && !nativeWorstOverrunMs.compare_exchange_weak(
                oldOver,overrun,std::memory_order_relaxed)){}
            double previous=nativeWorstCallbackMs.load(std::memory_order_relaxed);
            while(elapsed>previous && !nativeWorstCallbackMs.compare_exchange_weak(
                previous,elapsed,std::memory_order_relaxed)){}
        }
    }

private:
    struct BuiltinFX {
        p12::InsertDSP dsp;
        bool active() const noexcept {return dsp.active();}
        void configure(const juce::var& v,double rate){
            const auto params=property(v,"params");
            const auto type=stringProperty(v,"type","off").toStdString();
            const bool supported=dsp.configure(type,boolProperty(v,"on",true),rate,[&](const char* key,float fallback){
                const auto value=property(params,key);
                const float n=value.isVoid()||value.isUndefined()?fallback:static_cast<float>(static_cast<double>(value));
                return std::isfinite(n)?n:fallback;
            });
            if(!supported)std::cerr<<"[P12DIAG] Unsupported built-in insert: "<<type<<std::endl;
        }
        void process(juce::AudioBuffer<float>& buffer,int n) noexcept {
            dsp.process(buffer.getWritePointer(0),buffer.getWritePointer(1),n);
        }
    };
    std::array<p12::PadControlsDSP,p12::kPads> nativePadControls;
    std::map<juce::String,juce::String> nativeBuiltinConfigs;
    std::array<std::atomic<int>,p12::kPads> nativePadVirtualTrack{};
    std::array<std::atomic<float>,p12::kPads> nativePadControlGain{},nativePadControlPan{};
    p12::MixerGain nativeMixerGain;
    p12::BusCompressor nativeSslBus;
    std::array<p12::SpectrumTap,p12::kPads> padSpectra;
    std::array<p12::SpectrumTap,p12::kTracks> trackSpectra;
    p12::SpectrumTap masterSpectrum;
    std::atomic<int> spectrumLease{0};
    unsigned spectrumPoll=0;
    std::atomic<float> track0Pan{0};
    std::atomic<bool> track0Solo{false};
    juce::var nativePadControlsSync(const juce::var& payload) {
        const auto value=property(payload,"pads");const auto* pads=value.getArray();
        if(!pads)return makeObject({{"ok",false},{"message","Missing pad controls"}});
        const auto master=property(payload,"master");
        if(master.isObject())nativeMixerGain.setMaster(static_cast<float>(static_cast<double>(property(master,"gain"))));
        const auto bus=property(master,"sslComp");
        if(bus.isObject()){
            auto number=[&](const char* key,float fallback){const auto v=property(bus,key);return v.isVoid()?fallback:static_cast<float>(static_cast<double>(v));};
            nativeSslBus.configure(boolProperty(bus,"on",false),number("threshold",-10),number("ratio",4),number("attack",10),number("release",300),number("makeup",0),number("mix",100));
        }
        const auto tracks=property(payload,"tracks");
        if(const auto* list=tracks.getArray())for(const auto& item:*list)
            nativeMixerGain.setTrack(intProperty(item,"index",-1),static_cast<float>(static_cast<double>(property(item,"gain"))));
        // No instanceLock, allocation, or DSP history reset. UI only publishes scalars.
        const auto zero=property(payload,"track0");
        if(zero.isObject()){
            track0Solo.store(boolProperty(zero,"solo",false));
            const float gain=static_cast<float>(static_cast<double>(property(zero,"gain"))),pan=static_cast<float>(static_cast<double>(property(zero,"pan")));
            if(!tracks.isArray()&&std::isfinite(gain))nativeMixerGain.setTrack(p12::kVstTrack0,gain);if(std::isfinite(pan))track0Pan.store(juce::jlimit(-1.0f,1.0f,pan));
        }
        for(const auto& item:*pads){const int pad=intProperty(item,"pad",-1);if(pad<0||pad>=p12::kPads)continue;
            auto number=[&](const char* key,float fallback){auto v=property(item,key);const float x=v.isVoid()?fallback:static_cast<float>(static_cast<double>(v));return std::isfinite(x)?x:fallback;};
            nativePadVirtualTrack[pad].store(juce::jlimit(0,p12::kTracks-1,intProperty(item,"track",0)));
            nativePadControlGain[pad].store(number("gain",.9f));nativePadControlPan[pad].store(number("pan",0));
            nativeSampler.setPadControls(pad,number("gain",.9f),number("pan",0),boolProperty(item,"spCharacter",true),number("tuneRatio",1));
            const auto mode=stringProperty(item,"filter","off");
            nativePadControls[pad].configure(mode=="lp"?1:mode=="hp"?2:0,number("frequency",14000),number("q",.3f),number("compensation",1),number("reverb",0),number("crush",0));
        }
        return makeObject({{"ok",true}});
    }
    std::array<std::array<BuiltinFX,4>,p12::kPads> builtinPad;
    std::array<std::array<BuiltinFX,4>,p12::kTracks> builtinTrack;
    std::array<BuiltinFX,4> builtinMaster;
    juce::var nativeBuiltinSync(const juce::var& payload) {
        const auto routeValue=property(payload,"routes");
        auto* items=routeValue.getArray();
        if(!items)return makeObject({{"ok",false},{"message","Missing built-in routes"}});
        const juce::ScopedLock lock(instanceLock);
        int updated=0;
        for(const auto& entry:*items) {
            const auto kind=stringProperty(entry,"kind");
            const auto index=intProperty(entry,"index",0);
            const auto slotValue=property(entry,"slots");
            auto* slots=slotValue.getArray();
            if(!slots)continue;
            std::array<BuiltinFX,4>* dest=nullptr;
            if(kind=="pad" && index>=0 && index<p12::kPads)dest=&builtinPad[static_cast<size_t>(index)];
            if(kind=="track" && index>=0 && index<p12::kTracks)dest=&builtinTrack[static_cast<size_t>(index)];
            if(kind=="master")dest=&builtinMaster;
            if(!dest)continue;
            for(int i=0;i<4;i++) {
                const auto v=i<slots->size()?slots->getReference(i):juce::var();
                const auto key=kind+":"+juce::String(index)+":"+juce::String(i);
                const auto encoded=juce::JSON::toString(v,true);
                if(nativeBuiltinConfigs[key]==encoded)continue;
                (*dest)[static_cast<size_t>(i)].configure(v,currentSampleRate);
                nativeBuiltinConfigs[key]=encoded;
            }
            ++updated;
        }
        return makeObject({{"ok",true},{"routes",updated}});
    }
    std::array<std::pair<int,bool>,32> nativeClicks{};
    int nativeClickCount=0,nativeClickAge=-1;
    bool nativeClickAccent=false;
    void prepareNativeSequence(int n,juce::int64 epoch,bool bypass) noexcept
    {
        nativeSampler.beginBlock(epoch,currentSampleRate);
        nativeClickCount=0;if(!nativeSequenceClock.playing.load())nativeClickAge=-1;
        nativeSequenceClock.process(n,currentSampleRate,epoch,[&](const p12::SequenceEvent& e,int offset){
            if(e.click){if(nativeClickCount<int(nativeClicks.size()))nativeClicks[nativeClickCount++]={offset,e.accent};return;}
            // Keep muted notes in the immutable plan. Gate new hits/chokes live,
            // while MIDI note-offs always pass to prevent stuck instrument notes.
            if(e.sourcePad>=0&&e.sourcePad<p12::kPads&&nativePadControlGain[e.sourcePad].load(std::memory_order_relaxed)<=0&&
               (!e.midi||((e.bytes[0]&0xf0)==0x90&&e.bytes[2]>0)))return;
            if(!e.midi){auto x=e.sample;x.delayFrames=offset;nativeSampler.applyRealtimeEvent(x);return;}
            if(bypass)return;
            // Only called with instanceLock already held. Preallocated MIDI
            // buffers keep sequence note dispatch out of the allocator.
            const juce::MidiMessage m(e.bytes.data(),e.byteCount,0.0);
            for(auto& [key,inst]:instances){
                bool match=false;
                if(e.routeKind<0)match=!inst->mixerInsert && (e.routeIndex>0?inst->slot==e.routeIndex:inst->midiChannel==e.channel);
                else if(e.slot>=1&&e.slot<=4){
                    if(e.routeKind==0&&e.routeIndex>=0&&e.routeIndex<p12::kPads)match=key==nativePadKeys[e.routeIndex][e.slot-1];
                    if(e.routeKind==1&&e.routeIndex>=0&&e.routeIndex<p12::kTracks)match=key==nativeTrackKeys[e.routeIndex][e.slot-1];
                    if(e.routeKind==2)match=key==nativeMasterKeys[e.slot-1];
                }
                if(match&&inst->enabled)inst->pendingMidi.addEvent(m,offset);
            }
        });
    }
    void renderNativeMixer (float* const* out, int outputChannels, int n,
                            bool bypassInserts = false,
                            juce::int64 callbackEpochMs = 0) noexcept
    {
        if (n <= 0 || n > nativePadScratch.getNumSamples() ||
            n > nativeMasterScratch.getNumSamples()) return;
        // beginBlock and sequence dispatch ran once before plugin processing.
        nativeSampler.refreshRoutes();
        nativeMasterScratch.clear (0, n);
        int click=0;
        for(int i=0;i<n;++i){
            if(click<nativeClickCount&&nativeClicks[click].first==i){nativeClickAge=0;nativeClickAccent=nativeClicks[click++].second;}
            if(nativeClickAge>=0){
                const double age=nativeClickAge/currentSampleRate;
                if(age>=.06){nativeClickAge=-1;continue;}
                const float value=static_cast<float>((nativeClickAccent ? .5 : .32)*std::exp(-age*170)*std::sin(2*3.141592653589793*(nativeClickAccent?1500:1000)*age));
                nativeMasterScratch.addSample(0,i,value);nativeMasterScratch.addSample(1,i,value);++nativeClickAge;
            }
        }
        std::array<float,p12::kPads> blockPadPeaks{};
        const bool captureSpectrum=spectrumLease.load(std::memory_order_relaxed)>0;
        if(captureSpectrum)spectrumLease.fetch_sub(n,std::memory_order_relaxed);
        std::array<bool,p12::kPads> capturedPads{};

        std::array<float,p12::kTracks> blockTrackPeaks{};
        // Track index is the actual 0-based P-12 track identity. Each pad's
        // processing is independent before summing through one track insert.
        for (int track = 0; track < p12::kTracks; ++track)
        {
            nativeTrackScratch.clear (0, n);
            bool hasTrack = false;
            for (int pad = 0; pad < p12::kPads; ++pad)
            {
                const bool hasSample = nativeSampler.hasActiveRoute(pad, track);
                // Inserts continue processing after sample voices end, preserving FX tails.
                // Empty/sustained pad instruments and FX tails follow the explicit pad/sub route,
                // including additional Track 0, once per callback.
                bool virtualInstrumentPad = !hasSample && track == nativePadVirtualTrack[pad].load(std::memory_order_relaxed) &&
                                            !bypassInserts && hasActiveInsert (0, pad);
                // An instrument on a pad with a live voice routed to Track 2–8
                // must not also be rendered on Track 1 in the same callback.
                if (virtualInstrumentPad)
                    for (int otherTrack = 0; otherTrack < p12::kTracks; ++otherTrack)
                        if (otherTrack!=track && nativeSampler.hasActiveRoute (pad, otherTrack))
                        { virtualInstrumentPad = false; break; }
                const bool padTail=nativePadControls[pad].hasTail(track);
                if (!hasSample && !virtualInstrumentPad && !padTail) continue;
                nativePadScratch.clear (0, n);
                if (hasSample)
                    nativeSampler.renderBus (pad, track,
                        nativePadScratch.getWritePointer (0),
                        nativePadScratch.getWritePointer (1), n, currentSampleRate);
                hasTrack = true;
                if (!bypassInserts) processNativeChain (0, pad, nativePadScratch, n);
                if(virtualInstrumentPad&&hasInstrumentInsert(0,pad)){const float gain=juce::jlimit(0.0f,16.0f,nativePadControlGain[pad].load()),pan=juce::jlimit(-1.0f,1.0f,nativePadControlPan[pad].load());
                    nativePadScratch.applyGain(0,0,n,gain*std::sqrt(1.0f-juce::jmax(0.0f,pan)));
                    nativePadScratch.applyGain(1,0,n,gain*std::sqrt(1.0f+juce::jmin(0.0f,pan)));
                }
                nativePadControls[pad].process(nativePadScratch.getWritePointer(0),nativePadScratch.getWritePointer(1),n,track,hasSample||virtualInstrumentPad);
                if(captureSpectrum){padSpectra[pad].capture(nativePadScratch.getReadPointer(0),nativePadScratch.getReadPointer(1),n);capturedPads[pad]=true;}
                blockPadPeaks[static_cast<size_t>(pad)] = juce::jmax(
                    blockPadPeaks[static_cast<size_t>(pad)], juce::jmax(nativePadScratch.getMagnitude(0,n),nativePadScratch.getMagnitude(1,n)));
                for (int ch = 0; ch < 2; ++ch)
                    nativeTrackScratch.addFrom(ch, 0, nativePadScratch, ch, 0, n);
            }
            // Track VST instruments require no sample/pad events. Run the
            // generator while stopped too, to let held keys and releases sound.
            if (!hasTrack && !bypassInserts && hasActiveInsert (1, track)) hasTrack = true;
            if (hasTrack)
            {
                if (!bypassInserts) processNativeChain (1, track, nativeTrackScratch, n);
                nativeMixerGain.processTrack(track,nativeTrackScratch.getWritePointer(0),nativeTrackScratch.getWritePointer(1),n);
                if(track==p12::kVstTrack0){const float pan=track0Pan.load();
                    nativeTrackScratch.applyGain(0,0,n,std::sqrt(1.0f-juce::jmax(0.0f,pan)));
                    nativeTrackScratch.applyGain(1,0,n,std::sqrt(1.0f+juce::jmin(0.0f,pan)));
                }
                if(captureSpectrum)trackSpectra[track].capture(nativeTrackScratch.getReadPointer(0),nativeTrackScratch.getReadPointer(1),n);
                blockTrackPeaks[static_cast<size_t>(track)] = juce::jmax(nativeTrackScratch.getMagnitude(0,n),nativeTrackScratch.getMagnitude(1,n));
                for (int ch = 0; ch < 2; ++ch)
                    if(!track0Solo.load()||track==p12::kVstTrack0)nativeMasterScratch.addFrom(ch, 0, nativeTrackScratch, ch, 0, n);
            }
        }
        if(captureSpectrum){
            for(int pad=0;pad<p12::kPads;pad++)if(!capturedPads[pad])padSpectra[pad].capture(nullptr,nullptr,n);
            for(int track=0;track<p12::kTracks;track++)if(blockTrackPeaks[track]==0)trackSpectra[track].capture(nullptr,nullptr,n);
        }
        nativeSampler.endBlock(n);
        // Peak-hold/decay telemetry for Mixer 32. These atomics are visual only:
        // no audio leaves the native callback and no JS processing is introduced.
        for(size_t i=0;i<blockPadPeaks.size();++i){
            captureMeter(meterPadPeaks[i],blockPadPeaks[i]);
            const float prior=nativePadPeaks[i].load(std::memory_order_relaxed);
            nativePadPeaks[i].store(juce::jmax(blockPadPeaks[i],prior*0.92f),std::memory_order_relaxed);
        }
        for(size_t i=0;i<blockTrackPeaks.size();++i){
            captureMeter(meterTrackPeaks[i],blockTrackPeaks[i]);
            const float prior=nativeTrackPeaks[i].load(std::memory_order_relaxed);
            nativeTrackPeaks[i].store(juce::jmax(blockTrackPeaks[i],prior*0.92f),std::memory_order_relaxed);
        }
        nativeInputPeak.store(nativeMasterScratch.getMagnitude(0,n),std::memory_order_relaxed);
        // Master always runs during native mode so Ozone's meters and
        // Master Assistant see a continuous stream, including quiet gaps.
        if (!bypassInserts) processNativeChain (2, 0, nativeMasterScratch, n);
        nativeSslBus.process(nativeMasterScratch.getWritePointer(0),nativeMasterScratch.getWritePointer(1),n);
        nativeMixerGain.processMaster(nativeMasterScratch.getWritePointer(0),nativeMasterScratch.getWritePointer(1),n);
        if(captureSpectrum)masterSpectrum.capture(nativeMasterScratch.getReadPointer(0),nativeMasterScratch.getReadPointer(1),n);
        {
            const float now=juce::jmax(nativeMasterScratch.getMagnitude(0,n),nativeMasterScratch.getMagnitude(1,n));
            captureMeter(meterOutputPeak,now);
            const float prior=nativeOutputPeak.load(std::memory_order_relaxed);
            nativeOutputPeak.store(juce::jmax(now,prior*0.92f),std::memory_order_relaxed);
        }
        for(int ch=0; ch < juce::jmin(2,outputChannels); ++ch)
        {
            if (out[ch] == nullptr) continue;
            juce::FloatVectorOperations::add (out[ch],nativeMasterScratch.getReadPointer(ch),n);
        }
        nativeVoiceCount.store(nativeSampler.activeVoices(),std::memory_order_relaxed);
        nativePlayhead.soundingVoices.store(nativeSampler.activeVoices(),std::memory_order_relaxed);
        nativeRenderedBlocks.fetch_add(1,std::memory_order_relaxed);
    }

    // Called only with instanceLock held by the native callback.
    bool hasInstrumentInsert (int kind, int index) const noexcept
    {
        const auto& keys = kind == 0 ? nativePadKeys[index] : nativeTrackKeys[index];
        for (int slot = 0; slot < 4; ++slot)
        {
            const auto it = instances.find (keys[slot]);
            if (it != instances.end() && it->second->mixerInsert &&
                it->second->enabled && it->second->isInstrument &&
                it->second->processor != nullptr && !it->second->processor->isSuspended())
                return true;
        }
        return false;
    }

    bool hasActiveInsert(int kind,int index) const noexcept {
        const auto& builtins=kind==0?builtinPad[index]:builtinTrack[index];
        for(const auto& slot:builtins)if(slot.active())return true;
        const auto& keys=kind==0?nativePadKeys[index]:nativeTrackKeys[index];
        for(const auto& key:keys){const auto it=instances.find(key);
            if(it!=instances.end()&&it->second->enabled&&it->second->mixerInsert&&it->second->processor&&!it->second->processor->isSuspended())return true;}
        return false;
    }

    void processNativeChain(int kind,int index,
                            juce::AudioBuffer<float>& audio,int n) noexcept
    {
        const auto& keys = kind==0 ? nativePadKeys[index] :
                           kind==1 ? nativeTrackKeys[index] : nativeMasterKeys;
        auto& builtins=kind==0?builtinPad[static_cast<size_t>(index)]:
                       kind==1?builtinTrack[static_cast<size_t>(index)]:builtinMaster;
        for(int slot=0;slot<4;++slot)
        {
            auto& builtin=builtins[static_cast<size_t>(slot)];
            if(builtin.active()){
                builtin.process(audio,n);
                nativeProcessedInserts.fetch_add(1,std::memory_order_relaxed);
                nativeInsertByKind[static_cast<size_t>(kind)].fetch_add(1,std::memory_order_relaxed);
                if(kind==1)nativeTrackFxBlocks[static_cast<size_t>(index)].fetch_add(1,std::memory_order_relaxed);
            }
            const auto it=instances.find(keys[slot]);
            if(it==instances.end()) continue;
            auto& inst=*it->second;
            if(!inst.enabled || !inst.mixerInsert || inst.processor==nullptr ||
               inst.processor->isSuspended()) continue;
            const int inputs=inst.processor->getTotalNumInputChannels();
            const int outputs=inst.processor->getTotalNumOutputChannels();
            const int channels=juce::jmax(2,juce::jmax(inputs,outputs));
            if(outputs < 1 ||
               inst.scratch.getNumChannels() < channels ||
               inst.scratch.getNumSamples()<n) continue;
            // Prepared VST buffers are allocated outside the callback. Never
            // resize/allocate in the realtime thread. Unexpected partial
            // device callbacks bypass this insert until a safe reprepare.
            // A device callback can contain fewer frames than the prepared
            // maximum; a non-owning view keeps processBlock exactly aligned.
            juce::AudioBuffer<float> block(inst.scratch.getArrayOfWritePointers(), channels, n);
            block.clear();
            for(int ch=0;ch<juce::jmin(inputs,2);++ch)
                block.copyFrom(ch,0,audio,ch,0,n);
            // Consume the MIDI messages queued by Piano Roll, CHORDS, and
            // sequencer hits. The former empty MIDI buffer made every VSTi silent.
            const auto pluginStart=std::chrono::steady_clock::now();
            const bool hadInstrumentMidi = inst.isInstrument && !inst.pendingMidi.isEmpty();
            inst.processor->processBlock(block,inst.pendingMidi);
            inst.pendingMidi.clear();
            if (inst.isInstrument)
            {
                nativeInstrumentBlocks.fetch_add (1, std::memory_order_relaxed);
                if (hadInstrumentMidi) nativeInstrumentMidiBlocks.fetch_add (1, std::memory_order_relaxed);
                if (block.getMagnitude (0, n) > 1.0e-7f)
                    nativeInstrumentAudibleBlocks.fetch_add (1, std::memory_order_relaxed);
            }
            const double pluginMs=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-pluginStart).count();
            // Lightweight DSP-only timing; GUI time and JS timing are
            // measured separately. No logging/heap allocation on callback.
            auto& peak=(kind==2 ? nativeMasterVstPeakMs :
                        kind==1 ? nativeTrackVstPeakMs : nativePadVstPeakMs);
            auto oldPeak=peak.load(std::memory_order_relaxed);
            while (pluginMs>oldPeak && !peak.compare_exchange_weak(
                oldPeak,pluginMs,std::memory_order_relaxed)) {}
            if (kind != 2)
            {
                double combined = nativeInsertVstPeakMs.load(std::memory_order_relaxed);
                while (pluginMs > combined && ! nativeInsertVstPeakMs.compare_exchange_weak(
                    combined, pluginMs, std::memory_order_relaxed)) {}
            }
            if (pluginMs>1000.0*n/juce::jmax(8000.0,currentSampleRate)*0.75)
            {
                nativeVstCostlyBlocks.fetch_add(1,std::memory_order_relaxed);
                nativeCostlyKind.store(kind,std::memory_order_relaxed);
                nativeCostlyIndex.store(index,std::memory_order_relaxed);
                nativeCostlySlot.store(slot,std::memory_order_relaxed);
            }
            for(int ch=0;ch<2;++ch)
                audio.copyFrom(ch,0,block,juce::jmin(ch,outputs-1),0,n);
            nativeProcessedInserts.fetch_add(1,std::memory_order_relaxed);
            nativeInsertByKind[static_cast<size_t>(kind)].fetch_add(1,std::memory_order_relaxed);
            if (kind == 1) nativeTrackFxBlocks[static_cast<size_t>(index)].fetch_add(1,std::memory_order_relaxed);
        }
    }

    p12::SequenceClock nativeSequenceClock;
    std::vector<std::unique_ptr<p12::SequencePlan>> sequencePlans;
    juce::String lastRestartToken;
    bool nativeTransportPaused=false;
    juce::var nativeSequenceLoad(const juce::var& payload)
    {
        auto plan=std::make_unique<p12::SequencePlan>();
        plan->immediate=boolProperty(payload,"immediate",false);
        plan->duration=static_cast<double>(property(payload,"duration"));
        plan->bpm=static_cast<double>(property(payload,"bpm"));
        plan->id=static_cast<uint64_t>(static_cast<double>(property(payload,"id")));
        const auto eventValues=property(payload,"events");
        const auto* events=eventValues.getArray();
        if(!events || events->size()>32768 || !std::isfinite(plan->duration) || plan->duration<=0 ||
           plan->duration>86400 || !std::isfinite(plan->bpm) || plan->bpm<20 || plan->bpm>400)
            return makeObject({{"ok",false},{"message","Invalid native sequence"}});
        // Retain immutable plans for audio pointer lifetime; bounded to one
        // session. Never delete a plan that the callback could still be reading.
        if(sequencePlans.size()>=512)return makeObject({{"ok",false},{"message","Sequence plan limit; restart P-12 safely"}});
        for(const auto& v:*events){
            p12::SequenceEvent e;
            e.sourcePad=intProperty(v,"sourcePad",-1);
            e.seconds=static_cast<double>(property(v,"seconds"));
            if(!std::isfinite(e.seconds)||e.seconds<0)continue;
            e.seconds=std::fmod(e.seconds,plan->duration);
            if(boolProperty(v,"external",false))continue;
            e.click=boolProperty(v,"click",false);e.accent=boolProperty(v,"accent",false);
            e.midi=boolProperty(v,"midi",false);
            if(e.midi){
                e.routeKind=intProperty(v,"routeKind",-1); e.routeIndex=intProperty(v,"routeIndex",0);
                e.slot=intProperty(v,"slot",1);e.channel=intProperty(v,"channel",1);
                auto m=midiMessageFromPayload(v);
                if(m.getRawDataSize()<1||m.getRawDataSize()>3)continue;
                e.byteCount=m.getRawDataSize();std::copy_n(m.getRawData(),e.byteCount,e.bytes.begin());
            }else if(!e.click){
                auto& x=e.sample;
                x.type=boolProperty(v,"stop",false)?p12::Event::StopPad:p12::Event::Play;
                x.pad=intProperty(v,"pad",-1);x.track=intProperty(v,"track",0);
                x.velocity=static_cast<float>(static_cast<double>(property(v,"velocity")));
                x.ratio=static_cast<float>(static_cast<double>(property(v,"ratio")));
                x.pan=static_cast<float>(static_cast<double>(property(v,"pan")));
                x.trimStart=static_cast<float>(static_cast<double>(property(v,"start")));
                x.trimEnd=static_cast<float>(static_cast<double>(property(v,"end")));
                x.spCharacter=boolProperty(v,"spCharacter",true);
                x.livePadControls=boolProperty(v,"livePadControls",false);
                if(x.pad<0||x.pad>=p12::kPads||x.track<0||x.track>=p12::kTracks||
                   !std::isfinite(x.velocity)||!std::isfinite(x.ratio))continue;
            }
            plan->events.push_back(e);
        }
        std::stable_sort(plan->events.begin(),plan->events.end(),[](const auto& a,const auto& b){return a.seconds<b.seconds;});
        auto* ptr=plan.get();sequencePlans.push_back(std::move(plan));
        nativeSequenceClock.published.store(ptr,std::memory_order_release);
        return makeObject({{"ok",true},{"id",static_cast<double>(ptr->id)},{"events",static_cast<int>(ptr->events.size())}});
    }
    juce::var nativeTransport(const juce::var& payload)
    {
        const double tempo = static_cast<double>(property(payload,"bpm"));
        if(tempo>=20.0 && tempo<=400.0){nativePlayhead.bpm.store(tempo);nativeSequenceClock.tempo.store(tempo);}
        const bool wanted=boolProperty(payload,"playing",nativeSequenceClock.playing.load());
        const bool was=nativeSequenceClock.playing.load();
        const auto token=stringProperty(payload,"restartToken");
        const bool restart=boolProperty(payload,"reset",false) && token.isNotEmpty() && token!=lastRestartToken;
        if(wanted && !was && boolProperty(payload,"resume",false) && nativeTransportPaused){
            nativeSequenceClock.playing.store(true,std::memory_order_release);nativeTransportPaused=false;
        }else if(wanted && (!was||restart)){
            nativeTransportPaused=false;
            lastRestartToken=token;
            nativePlayhead.samples.store(0);
            nativeSequenceClock.start(static_cast<juce::int64>(static_cast<double>(property(payload,"startEpochMs"))));
        }else if(!wanted && was){nativeSequenceClock.stop();nativeTransportPaused=boolProperty(payload,"pause",false);if(!nativeTransportPaused){nativeSampler.requestStop();allNotesOff();}}
        nativePlayhead.playing.store(wanted);
        return makeObject({{"ok",true},{"playing",wanted},{"redundant",wanted==was&&!restart}});
    }
    juce::var nativeModeSet(const juce::var& payload)
    {
        // Control thread only. Re-prepare processors for the DEVICE clock,
        // not the browser clock/2048-frame IPC bridge left in legacy mode.
        // GUI thread dispatch, with the audio graph locked briefly.
        const bool enabled=boolProperty(payload,"enabled",false);
        nativeMode.store(false,std::memory_order_release);
        nativePlayhead.playing.store(false,std::memory_order_relaxed);
        nativeSequenceClock.stop();
        if(!enabled) nativePlayhead.samples.store(0,std::memory_order_relaxed);
        nativeSampler.requestStop();
        {
            const juce::ScopedLock lock(instanceLock);
            for(auto& [key, inst]:instances)
                if(inst->mixerInsert && inst->processor!=nullptr) {
                    prepareInstance(*inst);
                    inst->pcmSampleRate=0;
                    inst->pcmBlockSize=0;
                }
        }
        // Diagnostics are per native-audio session, not lifetime totals.
        if (enabled) {
            nativeSampler.resetDiagnostics();
            nativeLockMisses.store(0,std::memory_order_relaxed);
            nativeDryFallbacks.store(0,std::memory_order_relaxed);
            nativeSlowCallbacks.store(0,std::memory_order_relaxed);
            nativeWorstCallbackMs.store(0.0,std::memory_order_relaxed);
            nativeWorstOverrunMs.store(0.0,std::memory_order_relaxed);
            nativeCallbackGapPeakMs.store(0.0,std::memory_order_relaxed);
            nativeMixerPeakMs.store(0.0,std::memory_order_relaxed);
            nativeMasterVstPeakMs.store(0.0,std::memory_order_relaxed);
            nativeTrackVstPeakMs.store(0.0,std::memory_order_relaxed);
            nativePadVstPeakMs.store(0.0,std::memory_order_relaxed);
            nativeInsertVstPeakMs.store(0.0,std::memory_order_relaxed);
            nativeNearDeadlineCallbacks.store(0,std::memory_order_relaxed);
            nativeLateCallbackStarts.store(0,std::memory_order_relaxed);
            nativeVstCostlyBlocks.store(0,std::memory_order_relaxed);
            lastCallbackStart={};
            lastCallbackSamples=currentBlockSize;
            nativeProcessedInserts.store(0,std::memory_order_relaxed);
            for (auto& counter:nativeInsertByKind) counter.store(0,std::memory_order_relaxed);
            for (auto& counter:nativeTrackFxBlocks) counter.store(0,std::memory_order_relaxed);
        }
        nativeMode.store(enabled,std::memory_order_release);
        return nativeStatus();
    }
    juce::var nativeSampleClear() {
        // Project switch: prevent old pad assignments leaking into a new image.
        // The sampler retains audio memory to protect in-flight callback pointers.
        nativeSampler.clearPads();
        return makeObject({{"ok",true},{"message","Native pad assignments cleared"}});
    }
    juce::var nativeSampleLoad(const juce::var& payload)
    {
        const int pad=intProperty(payload,"pad",-1);
        const int frames=intProperty(payload,"frames",0);
        const int rate=intProperty(payload,"sampleRate",0);
        if(pad<0 || pad>=p12::kPads || frames<2 || frames>192000*60 ||
           rate<8000 || rate>192000)
            return makeObject({{"ok",false},{"message","Invalid pad, frames, or rate"}});
        juce::MemoryOutputStream stream;
        if(!juce::Base64::convertFromBase64(stream,stringProperty(payload,"pcm")) ||
           stream.getDataSize()!=static_cast<size_t>(frames)*2*sizeof(float))
            return makeObject({{"ok",false},{"message","Expected planar stereo Float32LE in RFC4648 Base64"}});
        auto sample=std::make_unique<p12::Sample>();
        sample->rate=rate;
        sample->left.resize(static_cast<size_t>(frames));
        sample->right.resize(static_cast<size_t>(frames));
        const char* src=static_cast<const char*>(stream.getData());
        // Read aligned float values via memcpy (MemoryBlock need not be aligned).
        for(int i=0;i<frames;++i)
        {
            float a=0,b=0;
            std::memcpy(&a,src+static_cast<size_t>(i)*4,4);
            std::memcpy(&b,src+(static_cast<size_t>(frames)+i)*4,4);
            sample->left[static_cast<size_t>(i)]=std::isfinite(a)?juce::jlimit(-16.0f,16.0f,a):0.0f;
            sample->right[static_cast<size_t>(i)]=std::isfinite(b)?juce::jlimit(-16.0f,16.0f,b):0.0f;
        }
        const bool ok=nativeSampler.load(pad,std::move(sample));
        return makeObject({{"ok",ok},{"message",ok?"Sample staged in native mixer":"Load refused; check sample memory limit"}});
    }
    juce::var nativeTrigger(const juce::var& payload)
    {
        p12::Event e;
        e.type=p12::Event::Play;
        e.pad=intProperty(payload,"pad",-1);
        e.track=intProperty(payload,"track",0);
        e.delayFrames=intProperty(payload,"delayFrames",0);
        e.targetEpochMs=static_cast<juce::int64>(static_cast<double>(property(payload,"targetEpochMs")));
        e.submittedEpochMs=static_cast<juce::int64>(static_cast<double>(property(payload,"submittedEpochMs")));
        e.velocity=static_cast<float>(static_cast<double>(property(payload,"velocity")));
        e.ratio=static_cast<float>(static_cast<double>(property(payload,"ratio")));
        e.pan=static_cast<float>(static_cast<double>(property(payload,"pan")));
        e.trimStart=static_cast<float>(static_cast<double>(property(payload,"start")));
        e.trimEnd=static_cast<float>(static_cast<double>(property(payload,"end")));
        e.spCharacter=boolProperty(payload,"spCharacter",true);
        e.livePadControls=boolProperty(payload,"livePadControls",false);
        if(e.pad<0||e.pad>=p12::kPads ||e.track<0||e.track>=p12::kTracks ||
           !std::isfinite(e.ratio) || e.ratio<=0 || !std::isfinite(e.velocity) ||
           e.trimEnd<=e.trimStart)
            return makeObject({{"ok",false},{"message","Invalid native trigger"}});
        const bool ok=nativeSampler.submit(e);
        return makeObject({{"ok",ok},{"message",ok?"Queued":"Native trigger queue full"}});
    }
    juce::var nativeStop(const juce::var& payload)
    {
        p12::Event e;
        e.type=boolProperty(payload,"all",false)?p12::Event::StopAll:p12::Event::StopPad;
        e.pad=intProperty(payload,"pad",-1);
        e.delayFrames=juce::jlimit(0,192000,intProperty(payload,"delayFrames",0));
        e.targetEpochMs=static_cast<juce::int64>(static_cast<double>(property(payload,"targetEpochMs")));
        e.submittedEpochMs=static_cast<juce::int64>(static_cast<double>(property(payload,"submittedEpochMs")));
        const bool ok=nativeSampler.submit(e);
        return makeObject({{"ok",ok}});
    }
    static void captureMeter(std::atomic<float>& target,float value) noexcept {
        auto prior=target.load(std::memory_order_relaxed);
        while(value>prior&&!target.compare_exchange_weak(prior,value,std::memory_order_relaxed)){}
    }
    juce::var nativeMeters() {
        juce::Array<juce::var> pads,tracks;
        for(auto& p:meterPadPeaks)pads.add(p.exchange(0,std::memory_order_relaxed));
        for(auto& t:meterTrackPeaks)tracks.add(t.exchange(0,std::memory_order_relaxed));
        auto result=makeObject({{"ok",true},{"nativeEngine",nativeMode.load()},
            {"padPeaks",juce::var(pads)},{"trackPeaks",juce::var(tracks)},
            {"outputPeak",meterOutputPeak.exchange(0,std::memory_order_relaxed)}});
        spectrumLease.store(static_cast<int>(nativePlayhead.sampleRate.load())*2,std::memory_order_relaxed);
        if((++spectrumPoll&1)==0){
            auto bins=[&](const p12::SpectrumTap& tap){juce::Array<juce::var> values;for(int value:tap.read(nativePlayhead.sampleRate.load()))values.add(value);return juce::var(values);};
            juce::Array<juce::var> padBins,trackBins;
            for(const auto& tap:padSpectra)padBins.add(bins(tap));
            for(const auto& tap:trackSpectra)trackBins.add(bins(tap));
            result.getDynamicObject()->setProperty("padSpectra",juce::var(padBins));
            result.getDynamicObject()->setProperty("trackSpectra",juce::var(trackBins));
            result.getDynamicObject()->setProperty("masterSpectrum",bins(masterSpectrum));
        }
        return result;
    }
    juce::var nativeStatus() const
    {
        auto* obj=new juce::DynamicObject();
        obj->setProperty("ok",true);
        obj->setProperty("nativeEngine",nativeMode.load());
        obj->setProperty("sampleRate",currentSampleRate);
        obj->setProperty("blockSize",currentBlockSize);
        obj->setProperty("bpm",nativePlayhead.bpm.load());
        obj->setProperty("sequencePosition",nativeSequenceClock.position.load());
        obj->setProperty("sequencePlanId",static_cast<double>(nativeSequenceClock.planId.load()));
        obj->setProperty("sequenceLoops",static_cast<double>(nativeSequenceClock.loops.load()));
        obj->setProperty("sequencePlaying",nativeSequenceClock.playing.load());
        obj->setProperty("sessionId",nativeSessionId);
        obj->setProperty("activeVoices",nativeVoiceCount.load(std::memory_order_relaxed));
        obj->setProperty("rejectedVoices",static_cast<juce::int64>(nativeSampler.rejectedVoices()));
        obj->setProperty("liveNativeEvents",static_cast<juce::int64>(nativeSampler.liveEvents()));
        obj->setProperty("liveNativeWorstQueueMs",static_cast<juce::int64>(nativeSampler.liveWorstQueueMs()));
        obj->setProperty("lateNativeEvents",static_cast<juce::int64>(nativeSampler.lateEvents()));
        obj->setProperty("lateNativeStarts",static_cast<juce::int64>(nativeSampler.lateStarts()));
        obj->setProperty("lateNativeStops",static_cast<juce::int64>(nativeSampler.lateStops()));
        obj->setProperty("sampleBytes",static_cast<juce::int64>(nativeSampler.sampleMemory()));
        obj->setProperty("spCharacterPlayback",true);
        obj->setProperty("spCharacterClockHz",26040);
        obj->setProperty("renderedBlocks",static_cast<juce::int64>(nativeRenderedBlocks.load()));
        obj->setProperty("processedInserts",static_cast<juce::int64>(nativeProcessedInserts.load()));
        obj->setProperty("instrumentBlocks",static_cast<juce::int64>(nativeInstrumentBlocks.load()));
        obj->setProperty("instrumentMidiBlocks",static_cast<juce::int64>(nativeInstrumentMidiBlocks.load()));
        obj->setProperty("instrumentAudibleBlocks",static_cast<juce::int64>(nativeInstrumentAudibleBlocks.load()));
        obj->setProperty("padInsertBlocks",static_cast<juce::int64>(nativeInsertByKind[0].load()));
        obj->setProperty("trackInsertBlocks",static_cast<juce::int64>(nativeInsertByKind[1].load()));
        obj->setProperty("masterInsertBlocks",static_cast<juce::int64>(nativeInsertByKind[2].load()));
        juce::Array<juce::var> trackFxBlocks;
        for (const auto& counter:nativeTrackFxBlocks)
            trackFxBlocks.add(static_cast<juce::int64>(counter.load()));
        obj->setProperty("trackFxBlocks",juce::var(trackFxBlocks));
        obj->setProperty("inputPeak",nativeInputPeak.load());
        obj->setProperty("outputPeak",nativeOutputPeak.load());
        obj->setProperty("slowCallbacks",static_cast<juce::int64>(nativeSlowCallbacks.load()));
        obj->setProperty("audioLockMisses",static_cast<juce::int64>(nativeLockMisses.load()));
        obj->setProperty("dryFallbackBlocks",static_cast<juce::int64>(nativeDryFallbacks.load()));
        obj->setProperty("worstCallbackMs",nativeWorstCallbackMs.load());
        obj->setProperty("worstOverrunMs",nativeWorstOverrunMs.load());
        obj->setProperty("callbackGapPeakMs",nativeCallbackGapPeakMs.load());
        obj->setProperty("mixerPeakMs",nativeMixerPeakMs.load());
        obj->setProperty("masterVstPeakMs",nativeMasterVstPeakMs.load());
        obj->setProperty("trackVstPeakMs",nativeTrackVstPeakMs.load());
        obj->setProperty("padVstPeakMs",nativePadVstPeakMs.load());
        obj->setProperty("otherVstPeakMs",nativeInsertVstPeakMs.load());
        obj->setProperty("nearDeadlineCallbacks",static_cast<juce::int64>(nativeNearDeadlineCallbacks.load()));
        obj->setProperty("lateCallbackStarts",static_cast<juce::int64>(nativeLateCallbackStarts.load()));
        obj->setProperty("vstCostlyBlocks",static_cast<juce::int64>(nativeVstCostlyBlocks.load()));
        obj->setProperty("costlyKind",nativeCostlyKind.load(std::memory_order_relaxed));
        obj->setProperty("costlyIndex",nativeCostlyIndex.load(std::memory_order_relaxed));
        obj->setProperty("costlySlot",nativeCostlySlot.load(std::memory_order_relaxed));
        juce::Array<juce::var> pads,tracks;
        pads.ensureStorageAllocated(p12::kPads); tracks.ensureStorageAllocated(p12::kTracks);
        for(const auto& p:nativePadPeaks) pads.add(p.load(std::memory_order_relaxed));
        for(const auto& t:nativeTrackPeaks) tracks.add(t.load(std::memory_order_relaxed));
        obj->setProperty("padPeaks",juce::var(pads));
        obj->setProperty("trackPeaks",juce::var(tracks));
        return juce::var(obj);
    }

    juce::var statusObject (bool ok, const juce::String& message) const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("ok", ok);
        obj->setProperty ("message", message);
        obj->setProperty ("hostVersion", "0.5.4-native-live-timing-resources");
        obj->setProperty ("juceVersion", juce::SystemStats::getJUCEVersion());
        obj->setProperty ("audioReady", audioError.isEmpty());
        obj->setProperty ("audioError", audioError);
        obj->setProperty ("sampleRate", currentSampleRate);
        obj->setProperty ("blockSize", currentBlockSize);
        juce::Array<juce::var> supportedBuffers;
        if (auto* audioDevice = deviceManager.getCurrentAudioDevice())
            for (const int frames : audioDevice->getAvailableBufferSizes())
                supportedBuffers.add (frames);
        obj->setProperty ("availableBufferSizes", juce::var (supportedBuffers));
        obj->setProperty ("instances", static_cast<int> (instances.size()));
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            obj->setProperty ("audioDevice", device->getName());
            obj->setProperty ("audioDeviceType", device->getTypeName());
        }
        else
        {
            obj->setProperty ("audioDevice", "");
            obj->setProperty ("audioDeviceType", "");
        }
        obj->setProperty ("processedBlocks", static_cast<juce::int64> (processedBlocks.load (std::memory_order_relaxed)));
        obj->setProperty ("midiMessages", static_cast<juce::int64> (midiMessages.load (std::memory_order_relaxed)));
        obj->setProperty ("nonZeroPluginBlocks", static_cast<juce::int64> (nonZeroPluginBlocks.load (std::memory_order_relaxed)));
        obj->setProperty ("oversizePluginCallbacks", static_cast<juce::int64> (nativeOversizePluginCallbacks.load (std::memory_order_relaxed)));
        obj->setProperty ("vst3", true);
       #if P12_ENABLE_VST2
        obj->setProperty ("vst2", true);
       #else
        obj->setProperty ("vst2", false);
       #endif
        return juce::var (obj);
    }

    juce::AudioPluginFormat* findFormat (const juce::String& requested)
    {
        for (int i = 0; i < formatManager.getNumFormats(); ++i)
        {
            auto* format = formatManager.getFormat (i);
            if (format == nullptr)
                continue;

            const auto name = format->getName();
            if (requested.equalsIgnoreCase ("VST3") && name.containsIgnoreCase ("VST3"))
                return format;
            if (requested.equalsIgnoreCase ("VST2") && name.equalsIgnoreCase ("VST"))
                return format;
        }
        return nullptr;
    }

    juce::var loadStandalone (const juce::var& payload, const juce::String& format)
    {
        const auto key = hostKeyFor (payload);
        const int channel = juce::jlimit (1, 16, intProperty (payload, "channel", 1));
        return loadPlugin (key, payload, format, false, channel);
    }

    juce::var unloadStandalone (const juce::var& payload)
    {
        const auto key = hostKeyFor (payload);
        removeInstance (key);
        return makeObject ({ { "ok", true }, { "message", "Standalone VST slot unloaded." }, { "key", key } });
    }

    // Renderer-owned Web Audio stream: process actual stereo PCM through the
    // requested mixer insert chain. Native output callback must NOT run these
    // effects again on a separate silent buffer.
    juce::var processPCM (const juce::var& payload)
    {
        const auto target = stringProperty (payload, "target");
        const auto id = stringProperty (payload, "id");
        const int frames = intProperty (payload, "frames", 0);
        const double rate = static_cast<double> (intProperty (payload, "sampleRate", 48000));
        if (frames < 1 || frames > 8192 || rate < 8000 || rate > 192000)
            return makeObject ({ { "ok", false }, { "message", "Invalid PCM frames or sample rate." } });
        // The Electron renderer uses browser btoa(): STANDARD RFC 4648 Base64.
        // MemoryBlock::fromBase64Encoding uses JUCE's proprietary NNN.<data>
        // alphabet and always rejects browser-generated Base64 PCM.
        juce::MemoryOutputStream decodedPCM;
        if (! juce::Base64::convertFromBase64 (decodedPCM, stringProperty (payload, "pcm")) ||
            decodedPCM.getDataSize() != static_cast<size_t> (frames * 2 * sizeof (float)))
            return makeObject ({ { "ok", false }, { "message", "Invalid stereo PCM payload (expected standard Base64 / Float32LE)." } });
        const auto& bytes = decodedPCM.getMemoryBlock();

        juce::AudioBuffer<float> buffer (2, frames);
        const auto* src = static_cast<const float*> (bytes.getData());
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < frames; ++n)
            {
                const float v = src[ch * frames + n];
                buffer.setSample (ch, n, std::isfinite (v) ? juce::jlimit (-16.0f, 16.0f, v) : 0.0f);
            }
        const float before = juce::jmax (buffer.getMagnitude (0, 0, frames), buffer.getMagnitude (1, 0, frames));
        int active = 0;
        {
            const juce::ScopedLock lock (instanceLock);
            for (int slot = 1; slot <= 4; ++slot)
            {
                const auto key = "mixer:" + target + ":" + id + ":" + juce::String (slot);
                const auto it = instances.find (key);
                if (it == instances.end()) continue;
                auto& inst = *it->second;
                if (! inst.enabled || ! inst.mixerInsert || inst.processor == nullptr || inst.processor->isSuspended()) continue;
                // Browser and native device sample rates can be different.
                // Effect plugins must be prepared for the browser bus rate.
                if (inst.pcmSampleRate != rate || inst.pcmBlockSize != frames)
                {
                    inst.processor->releaseResources();
                    inst.processor->setRateAndBufferSizeDetails (rate, frames);
                    inst.processor->prepareToPlay (rate, frames);
                    inst.pcmSampleRate = rate;
                    inst.pcmBlockSize = frames;
                }
                const int numIn = inst.processor->getTotalNumInputChannels();
                const int numOut = inst.processor->getTotalNumOutputChannels();
                // Effects such as Ozone have real PCM input. Do not route
                // zero-input instrument plugins as effects on the master.
                if (numOut < 1 || (numIn < 1 && !inst.isInstrument)) continue;
                const int channels = juce::jmax (2, juce::jmax (numIn, numOut));
                juce::AudioBuffer<float> pluginBuffer (channels, frames);
                pluginBuffer.clear();
                for (int ch = 0; ch < juce::jmin (numIn, channels); ++ch)
                    pluginBuffer.copyFrom (ch, 0, buffer, juce::jmin (ch, 1), 0, frames);
                inst.processor->processBlock (pluginBuffer, inst.pendingMidi);
                inst.pendingMidi.clear();
                for (int ch = 0; ch < 2; ++ch)
                    buffer.copyFrom (ch, 0, pluginBuffer, juce::jmin (ch, numOut - 1), 0, frames);
                ++active;
            }
        }
        const float after = juce::jmax (buffer.getMagnitude (0, 0, frames), buffer.getMagnitude (1, 0, frames));
        juce::MemoryBlock encoded (static_cast<size_t> (frames * 2 * sizeof (float)), true);
        auto* dst = static_cast<float*> (encoded.getData());
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < frames; ++n)
                dst[ch * frames + n] = buffer.getSample (ch, n);
        // Match the renderer's atob() input format: standard Base64, not
        // MemoryBlock's legacy JUCE-specific storage encoding.
        return makeObject ({ { "ok", true }, { "pcm", juce::Base64::toBase64 (encoded.getData(), encoded.getSize()) },
                             { "frames", frames }, { "processedPlugins", active },
                             { "inputPeak", before }, { "outputPeak", after } });
    }

    juce::var setMixerInsert (const juce::var& payload)
    {
        const auto key = mixerKeyFor (payload);
        const bool enabled = boolProperty (payload, "enabled", false);
        const auto path = stringProperty (payload, "path");

        // Power buttons change bypass state; they MUST NOT destroy an expensive
        // commercial plug-in instance, editor window, licensing state or DSP buffers.
        // Empty path explicitly means remove/replace the plug-in.
        if (path.isEmpty())
        {
            removeInstance (key);
            return makeObject ({ { "ok", true }, { "message", "Mixer VST insert removed." }, { "key", key } });
        }
        {
            const juce::ScopedLock lock (instanceLock);
            const auto it = instances.find (key);
            if (it != instances.end() && it->second->path.equalsIgnoreCase (path))
            {
                it->second->enabled = enabled;
                return makeObject ({ { "ok", true }, { "message", enabled ? "Existing VST resumed without reload." : "Existing VST bypassed without unload." },
                                     { "key", key }, { "reused", true }, { "enabled", enabled } });
            }
        }
        // Loading/being available is independent of bypass.  The user may
        // select a VST3, open its editor and restore its state while its
        // insert power is OFF, then enable it without a costly reload.
        // Do not confuse the LEGACY VST comparison switch with hosting support.
        auto format = stringProperty (payload, "format").toUpperCase();
        if (format.isEmpty())
            format = path.endsWithIgnoreCase (".vst3") ? "VST3" : "VST2";

        auto reply = loadPlugin (key, payload, format, true, kDefaultChannel);
        if (boolProperty(reply, "ok", false) && ! enabled)
        {
            const juce::ScopedLock lock (instanceLock);
            const auto it = instances.find (key);
            if (it != instances.end()) it->second->enabled = false;
        }
        return reply;
    }

    juce::var loadPlugin (const juce::String& key,
                          const juce::var& payload,
                          const juce::String& formatName,
                          bool mixerInsert,
                          int midiChannel)
    {
        const auto path = stringProperty (payload, "path");
        std::cerr << "[P12] LOAD BEGIN format=" << formatName << " key=" << key << " path=" << path << std::endl;
        if (path.isEmpty())
            return makeObject ({ { "ok", false }, { "message", "Plug-in path is empty." } });

        auto* format = findFormat (formatName);
        if (format == nullptr)
        {
            if (formatName.equalsIgnoreCase ("VST2"))
                return makeObject ({ { "ok", false }, { "message", "This native host was built without VST2 support. Rebuild with P12_ENABLE_VST2=ON and a legally obtained VST2 SDK." } });
            return makeObject ({ { "ok", false }, { "message", "Requested plug-in format is not available in this host: " + formatName } });
        }

        PluginResourceScope resources(path);
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile (descriptions, path);
        if (descriptions.isEmpty())
        {
            const juce::File bundle (path);
            const auto diagnostic = bundle.exists() ?
                "Bundle exists but JUCE could not discover a loadable class (check a matching host/plugin CPU architecture, dependencies and installation)." :
                "Bundle path is missing; rescan the installed VST3 locations.";
            std::cerr << "[P12] VST DISCOVERY FAILED path=" << path
                      << " format=" << formatName << " " << diagnostic << std::endl;
            return makeObject ({ { "ok", false }, { "message", "No loadable " + formatName +
                " plug-in type was found in: " + path + ". " + diagnostic } });
        }

        // A single .vst3 bundle may advertise multiple component classes. The
        // first class is not guaranteed to be instantiable on this machine.
        // Prefer a matching name, then try the remaining classes, reporting each
        // failure instead of turning one failed class into a failed bundle.
        const auto requestedName = stringProperty (payload, "name").trim();
        const auto bundleName = juce::File (path).getFileNameWithoutExtension();
        juce::Array<int> order;
        for (int i = 0; i < descriptions.size(); ++i)
            if ((requestedName.isNotEmpty() && descriptions[i]->name.equalsIgnoreCase (requestedName)) ||
                descriptions[i]->name.equalsIgnoreCase (bundleName))
                order.add (i);
        for (int i = 0; i < descriptions.size(); ++i)
            if (! order.contains (i)) order.add (i);

        juce::StringArray errors;
        std::unique_ptr<juce::AudioPluginInstance> processor;
        int chosenType = -1;
        for (auto index : order)
        {
            juce::String error;
            auto candidate = formatManager.createPluginInstance (*descriptions[index], currentSampleRate, currentBlockSize, error);
            if (candidate != nullptr)
            {
                processor = std::move (candidate);
                chosenType = index;
                break;
            }
            const auto detail = descriptions[index]->name + ": " +
                (error.isNotEmpty() ? error : "cannot instantiate (check plug-in dependencies, activation and architecture)");
            errors.add (detail);
            std::cerr << "[P12] VST LOAD CLASS FAILED " << detail << std::endl;
        }
        if (processor == nullptr)
        {
            const auto detail = errors.joinIntoString (" | ");
            std::cerr << "[P12] VST INSTANTIATION FAILED path=" << path
                      << " format=" << formatName << " classes=" << descriptions.size()
                      << " detail=" << detail << std::endl;
            return makeObject ({ { "ok", false }, { "message", "VST3/VST load failed for all " + juce::String (descriptions.size()) +
                " plug-in types in " + path + ": " + detail +
                ". Check native plugin dependencies, license/activation, x64 component and installation integrity; then rescan or reinstall if needed." } });
        }

        auto hosted = std::make_unique<HostedPlugin>();
        hosted->key = key;
        hosted->path = path;
        hosted->format = formatName.toUpperCase();
        hosted->displayName = stringProperty (payload, "name", processor->getName());
        hosted->target = stringProperty (payload, "target");
        hosted->targetId = stringProperty (payload, "id");
        hosted->slot = juce::jmax (1, intProperty (payload, "slot", 1));
        hosted->midiChannel = juce::jlimit (1, 16, midiChannel);
        hosted->mixerInsert = mixerInsert;
        hosted->isInstrument = descriptions[chosenType]->isInstrument;
        hosted->enabled = true;
        const bool hasEditor = processor->hasEditor();
        hosted->pendingMidi.ensureSize(524288);
        hosted->processor = std::move (processor);
        prepareInstance (*hosted);
        // New instance is private: lazy DSP setup happens here, never in the
        // first live callback. No graph lock, no publication until warm-up ends.
        double warmPeakMs=0;
        p12::warmPlugin(4,[&]{hosted->scratch.clear();hosted->pendingMidi.clear();},[&]{
            const auto begin=std::chrono::steady_clock::now();
            juce::AudioBuffer<float> warmBlock(hosted->scratch.getArrayOfWritePointers(),hosted->scratch.getNumChannels(),currentBlockSize);
            hosted->processor->processBlock(warmBlock,hosted->pendingMidi);
            warmPeakMs=(std::max)(warmPeakMs,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
        });
        hosted->pendingMidi.clear();hosted->scratch.clear();
        std::cerr<<"[P12] DSP WARMUP key="<<key<<" blocks=4 peakMs="<<warmPeakMs<<std::endl;

        std::unique_ptr<HostedPlugin> oldInstance;
        {
            const juce::ScopedLock lock (instanceLock);
            auto old = instances.find (key);
            if (old != instances.end())
            {
                oldInstance = std::move (old->second);
                instances.erase (old);
            }
            instances.emplace (key, std::move (hosted));
        }
        // Release potentially slow plug-in/editor resources outside the audio lock.
        if (oldInstance != nullptr)
        {
            oldInstance->editor.reset();
            if (oldInstance->processor != nullptr)
                oldInstance->processor->releaseResources();
        }

        std::cerr << "[P12] LOAD OK format=" << formatName << " key=" << key << " name=" << descriptions[chosenType]->name << std::endl;
        return makeObject ({
            { "ok", true },
            { "message", "Loaded [" + formatName.toUpperCase() + "] " + descriptions[chosenType]->name },
            { "key", key },
            { "name", descriptions[chosenType]->name },
            { "format", formatName.toUpperCase() },
            { "path", path },
            { "isInstrument", descriptions[chosenType]->isInstrument },
            { "hasEditor", hasEditor }
        });
    }

    void prepareInstance (HostedPlugin& inst)
    {
        if (inst.processor == nullptr)
            return;

        inst.processor->releaseResources();
        // Some instruments expose their main output bus disabled until a host
        // explicitly enables it. Without this, MIDI can arrive correctly while
        // processBlock still returns silence.
        inst.processor->enableAllBuses();
        inst.processor->setPlayHead (&nativePlayhead);
        inst.processor->setNonRealtime (false);
        inst.processor->setRateAndBufferSizeDetails (currentSampleRate, currentBlockSize);
        inst.processor->prepareToPlay (currentSampleRate, currentBlockSize);
        inst.processor->suspendProcessing (false);

        const int channels = juce::jmax (2, juce::jmax (inst.processor->getTotalNumInputChannels(),
                                                        inst.processor->getTotalNumOutputChannels()));
        inst.scratch.setSize (channels, juce::jmax (currentBlockSize, 64), false, false, true);
        inst.scratch.clear();
    }

    void removeInstance (const juce::String& key)
    {
        std::unique_ptr<HostedPlugin> doomed;
        {
            const juce::ScopedLock lock (instanceLock);
            const auto it = instances.find (key);
            if (it == instances.end())
                return;
            doomed = std::move (it->second);
            instances.erase (it);
        }

        // Do not make the audio callback wait while a third-party plug-in/editor tears down.
        doomed->editor.reset();
        if (doomed->processor != nullptr)
            doomed->processor->releaseResources();
    }

    HostedPlugin* findInstanceForPayload (const juce::var& payload)
    {
        const auto target = stringProperty (payload, "target");
        const auto key = target.isNotEmpty() ? mixerKeyFor (payload) : hostKeyFor (payload);
        const auto it = instances.find (key);
        return it == instances.end() ? nullptr : it->second.get();
    }

    juce::var openEditor (const juce::var& payload)
    {
        // Commands are serialised on the JUCE message thread; creating a potentially
        // heavy editor while holding the realtime audio lock would cause a dropout.
        auto* inst = findInstanceForPayload (payload);
        if (inst == nullptr || inst->processor == nullptr)
            return makeObject ({ { "ok", false }, { "message", "The requested plug-in instance is not loaded." } });

        // Guard against stale renderer state. If P-12 asks to open plug-in B but
        // the native slot still contains plug-in A, never silently show A's GUI.
        const auto expectedPath = stringProperty (payload, "path");
        if (expectedPath.isNotEmpty())
        {
            const auto expected = juce::File (expectedPath).getFullPathName();
            const auto loaded = juce::File (inst->path).getFullPathName();
            if (! expected.equalsIgnoreCase (loaded))
                return makeObject ({ { "ok", false },
                                     { "message", "VST instance mismatch: the requested plug-in was not loaded into this slot. Keeping the previous plug-in." },
                                     { "key", inst->key }, { "loadedPath", inst->path }, { "requestedPath", expectedPath },
                                     { "loadedName", inst->displayName } });
        }

        PluginResourceScope resources(inst->path);
        const bool forceGeneric = boolProperty (payload, "generic", false) || resources.useGenericEditor();
        if(resources.useGenericEditor())std::cerr<<"[P12] EDITOR RESOURCE FALLBACK: using parameter editor; Ozone vendor ICU/platform dependencies are unavailable."<<std::endl;
        std::cerr << "[P12] EDITOR BEGIN key=" << inst->key << " generic=" << (forceGeneric ? 1 : 0) << " path=" << inst->path << std::endl;
        if (inst->editor != nullptr && inst->editorIsGeneric != forceGeneric)
            inst->editor.reset();

        if (inst->editor == nullptr)
        {
            try
            {
                inst->editor = std::make_unique<PluginEditorWindow> (*inst->processor,
                                                                      "P-12 · " + inst->displayName,
                                                                      forceGeneric);
                inst->editorIsGeneric = forceGeneric;
            }
            catch (const std::exception& e)
            {
                return makeObject ({ { "ok", false },
                                     { "message", juce::String ("Plug-in editor creation failed: ") + e.what() },
                                     { "key", inst->key }, { "path", inst->path } });
            }
            catch (...)
            {
                return makeObject ({ { "ok", false },
                                     { "message", "Plug-in editor creation failed with an unknown native exception." },
                                     { "key", inst->key }, { "path", inst->path } });
            }
        }
        else
        {
            inst->editor->setVisible (true);
            inst->editor->toFront (true);
            inst->editor->grabKeyboardFocus();
        }

        // A few commercial editors create child/authentication windows on the
        // next Windows message-loop turn. Re-focus once after that turn, but do
        // not force topmost z-order (license/login dialogs must be allowed above).
        const auto editorKey = inst->key;
        juce::Timer::callAfterDelay (250, [this, editorKey]
        {
            // The command handler and timer both run on the JUCE message
            // thread; editor visibility does not modify the DSP instance map.
            // Never hold the audio graph lock across Windows GUI operations.
            const auto it = instances.find (editorKey);
            if (it != instances.end() && it->second->editor != nullptr)
            {
                it->second->editor->setVisible (true);
                it->second->editor->toFront (true);
            }
        });

        std::cerr << "[P12] EDITOR OK key=" << inst->key << " generic=" << (forceGeneric ? 1 : 0) << " path=" << inst->path << std::endl;
        return makeObject ({ { "ok", true },
                             { "message", resources.useGenericEditor() ? "Ozone custom editor resources unavailable; parameter editor opened." : forceGeneric ? "Generic plug-in editor opened." : "Plug-in editor opened." },
                             { "key", inst->key }, { "path", inst->path }, { "name", inst->displayName },
                             { "editor", forceGeneric ? "generic" : "custom" } });
    }

    juce::var closeEditor (const juce::var& payload)
    {
        auto* inst = findInstanceForPayload (payload);
        if (inst == nullptr)
            return makeObject ({ { "ok", false }, { "message", "The requested plug-in instance is not loaded." } });

        if (inst->editor != nullptr)
            inst->editor->setVisible (false);

        return makeObject ({ { "ok", true }, { "message", "Plug-in editor hidden." }, { "key", inst->key } });
    }

    juce::MidiMessage midiMessageFromPayload (const juce::var& payload) const
    {
        const auto type = stringProperty (payload, "type").toLowerCase();
        const int channel = juce::jlimit (1, 16, intProperty (payload, "channel", 1));
        const int note = juce::jlimit (0, 127, intProperty (payload, "note", 60));
        const int velocity = juce::jlimit (0, 127, intProperty (payload, "velocity", 100));

        if (type == "noteoff" || velocity == 0)
            return juce::MidiMessage::noteOff (channel, note, static_cast<juce::uint8> (velocity));

        if (type == "noteon")
            return juce::MidiMessage::noteOn (channel, note, static_cast<juce::uint8> (velocity));

        if (type == "cc")
            return juce::MidiMessage::controllerEvent (channel,
                                                       juce::jlimit (0, 127, intProperty (payload, "controller", 0)),
                                                       velocity);

        if (type == "pitchbend")
            return juce::MidiMessage::pitchWheel (channel, juce::jlimit (0, 16383, intProperty (payload, "value", 8192)));

        if (type == "pressure")
            return juce::MidiMessage::channelPressureChange (channel, velocity);

        if (type == "polypressure")
            return juce::MidiMessage::aftertouchChange (channel, note, velocity);

        return {};
    }

    juce::var inspectInstrument (const juce::var& payload)
    {
        const juce::ScopedLock lock (instanceLock);
        auto* inst = findInstanceForPayload (payload);
        if (inst == nullptr || inst->processor == nullptr)
            return makeObject ({{"ok", false}, {"message", "No VST loaded in this slot. Choose the correct VST Target and Slot."}});
        if (!inst->enabled || inst->processor->isSuspended())
            return makeObject ({{"ok", false}, {"message", "Selected VST is disabled or suspended."}});
        if (!inst->isInstrument)
            return makeObject ({{"ok", false}, {"message", "This slot contains an effect, not a MIDI instrument."}});
        if (inst->processor->getTotalNumOutputChannels() < 1)
            return makeObject ({{"ok", false}, {"message", "This instrument has no active audio output bus."}});
        return makeObject ({{"ok", true}, {"message", "Ready: " + inst->displayName},
                            {"name", inst->displayName}, {"audioReady", audioError.isEmpty()},
                            {"outputs", inst->processor->getTotalNumOutputChannels()}});
    }

    juce::var routeMidi (const juce::var& payload)
    {
        const auto message = midiMessageFromPayload (payload);
        if (message.getRawDataSize() == 0)
            return makeObject ({ { "ok", false }, { "message", "Unsupported or malformed MIDI message." } });

        const auto target = stringProperty (payload, "target");
        const int channel = juce::jlimit (1, 16, intProperty (payload, "channel", 1));
        const int requestedHostSlot = intProperty (payload, "hostSlot", intProperty (payload, "slot", 0));
        int recipients = 0;

        const juce::ScopedLock lock (instanceLock);
        if (target.isNotEmpty())
        {
            const auto key = mixerKeyFor (payload);
            const auto it = instances.find (key);
            if (it != instances.end())
            {
                it->second->pendingMidi.addEvent (message, 0);
                ++recipients;
            }
        }
        else if (requestedHostSlot > 0)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("slot", juce::jlimit (1, 64, requestedHostSlot));
            const auto key = hostKeyFor (juce::var (obj));
            const auto it = instances.find (key);
            if (it != instances.end() && ! it->second->mixerInsert)
            {
                it->second->pendingMidi.addEvent (message, 0);
                ++recipients;
            }
        }
        else
        {
            for (auto& [key, inst] : instances)
            {
                if (! inst->mixerInsert && inst->midiChannel == channel)
                {
                    inst->pendingMidi.addEvent (message, 0);
                    ++recipients;
                }
            }
        }

        if (recipients > 0)
            midiMessages.fetch_add (recipients, std::memory_order_relaxed);
        return makeObject ({ { "ok", true }, { "recipients", recipients } });
    }

    juce::var allNotesOff()
    {
        const juce::ScopedLock lock (instanceLock);
        for (auto& [key, inst] : instances)
            for (int ch = 1; ch <= 16; ++ch)
                inst->pendingMidi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        return makeObject ({ { "ok", true }, { "message", "All Notes Off queued." } });
    }

    // Offline instrument capture. NEVER process the live instance: its voice state,
    // processBlock and native audio callback are left untouched.
    juce::var renderVSTToWav (const juce::var& payload)
    {
        const double sr = juce::jlimit (22050.0, 96000.0, currentSampleRate);
        constexpr int quantum = 256;
        const double bpm = juce::jlimit (30.0, 300.0, static_cast<double> (property (payload, "bpm").isVoid() ? juce::var (96.0) : property (payload, "bpm")));
        const double tailSeconds = juce::jlimit (0.15, 2.5, static_cast<double> (property (payload, "tailSeconds").isVoid() ? juce::var (0.75) : property (payload, "tailSeconds")));
        const auto noteValues = property (payload, "notes");
        const auto* notes = noteValues.getArray();
        if (notes == nullptr || notes->isEmpty() || notes->size() > 128)
            return makeObject ({{"ok", false}, {"message", "Choose 1–128 piano-roll notes before rendering."}});

        juce::PluginDescription desc;
        juce::MemoryBlock preset;
        {
            const juce::ScopedLock lock (instanceLock);
            auto* live = findInstanceForPayload (payload);
            if (live == nullptr || live->processor == nullptr || ! live->enabled)
                return makeObject ({{"ok", false}, {"message", "Load and enable a VST instrument in the selected slot first."}});
            live->processor->fillInPluginDescription (desc);
            if (! desc.isInstrument)
                return makeObject ({{"ok", false}, {"message", "The selected VST is an effect, not an instrument. Load a sound-generating VSTi."}});
            live->processor->getStateInformation (preset);
        }
        juce::String loadError;
        auto synth = formatManager.createPluginInstance (desc, sr, quantum, loadError);
        if (synth == nullptr)
            return makeObject ({{"ok", false}, {"message", "Could not create independent VST render instance: " + loadError}});
        synth->enableAllBuses();
        synth->setNonRealtime (true);
        synth->setRateAndBufferSizeDetails (sr, quantum);
        synth->prepareToPlay (sr, quantum);
        if (preset.getSize() > 0) synth->setStateInformation (preset.getData(), static_cast<int> (preset.getSize()));
        synth->suspendProcessing (false);

        struct Event { int frame; int note; int velocity; bool on; };
        std::vector<Event> events;
        const double stepFrames = sr * 60.0 / bpm / 4.0;
        int lastNoteFrame = 0;
        for (const auto& item : *notes)
        {
            const int note = intProperty (item, "note", -1);
            // Piano Roll stores real SP-1200 tick positions; triplets are
            // fractional 16th steps. Do not truncate the timing to integers.
            const double start = static_cast<double> (property (item,"start"));
            const double len = static_cast<double> (property (item,"length"));
            if (note < 0 || note > 127 || !std::isfinite(start) || !std::isfinite(len) ||
                start < 0 || start >= 512 || len <= 0 || start+len > 512 ||
                (start + len) * stepFrames > sr * 20.0)
            {
                synth->releaseResources();
                return makeObject ({{"ok", false}, {"message", "Invalid piano-roll note position or length."}});
            }
            const int on = juce::roundToInt (stepFrames * start);
            const int off = juce::roundToInt (stepFrames * (start + len));
            events.push_back ({on, note, juce::jlimit (1, 127, intProperty (item, "velocity", 100)), true});
            events.push_back ({off, note, 0, false});
            lastNoteFrame = juce::jmax (lastNoteFrame, off);
        }
        std::sort (events.begin(), events.end(), [](const Event& a, const Event& b) {
            if (a.frame != b.frame) return a.frame < b.frame;
            return !a.on && b.on; // release retriggered notes first
        });
        // Never silently truncate a long sequence to eight seconds. Very
        // long performances should be bounced in shorter Rossum-safe clips.
        const int frames = lastNoteFrame + juce::roundToInt (sr * tailSeconds);
        juce::AudioBuffer<float> captured (2, frames);
        captured.clear();
        const int channels = juce::jmax (2, juce::jmax (synth->getTotalNumInputChannels(), synth->getTotalNumOutputChannels()));
        juce::AudioBuffer<float> scratch (channels, quantum);
        size_t nextEvent = 0;
        for (int offset = 0; offset < frames; offset += quantum)
        {
            const int count = juce::jmin (quantum, frames - offset);
            scratch.clear();
            juce::MidiBuffer midi;
            while (nextEvent < events.size() && events[nextEvent].frame < offset + count)
            {
                const auto& e = events[nextEvent++];
                midi.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.note, static_cast<juce::uint8> (e.velocity))
                                    : juce::MidiMessage::noteOff (1, e.note),
                               juce::jmax (0, e.frame - offset));
            }
            synth->processBlock (scratch, midi);
            for (int ch = 0; ch < 2; ++ch)
                captured.copyFrom (ch, offset, scratch, ch, 0, count);
        }
        synth->releaseResources();
        if (captured.getMagnitude (0, 0, frames) < 0.00002f &&
            captured.getMagnitude (1, 0, frames) < 0.00002f)
            return makeObject ({{"ok", false}, {"message", "VST render was silent; the empty pad was not changed."}});

        juce::MemoryBlock wavBytes;
        auto* sink = new juce::MemoryOutputStream (wavBytes, false);
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatWriter> writer (static_cast<juce::AudioFormat&> (wavFormat).createWriterFor (
            sink, sr, 2, 16, {}, 0));
        if (writer == nullptr)
        {
            delete sink;
            return makeObject ({{"ok", false}, {"message", "Unable to encode VST sample as WAV."}});
        }
        const bool wrote = writer->writeFromAudioSampleBuffer (captured, 0, frames);
        writer.reset(); // finalizes WAV header and closes its owned memory stream
        if (! wrote || wavBytes.getSize() < 48)
            return makeObject ({{"ok", false}, {"message", "WAV write failed; no pad was changed."}});
        return makeObject ({{"ok", true}, {"wavBase64", juce::Base64::toBase64 (wavBytes.getData(), wavBytes.getSize())},
                            {"frames", frames}, {"sampleRate", sr}, {"duration", frames / sr},
                            {"message", "VST instrument rendered to a real WAV."}});
    }

    juce::var getPluginState (const juce::var& payload)
    {
        const juce::ScopedLock lock (instanceLock);
        auto* inst = findInstanceForPayload (payload);
        if (inst == nullptr || inst->processor == nullptr)
            return makeObject ({ { "ok", false }, { "message", "The requested plug-in instance is not loaded." } });

        juce::MemoryBlock state;
        inst->processor->getStateInformation (state);
        return makeObject ({ { "ok", true }, { "key", inst->key }, { "stateBase64", state.toBase64Encoding() } });
    }

    juce::var setPluginState (const juce::var& payload)
    {
        const juce::ScopedLock lock (instanceLock);
        auto* inst = findInstanceForPayload (payload);
        if (inst == nullptr || inst->processor == nullptr)
            return makeObject ({ { "ok", false }, { "message", "The requested plug-in instance is not loaded." } });

        juce::MemoryBlock state;
        if (! state.fromBase64Encoding (stringProperty (payload, "stateBase64")))
            return makeObject ({ { "ok", false }, { "message", "Invalid plug-in stateBase64." } });

        inst->processor->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        return makeObject ({ { "ok", true }, { "message", "Plug-in state restored." } });
    }

    juce::var startTestTone()
    {
        if (deviceManager.getCurrentAudioDevice() == nullptr)
            return makeObject ({ { "ok", false }, { "message", "No native audio output device is open." } });

        testTonePhase = 0.0;
        testToneSamplesRemaining.store (static_cast<int> (currentSampleRate * 0.75), std::memory_order_relaxed);
        return makeObject ({ { "ok", true }, { "message", "Native 440 Hz test tone started." } });
    }

    juce::var listAudioOutputs()
    {
        auto* out = new juce::DynamicObject();
        out->setProperty ("ok", true);
        juce::Array<juce::var> names;
        // Enumerate every available driver family. Previously we scanned only
        // the active type, hiding installed Focusrite ASIO devices.
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            for (const auto& name : type->getDeviceNames (false))
            {
                auto* item = new juce::DynamicObject();
                item->setProperty ("name", name);
                item->setProperty ("type", type->getTypeName());
                names.add (juce::var (item));
            }
        }
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            out->setProperty ("current", device->getName());
            out->setProperty ("deviceType", device->getTypeName());
        }
        else
        {
            out->setProperty ("current", "");
            out->setProperty ("deviceType", "");
        }
        out->setProperty ("outputs", juce::var (names));
        return juce::var (out);
    }

    // Change only on the message thread while transport is STOPPED. This
    // reopens the device, never allocating inside its real-time callback.
    juce::var nativeBufferSize (const juce::var& payload)
    {
        const int frames = intProperty(payload, "frames", 0);
        auto* selectedDevice = deviceManager.getCurrentAudioDevice();
        if (selectedDevice == nullptr)
            return makeObject({{ "ok", false }, { "message", "No native output device is available." }});
        const auto sizes = selectedDevice->getAvailableBufferSizes();
        if (frames <= 0 || ! sizes.contains (frames))
            return makeObject({{ "ok", false }, { "blockSize", currentBlockSize },
                { "message", "The selected audio device does not offer " + juce::String (frames) + " frames. Keep its current setting or choose a listed supported size." }});
        if (nativePlayhead.playing.load(std::memory_order_relaxed))
            return makeObject({{ "ok", false }, { "message", "Stop the sequencer before changing the native buffer." }});
        const auto before = deviceManager.getAudioDeviceSetup();
        if (before.bufferSize == frames)
            return makeObject({{ "ok", true }, { "blockSize", currentBlockSize }, { "message", "Buffer unchanged." }});
        auto setup = before;
        setup.bufferSize = frames;
        const auto error = deviceManager.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
            return makeObject({{ "ok", false }, { "message", "Native device rejected requested buffer: " + error },
                               { "blockSize", currentBlockSize }});
        // WASAPI shared mode and some device drivers silently retain their own
        // callback size. Never claim the requested size was applied when it was not.
        const int actual = currentBlockSize;
        if (actual != frames)
            return makeObject({{ "ok", false }, { "blockSize", actual },
                { "message", "Audio driver retained " + juce::String(actual) +
                  " frames (requested " + juce::String(frames) +
                  "). The audio driver has retained its own setting." }});
        return makeObject({{ "ok", true }, { "blockSize", actual },
                           { "message", "Native buffer configured." }});
    }

    juce::var setAudioOutput (const juce::var& payload)
    {
        const auto requested = stringProperty (payload, "name");
        const auto wantedType = stringProperty (payload, "type");
        if (requested.isEmpty())
            return makeObject ({ { "ok", false }, { "message", "Audio output name is empty." } });

        const auto oldType = deviceManager.getCurrentAudioDeviceType();
        const auto oldSetup = deviceManager.getAudioDeviceSetup();
        if (wantedType.isNotEmpty() && wantedType != oldType)
        {
            bool found = false;
            for (const auto* type : deviceManager.getAvailableDeviceTypes())
                if (type->getTypeName() == wantedType) found = true;
            if (! found)
                return makeObject ({ { "ok", false }, { "message", "Audio driver type unavailable: " + wantedType } });
            deviceManager.setCurrentAudioDeviceType (wantedType, true);
        }
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = requested;
        const auto error = deviceManager.setAudioDeviceSetup (setup, true);
        if (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr ||
            deviceManager.getCurrentAudioDevice()->getName() != requested)
        {
            // Switching to a missing/unavailable ASIO driver must not leave
            // the previously working WASAPI output disconnected.
            deviceManager.setCurrentAudioDeviceType (oldType, true);
            deviceManager.setAudioDeviceSetup (oldSetup, true);
            return makeObject ({ { "ok", false }, { "message", "Could not open output " + requested + ": " +
                (error.isNotEmpty() ? error : "driver did not select the requested device") } });
        }
        audioError = {};
        return makeObject ({ { "ok", true }, { "message", "Native audio output: " + wantedType + " / " + requested },
                             { "audioDevice", requested }, { "deviceType", deviceManager.getCurrentAudioDeviceType() } });
    }

    // Declared before plugin map so transport outlives hosted processors.
    P12TransportClock nativePlayhead;
    std::array<std::array<juce::String,4>,p12::kPads> nativePadKeys;
    std::array<std::array<juce::String,4>,p12::kTracks> nativeTrackKeys;
    std::array<juce::String,4> nativeMasterKeys;
    p12::Sampler nativeSampler;
    std::atomic<bool> nativeMode {false};
    juce::String nativeSessionId {juce::Uuid().toString()};
    std::atomic<int> nativeVoiceCount {0};
    std::atomic<float> meterOutputPeak{0};
    std::array<std::atomic<float>,p12::kPads> meterPadPeaks{};
    std::array<std::atomic<float>,p12::kTracks> meterTrackPeaks{};
    std::atomic<float> nativeInputPeak {0}, nativeOutputPeak {0};
    std::array<std::atomic<float>,p12::kPads> nativePadPeaks {};
    std::array<std::atomic<float>,p12::kTracks> nativeTrackPeaks {};
    std::atomic<juce::int64> nativeSlowCallbacks {0};
    std::atomic<juce::int64> nativeNearDeadlineCallbacks {0};
    std::atomic<juce::int64> nativeLateCallbackStarts {0};
    std::atomic<juce::int64> nativeLockMisses {0};
    std::atomic<juce::int64> nativeDryFallbacks {0};
    std::atomic<double> nativeWorstCallbackMs {0};
    std::atomic<double> nativeWorstOverrunMs {0};
    std::atomic<double> nativeCallbackGapPeakMs {0};
    std::atomic<double> nativeMixerPeakMs {0};
    std::atomic<double> nativeMasterVstPeakMs {0}, nativeTrackVstPeakMs {0}, nativePadVstPeakMs {0};
    std::atomic<double> nativeInsertVstPeakMs {0};
    std::atomic<juce::int64> nativeVstCostlyBlocks {0};
    std::atomic<int> nativeCostlyKind {-1}, nativeCostlyIndex {-1}, nativeCostlySlot {-1};
    std::chrono::steady_clock::time_point lastCallbackStart {};
    int lastCallbackSamples = 0;
    std::atomic<juce::int64> nativeRenderedBlocks {0}, nativeProcessedInserts {0};
    std::atomic<juce::int64> nativeInstrumentBlocks {0}, nativeInstrumentMidiBlocks {0}, nativeInstrumentAudibleBlocks {0};
    std::array<std::atomic<uint64_t>,3> nativeInsertByKind {};
    std::array<std::atomic<uint64_t>,p12::kTracks> nativeTrackFxBlocks {};
    juce::AudioBuffer<float> nativePadScratch, nativeTrackScratch, nativeMasterScratch;
    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::CriticalSection instanceLock;
    std::map<juce::String, std::unique_ptr<HostedPlugin>> instances;

    std::atomic<juce::int64> processedBlocks { 0 };
    std::atomic<juce::int64> midiMessages { 0 };
    std::atomic<juce::int64> nonZeroPluginBlocks { 0 };
    std::atomic<juce::int64> nativeOversizePluginCallbacks { 0 };
    std::atomic<int> testToneSamplesRemaining { 0 };
    double testTonePhase = 0.0;

    double currentSampleRate = 48000.0;
    int currentBlockSize = 512;
    juce::String audioError;
    bool audioCallbackAttached = false;
};

class ProtocolServer final : private juce::Thread
{
public:
    explicit ProtocolServer (HostEngine& e)
        : juce::Thread ("P12 JSON-lines protocol"), engine (e)
    {
    }

    ~ProtocolServer() override
    {
        signalThreadShouldExit();
        stopThread (1500);
    }

    void start()
    {
        startThread();
    }

private:
    void run() override
    {
        std::string line;
        while (! threadShouldExit() && std::getline (std::cin, line))
        {
            const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (line.c_str()));
            if (! parsed.isObject())
            {
                writeResponse (makeObject ({ { "ok", false }, { "message", "Invalid JSON command." } }));
                continue;
            }

            const auto command = stringProperty (parsed, "command");
            const auto payload = property (parsed, "payload");
            const auto requestId = stringProperty (parsed, "id");
            const bool wantsReply = requestId.isNotEmpty();

            // PCM is time-critical. Process on the IPC worker, NOT the JUCE GUI
            // message thread. Opening an iZotope/Ozone editor can take seconds;
            // it must not hold up the entire 2048-frame audio return queue.
            // Control/UI tasks remain on JUCE's message thread, posted without
            // waiting so this worker can keep servicing PCM during editor load.
            auto execute = [this, command, payload, requestId, wantsReply]
            {
                juce::var result;
                try
                {
                    result = engine.handleCommand (command, payload);
                }
                catch (const std::exception& e)
                {
                    result = makeObject ({ { "ok", false }, { "message", juce::String ("Native host exception: ") + e.what() } });
                }
                catch (...)
                {
                    result = makeObject ({ { "ok", false }, { "message", "Unknown native host exception." } });
                }
                if (wantsReply)
                    writeResponseWithId (requestId, result);
            };
            if (command == "processPCM" || command == "nativeSampleLoad" ||
                command == "nativeSampleClear" ||
                command == "nativeTrigger" || command == "nativeStop" ||
                command == "nativeStatus" || command == "nativeMeters" || command == "nativeTransport")
                execute();
            else
                juce::MessageManager::callAsync (execute);
        }

        juce::MessageManager::callAsync ([]
        {
            juce::JUCEApplicationBase::quit();
        });
    }

    void writeResponseWithId (const juce::String& requestId, const juce::var& result)
    {
        auto* out = new juce::DynamicObject();
        out->setProperty ("replyTo", requestId);
        if (auto* resultObj = result.getDynamicObject())
        {
            const auto& props = resultObj->getProperties();
            for (int i = 0; i < props.size(); ++i)
                out->setProperty (props.getName (i), props.getValueAt (i));
        }
        else
        {
            out->setProperty ("ok", true);
            out->setProperty ("result", result);
        }
        writeResponse (juce::var (out));
    }

    void writeResponse (const juce::var& response)
    {
        const std::lock_guard<std::mutex> lock (outputMutex);
        std::cout << juce::JSON::toString (response, true).toStdString() << std::endl;
        std::cout.flush();
    }

    HostEngine& engine;
    std::mutex outputMutex;
};

class P12VstHostApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "P-12 VST Host"; }
    const juce::String getApplicationVersion() override    { return "0.4.0"; }
    bool moreThanOneInstanceAllowed() override              { return false; }

    void initialise (const juce::String&) override
    {
       #if JUCE_WINDOWS
        // Commercial VST editors frequently use OLE/COM for drag-and-drop,
        // embedded browsers, licensing/login helpers and shell integration.
        // Keep the JUCE message thread in a normal STA/OLE environment.
        const auto hr = ::OleInitialize (nullptr);
        oleInitialised = (hr == S_OK || hr == S_FALSE);
       #endif

        initialStatus = engine.initialise();
        protocol = std::make_unique<ProtocolServer> (engine);
        protocol->start();
    }

    void shutdown() override
    {
        protocol.reset();
        engine.shutdown();
       #if JUCE_WINDOWS
        if (oleInitialised)
            ::OleUninitialize();
       #endif
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    HostEngine engine;
    juce::var initialStatus;
    std::unique_ptr<ProtocolServer> protocol;
   #if JUCE_WINDOWS
    bool oleInitialised = false;
   #endif
};
}

START_JUCE_APPLICATION (P12VstHostApplication)
