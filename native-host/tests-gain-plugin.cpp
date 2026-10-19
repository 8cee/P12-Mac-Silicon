#include <juce_audio_utils/juce_audio_utils.h>
#include <cstring>
// Local fixture only. Never copied into the user's plugin folders or app package.
class TestGain final : public juce::AudioProcessor {
public:
    TestGain() : AudioProcessor(BusesProperties().withInput("In",juce::AudioChannelSet::stereo(),true).withOutput("Out",juce::AudioChannelSet::stereo(),true)) {}
    const juce::String getName() const override { return "P12 Test Gain"; }
    void prepareToPlay(double,int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&) override { b.applyGain(gain); }
    bool isBusesLayoutSupported(const BusesLayout& b) const override { return b.getMainInputChannelSet()==juce::AudioChannelSet::stereo() && b.getMainOutputChannelSet()==juce::AudioChannelSet::stereo(); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int,const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
    void getStateInformation(juce::MemoryBlock& b) override { b.replaceWith(&gain,sizeof(gain)); }
    void setStateInformation(const void* data,int size) override { if(size==sizeof(gain))std::memcpy(&gain,data,sizeof(gain)); }
private:
    float gain=0.5f;
};
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestGain; }
