#include "PluginProcessor.h"

namespace
{
    struct Preset { const char* name; float gain, bass, mid, treble, volume, gate, delay, reverb; };

    // Valores de 0 a 100. O primeiro preset e uma APROXIMACAO baseada em endorsements,
    // nao o setup confirmado da gravacao.
    const Preset presets[] =
    {
        { "Travelers of Time (aproximacao)", 80, 52, 42, 64, 55, 35, 12, 14 },
        { "Limpo",                           12, 50, 50, 56, 55,  0, 10, 22 },
        { "Crunch",                          45, 55, 55, 58, 55, 10,  8, 16 },
        { "Solo",                            88, 48, 60, 60, 52, 30, 30, 24 }
    };
    constexpr int numPresets = (int) (sizeof (presets) / sizeof (presets[0]));
    const char* const paramIds[] = { "gain", "bass", "mid", "treble", "volume", "gate", "delay", "reverb" };
}

AmpProcessor::AmpProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout AmpProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const char* names[] = { "Ganho", "Graves", "Medios", "Agudos", "Volume", "Gate", "Delay", "Reverb" };
    const auto& d = presets[0];
    const float defaults[] = { d.gain, d.bass, d.mid, d.treble, d.volume, d.gate, d.delay, d.reverb };
    for (int i = 0; i < 8; ++i)
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { paramIds[i], 1 }, names[i],
            juce::NormalisableRange<float> (0.f, 100.f, 0.1f), defaults[i]));
    return layout;
}

bool AmpProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

void AmpProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRateHz = sampleRate;
    const auto numCh = (juce::uint32) juce::jmax (1, getTotalNumOutputChannels());
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, numCh };

    for (auto* f : { &tightHp, &bassF, &midF, &trebF, &cabHp, &cabPk, &cabLp })
        f->prepare (spec);

    *tightHp.state = *Coeffs::makeHighPass (sampleRate, 110.f, 0.7f);
    *cabHp.state   = *Coeffs::makeHighPass (sampleRate, 85.f, 0.7f);
    *cabPk.state   = *Coeffs::makePeakFilter (sampleRate, 2600.f, 1.1f, juce::Decibels::decibelsToGain (4.f));
    *cabLp.state   = *Coeffs::makeLowPass (sampleRate, 5000.f, 0.8f);
    lastBass = lastMid = lastTreble = -1.f;

    gate.prepare (spec);
    gate.setRatio (10.f);
    gate.setAttack (1.f);
    gate.setRelease (80.f);

    preGain.prepare (spec);  preGain.setRampDurationSeconds (0.05);
    outGain.prepare (spec);  outGain.setRampDurationSeconds (0.05);

    limiter.prepare (spec);
    limiter.setThreshold (-1.f);
    limiter.setRelease (50.f);

    delayLine.prepare (spec);
    delayLine.setMaximumDelayInSamples ((int) sampleRate);
    delayLine.reset();

    reverb.prepare (spec);

    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) numCh, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    oversampling->initProcessing ((size_t) samplesPerBlock);
    setLatencySamples ((int) oversampling->getLatencyInSamples());
}

void AmpProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const float gain = param ("gain"), bass = param ("bass"), mid = param ("mid"), treble = param ("treble");
    const float volume = param ("volume"), gateAmt = param ("gate"), delayAmt = param ("delay"), revAmt = param ("reverb");

    if (bass != lastBass)
    {
        *bassF.state = *Coeffs::makeLowShelf (sampleRateHz, 120.f, 0.7f, juce::Decibels::decibelsToGain ((bass - 50.f) * 0.24f));
        lastBass = bass;
    }
    if (mid != lastMid)
    {
        *midF.state = *Coeffs::makePeakFilter (sampleRateHz, 750.f, 1.f, juce::Decibels::decibelsToGain ((mid - 50.f) * 0.24f));
        lastMid = mid;
    }
    if (treble != lastTreble)
    {
        *trebF.state = *Coeffs::makeHighShelf (sampleRateHz, 3200.f, 0.7f, juce::Decibels::decibelsToGain ((treble - 50.f) * 0.24f));
        lastTreble = treble;
    }

    gate.setThreshold (-80.f + gateAmt * 0.5f);
    preGain.setGainLinear (1.f + gain * 0.35f);
    outGain.setGainLinear (volume / 100.f * 1.2f);

    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::ProcessContextReplacing<float> context (block);

    if (gateAmt > 0.5f)
        gate.process (context);

    tightHp.process (context);
    preGain.process (context);

    // Distorcao com 4x de oversampling para reduzir aliasing
    auto upBlock = oversampling->processSamplesUp (block);
    const auto bias = std::tanh (0.15f);
    for (size_t ch = 0; ch < upBlock.getNumChannels(); ++ch)
    {
        auto* d = upBlock.getChannelPointer (ch);
        for (size_t i = 0; i < upBlock.getNumSamples(); ++i)
            d[i] = (std::tanh (1.6f * d[i] + 0.15f) - bias) * 0.5f;
    }
    oversampling->processSamplesDown (block);

    bassF.process (context);
    midF.process (context);
    trebF.process (context);
    cabHp.process (context);
    cabPk.process (context);
    cabLp.process (context);

    // Delay com feedback
    const float delaySamples = (float) (0.38 * sampleRateHz);
    const float mix = delayAmt / 100.f * 0.8f;
    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* d = block.getChannelPointer (ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const float in = d[i];
            const float del = delayLine.popSample ((int) ch, delaySamples);
            delayLine.pushSample ((int) ch, in + del * 0.35f);
            d[i] = in + del * mix;
        }
    }

    juce::dsp::Reverb::Parameters rp;
    rp.roomSize = 0.55f;
    rp.damping = 0.5f;
    rp.wetLevel = revAmt / 100.f * 0.35f;
    rp.dryLevel = 1.f;
    reverb.setParameters (rp);
    reverb.process (context);

    outGain.process (context);
    limiter.process (context);
}

juce::AudioProcessorEditor* AmpProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

int AmpProcessor::getNumPrograms() { return numPresets; }

const juce::String AmpProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, numPresets) ? presets[index].name : juce::String();
}

void AmpProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, numPresets))
        return;
    currentProgram = index;
    const auto& p = presets[index];
    const float values[] = { p.gain, p.bass, p.mid, p.treble, p.volume, p.gate, p.delay, p.reverb };
    for (int i = 0; i < 8; ++i)
        if (auto* prm = apvts.getParameter (paramIds[i]))
            prm->setValueNotifyingHost (prm->convertTo0to1 (values[i]));
}

void AmpProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AmpProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AmpProcessor();
}
