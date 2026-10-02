#include "voice_input.h"
#include "voice_capture_settings.h"
#include "conversation_session.h"
#include "robot_controls.h"
#include "fake_hardware.h"
#include "ambient_noise.h"
#include "music_player.h"
#include <iostream>
#include <cmath>

namespace VoiceCapture {
Settings testSettings;
const Settings &settings() { return testSettings; }
}
namespace RobotControls {
void begin() {}
bool talkPressed() { return FakeHardware::talkDown && FakeHardware::inputSamples < FakeHardware::releaseAfterSamples; }
bool abortRequested() { return FakeHardware::abortNow || FakeHardware::inputSamples >= FakeHardware::abortAfterSamples; }
bool consumeAbortRequest() { return abortRequested(); }
void clearAbortRequest() { FakeHardware::abortNow = false; }
void requestAbort() { FakeHardware::abortNow = true; }
}

namespace {
size_t checks=0;
void require(bool ok,const char *message) { ++checks; if(!ok)throw std::runtime_error(message); }
constexpr size_t drained=1920;
void reset() {
  using namespace FakeHardware;
  VoiceInput::clearRecording();
  VoiceInput::releaseAudioHardware();
  require(liveNs==0 && liveVad==0,"DSP resource leak");
  files.clear();inputSamples=readCalls=writtenFrames=serviceCalls=nsCalls=vadCalls=0;
  fileWriteCalls=fileWriteDelayMs=0;firstFileWriteInput=SIZE_MAX;
  failCaptureAllocation=false;captureAllocationCaps=0;
  failReadAt=failFileWriteAfter=abortAfterSamples=releaseAfterSamples=maxReadFrames=SIZE_MAX;
  failOpen=failI2sBegin=failI2sWrite=abortNow=failNs=failVad=false;
  talkDown=true;nsDivisor=1;vadFrames.clear();sampleAt={};
  VoiceCapture::testSettings=VoiceCapture::Settings{};
  VoiceInput::setServiceCallback([](){++FakeHardware::serviceCalls;});
  VoiceInput::begin(); VoiceInput::resetAmbientNoise(); VoiceInput::setAmbientTracking(true);
  VoiceInput::releaseAudioHardware();
}
void speech(size_t frames,int amplitude);
void ambientChecks() {
  AmbientNoise estimate;
  for (int i=0;i<64;i++) estimate.observe(40);
  require(estimate.ready() && estimate.threshold(140,5)==520,"ambient threshold ignores gain/noise");
  for(int i=0;i<16;i++) estimate.observe(500);
  require(estimate.floor()==40,"short speech contaminated ambient floor");
  for(int i=0;i<640;i++) estimate.observe(100);
  require(estimate.floor()>95 && estimate.floor()<101,"ambient failed to follow steady change");
  estimate.reset(); require(!estimate.ready() && estimate.threshold(140,5)==140,"reset retains noise");
  reset(); VoiceInput::begin(); FakeHardware::sampleAt=[](size_t n){return static_cast<int16_t>(100*std::sin(n*2*PI/32));};
  int16_t frame[128];for(int i=0;i<96;i++)require(VoiceInput::readMicrophoneMono16(frame,128),"idle measurement read");
  const float baseline=VoiceInput::ambientNoiseFloor();
  require(VoiceInput::ambientNoiseReady() && baseline>50,"production idle baseline absent");
  FakeHardware::mutexTaken=true;
  require(VoiceInput::ambientNoiseReady() && VoiceInput::ambientNoiseFloor()==baseline && VoiceInput::ambientSpeechThreshold()>140,
          "UI cannot read frozen baseline while capture holds microphone lock");
  FakeHardware::mutexTaken=false;
  VoiceInput::setAmbientTracking(false);
  FakeHardware::sampleAt=[](size_t n){return static_cast<int16_t>(3000*std::sin(n*2*PI/32));};
  for(int i=0;i<96;i++)require(VoiceInput::readMicrophoneMono16(frame,128),"frozen baseline read");
  require(std::abs(VoiceInput::ambientNoiseFloor()-baseline)<.01,"playback/recording freeze changed baseline");
  VoiceInput::setAmbientTracking(true);
  FakeHardware::inputSamples=FakeHardware::readCalls=0;
  speech(40,100);VoiceInput::AudioStats stats;
  require(VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats) && !stats.speechDetected,"VAD noise bypassed ambient gate");
  FakeHardware::inputSamples=FakeHardware::readCalls=FakeHardware::vadCalls=0;speech(40,1000);
  require(VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats) && stats.speechDetected,"clear speech rejected after ambient calibration");
  require(std::abs(VoiceInput::ambientNoiseFloor()-baseline)<.01,"capture learned its own voice");
  MusicPlayer::stop(); require(!MusicPlayer::prepare(1) && !MusicPlayer::prepare(MusicPlayer::MAX_BYTES+2),"music limits ignored");
  require(MusicPlayer::prepare(6400)!=nullptr,"music PSRAM allocation");MusicPlayer::loaded("test");
  const size_t written=FakeHardware::writtenFrames;
  require(MusicPlayer::tick() && FakeHardware::writtenFrames-written==1600,"music blocked beyond one chunk");
  require(MusicPlayer::tick() && MusicPlayer::tick(),"clip wrap failed");
  MusicPlayer::pause();require(MusicPlayer::available(),"pause lost clip");MusicPlayer::stop();require(!MusicPlayer::available(),"music stop failed");
}
void speech(size_t frames=40,int amplitude=1000) {
  FakeHardware::vadFrames.assign(frames,true);
  FakeHardware::sampleAt=[frames,amplitude](size_t n)->int16_t {
    if(n<drained || n>=drained+frames*160)return 0;
    return static_cast<int16_t>(amplitude*std::sin((n-drained)*2.0*PI/32.0+PI/2));
  };
}
bool capture(VoiceInput::AudioStats &stats) {
  return VoiceInput::recordPcmHandsFree("/test.pcm",400,30000,1200,stats);
}
void noSpeechChecks() {
  reset();VoiceInput::AudioStats stats;
  FakeHardware::sampleAt=[](size_t n)->int16_t {return n%160==0?60:n%2?-20:20;};
  require(capture(stats),"noise capture I/O failed");
  require(!stats.speechDetected && stats.fileBytes==0 && FakeHardware::files.empty(),"noise leaked to upload");
  require(stats.samples==19200 && stats.speechMs==0,"silence waited for the full listen deadline");
  std::cout<<"noise: rms="<<stats.rms<<" peak="<<stats.peak<<" speech="<<stats.speechDetected<<" fileBytes="<<stats.fileBytes<<'\n';
  reset();speech(1,5000);require(capture(stats),"click capture failed");
  require(!stats.speechDetected && FakeHardware::files.empty(),"single click accepted");
  reset();speech(19);require(capture(stats) && !stats.speechDetected,"190ms accepted");
  reset();speech(20);require(capture(stats) && stats.speechDetected,"200ms speech rejected");
  require(stats.speechMs==200 && stats.samples==22400,"silence endpoint wrong");
  reset();speech(40,10);require(capture(stats) && !stats.speechDetected,"weak VAD noise accepted");
  reset();speech(40);FakeHardware::vadFrames.assign(40,false);
  require(capture(stats) && !stats.speechDetected,"loud non-speech accepted");
  reset();speech(800);
  FakeHardware::vadFrames.assign(800,false);
  for(size_t i=0;i<800;++i)FakeHardware::vadFrames[i]=(i%23)<19;
  require(capture(stats) && !stats.speechDetected && stats.samples==128000,
          "unconfirmed intermittent speech ignored the maximum listen deadline");
  reset();speech();
  require(VoiceInput::recordPcmHandsFree("/test.pcm",400,30000,600,stats) &&
          stats.speechDetected && stats.samples==16000,"configured short pause did not end speech");
  reset();
  require(VoiceInput::recordPcmHandsFree("/test.pcm",800,30000,600,stats) &&
          !stats.speechDetected && stats.samples==12800,"initial silence ignored minimum capture time");
}
void audioChecks() {
  reset();speech();VoiceInput::AudioStats stats;
  require(capture(stats) && stats.speechDetected,"normal speech failed");
  require(stats.speechMs==400 && stats.samples==25600 && stats.fileBytes==51200,"capture duration or bytes wrong");
  require(FakeHardware::serviceCalls>0 && stats.noiseSuppressionApplied && FakeHardware::nsCalls>0,"service or NS absent");
  const auto bytes=FakeHardware::files.at("/test.pcm");
  int16_t first=0;std::memcpy(&first,bytes.data(),sizeof(first));
  require(first==5000,"first word samples lost to speech confirmation");
  const auto originalRms=stats.rms;
  reset();speech();FakeHardware::nsDivisor=2;
  require(capture(stats) && stats.speechDetected,"denoised speech failed");
  require(stats.rms<originalRms && FakeHardware::nsCalls>0,"NS output not saved");
  reset();speech();VoiceCapture::testSettings.noiseSuppression=false;
  require(capture(stats) && stats.speechDetected && !stats.noiseSuppressionApplied && FakeHardware::nsCalls==0,"NS bypass failed");
  reset();speech(40,10000);VoiceCapture::testSettings.gain=12;
  require(capture(stats) && stats.clippedSamples>0 && stats.peak<=32768,"clipping missing or wraps");
  reset();speech();FakeHardware::maxReadFrames=80;
  require(capture(stats) && stats.speechDetected && stats.speechMs==400,"partial I2S reads lost speech");
  reset();speech();VoiceCapture::testSettings.sensitivity=0;capture(stats);
  require(FakeHardware::vadMode==3,"low sensitivity VAD mode wrong");
  reset();speech();VoiceCapture::testSettings.sensitivity=100;capture(stats);
  require(FakeHardware::vadMode==0,"high sensitivity VAD mode wrong");
}
void storageTimingChecks() {
  reset();speech();VoiceInput::AudioStats stats;
  FakeHardware::fileWriteDelayMs=250;
  require(capture(stats) && stats.speechDetected,"slow storage rejected valid speech");
  require(FakeHardware::firstFileWriteInput==drained+stats.samples,
          "storage writes still block microphone acquisition");
  require(stats.samples==25600 && stats.fileBytes==51200,"buffered recording lost audio");
  require(stats.storageBytesWritten==51200 && stats.storageError==0,
          "successful write diagnostics incorrect");
  require(stats.durationMs==1600 && stats.storageMs==1750 && stats.maxReadGapUs<=16000,
          "slow storage polluted acquisition timing");
  require(FakeHardware::captureAllocationCaps==3,"capture buffer can consume internal RAM");
  reset();
  require(capture(stats) && !stats.speechDetected && FakeHardware::fileWriteCalls==0,
          "silence still writes flash");
  reset();speech();FakeHardware::failCaptureAllocation=true;
  require(!capture(stats) && stats.error[0] && FakeHardware::readCalls==0 &&
          FakeHardware::files.empty(),"buffer allocation failure ignored");
  reset();speech();FakeHardware::fileWriteDelayMs=250;
  VoiceInput::setServiceCallback([](){if(FakeHardware::fileWriteCalls)FakeHardware::abortNow=true;});
  require(!capture(stats) && stats.error[0] && FakeHardware::files.empty(),
          "web stop ignored while saving buffered audio");
}
void errorAndControlChecks() {
  VoiceInput::AudioStats stats;
  for(int failure=0;failure<7;++failure){
    reset();speech();
    if(failure==0)FakeHardware::failNs=true;
    if(failure==1)FakeHardware::failVad=true;
    if(failure==2)FakeHardware::failOpen=true;
    if(failure==3)FakeHardware::failReadAt=drained+160;
    if(failure==4)FakeHardware::failFileWriteAfter=400;
    if(failure==5)FakeHardware::failI2sBegin=true;
    if(failure==6)FakeHardware::failReadAt=0;
    require(!capture(stats) && FakeHardware::files.empty(),"I/O or DSP failure kept recording");
    require(stats.error && stats.error[0],"recording failure has no diagnostic reason");
    require(stats.fileBytes==0,"failed recording reports an uploadable file");
    if(failure==3 || failure==4) {
      require(stats.samples>0 && stats.rms>0 && stats.peak>0,"failure lost captured audio statistics");
    }
    require(FakeHardware::liveNs==0 && FakeHardware::liveVad==0,"failure leaked DSP memory");
  }
  reset();speech();FakeHardware::abortAfterSamples=drained+160*5;
  require(!capture(stats) && FakeHardware::files.empty(),"abort did not remove recording");
  require(stats.error[0] && stats.samples==800 && stats.rms>0,"abort lost diagnostic statistics");
  reset();speech();FakeHardware::releaseAfterSamples=drained+160*30;
  require(VoiceInput::recordPcmPushToTalk("/test.pcm",400,20000,stats) && stats.speechDetected,"PTT failed");
  require(stats.samples==6400,"PTT ignored minimum/release");
  reset();speech();VoiceInput::setServiceCallback([](){FakeHardware::abortNow=true;});
  require(!capture(stats) && FakeHardware::files.empty(),"web stop ignored during recording");
  reset();speech();VoiceInput::setServiceCallback([](){delay(51);});
  require(capture(stats) && stats.speechDetected && stats.fileBytes==51200,"web elapsed time falsely cancelled recording");
  require(stats.rms>0 && stats.peak>0 && !stats.error[0],"successful capture has invalid statistics");
  reset();speech();VoiceInput::setServiceCallback([](){delay(25);});
  require(capture(stats) && stats.speechDetected && stats.samples==25600,"cumulative work time falsely cancelled recording");
  reset();speech();VoiceInput::setServiceCallback([](){delay(1);});
  require(capture(stats) && stats.speechDetected && stats.samples==25600,"small per-frame overhead falsely cancelled recording");
  struct AudioStream:Stream {
    size_t reads=0;
    size_t readBytes(char *data,size_t count)override {++reads;std::memset(data,0,count);return count;}
  } stream;
  reset();VoiceInput::setServiceCallback([](){++FakeHardware::serviceCalls;FakeHardware::abortNow=true;});
  require(!VoiceInput::playPcmStream(stream,32000) && stream.reads==0 && FakeHardware::serviceCalls>0,"web stop ignored during playback");
  reset();VoiceInput::setServiceCallback([](){delay(51);});
  require(VoiceInput::playPcmStream(stream,32000) && stream.reads>0,"web elapsed time falsely cancelled playback");
  reset();VoiceInput::begin();require(VoiceInput::discardMicrophone(240),"RX drain failed");
  require(FakeHardware::inputSamples==3840,"RX not fully drained");
}
void conversationChecks() {
  ConversationSession session;
  session.start(false);session.complete(true,1000);
  require(!session.active() && !session.takeNext(2000),"single turn followed up");
  session.start(true);
  for(uint32_t turn=0;turn<10;++turn){
    const uint32_t at=turn*1000;
    session.complete(true,at);
    require(session.active() && !session.takeNext(at+599),"follow-up before playback cooldown");
    require(session.takeNext(at+600) && !session.takeNext(at+601),"follow-up capped or duplicated");
  }
  session.complete(false,12000);require(!session.active() && !session.takeNext(14000),"failure did not stop conversation");
  session.start(true);session.complete(true,15000);session.cancel();
  require(!session.active() && !session.takeNext(16000),"stop did not clear pending turn");
  session.start(true);session.complete(true,UINT32_MAX-100);
  require(!session.takeNext(498) && session.takeNext(499),"millis wrap broke follow-up");
}
void manualRecordingChecks() {
  VoiceInput::AudioStats stats;
  reset();speech();FakeHardware::talkDown=false;
  VoiceInput::setServiceCallback([](){
    if(FakeHardware::inputSamples>=drained+32000)VoiceInput::requestRecordingFinish();
  });
  require(VoiceInput::recordPcmManual("/test.pcm",25000,stats) && stats.speechDetected,
          "web finish cancelled rather than saved speech");
  require(stats.samples==32000 && stats.fileBytes==64000 && !RobotControls::abortRequested(),
          "manual capture stopped at silence or depended on a hardware button");
  reset();speech();VoiceInput::requestRecordingFinish();
  require(VoiceInput::recordPcmManual("/test.pcm",3000,stats) && stats.samples==48000,
          "manual finish leaked into the next capture or maximum duration ignored");
  reset();VoiceInput::setServiceCallback([](){VoiceInput::requestRecordingFinish();});
  require(VoiceInput::recordPcmManual("/test.pcm",25000,stats) && stats.samples==6400 &&
          !stats.speechDetected && FakeHardware::files.empty(),
          "immediate manual finish uploaded silence or ignored minimum duration");
  reset();speech();VoiceInput::setServiceCallback([](){FakeHardware::abortNow=true;});
  require(!VoiceInput::recordPcmManual("/test.pcm",25000,stats) && FakeHardware::files.empty(),
          "manual recording ignored conversation cancellation");
  reset();speech();VoiceInput::setServiceCallback([](){VoiceInput::requestRecordingFinish();});
  require(capture(stats) && stats.samples==25600,
          "manual finish flag changed the hands-free silence endpoint");
}
void memoryRecordingChecks() {
  reset();speech();VoiceInput::AudioStats stats;
  FakeHardware::fileWriteDelayMs=1000;FakeHardware::failOpen=true;
  require(VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats,true),
          "memory recording still depends on file storage");
  require(stats.speechDetected && stats.samples==25600 && stats.fileBytes==51200 &&
          stats.storageMs==0 && FakeHardware::fileWriteCalls==0 && FakeHardware::files.empty(),
          "memory recording still incurs flash writes");
  require(VoiceInput::recordingData() && VoiceInput::recordingBytes()==51200,
          "upload buffer not retained");
  int16_t first=0;std::memcpy(&first,VoiceInput::recordingData(),sizeof(first));
  require(first==5000,"memory upload lost first samples");
  VoiceInput::clearRecording();VoiceInput::clearRecording();
  require(!VoiceInput::recordingData() && VoiceInput::recordingBytes()==0,
          "upload buffer not released");
  reset();speech();
  require(VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats,true),
          "first memory capture failed");
  FakeHardware::inputSamples=FakeHardware::readCalls=0;
  FakeHardware::sampleAt={};FakeHardware::vadFrames.clear();
  require(VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats,true) &&
          !stats.speechDetected && !VoiceInput::recordingData() && VoiceInput::recordingBytes()==0,
          "silence can upload the preceding recording");
  reset();speech();FakeHardware::abortAfterSamples=drained+800;
  require(!VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats,true) &&
          !VoiceInput::recordingData() && FakeHardware::fileWriteCalls==0,
          "cancelled memory capture retained upload data");
  reset();speech();FakeHardware::failCaptureAllocation=true;
  require(!VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats,true) &&
          !VoiceInput::recordingData(),"failed allocation retained upload data");
  reset();speech();FakeHardware::talkDown=false;
  VoiceInput::setServiceCallback([](){if(FakeHardware::inputSamples>=drained+16000)VoiceInput::requestRecordingFinish();});
  require(VoiceInput::recordPcmManual("/test.pcm",25000,stats,true) &&
          stats.samples==16000 && VoiceInput::recordingBytes()==32000 &&
          FakeHardware::fileWriteCalls==0,"manual memory finish/save failed");
  reset();speech();FakeHardware::releaseAfterSamples=drained+6400;
  require(VoiceInput::recordPcmPushToTalk("/test.pcm",400,20000,stats,true) &&
          stats.samples==6400 && VoiceInput::recordingBytes()==12800 &&
          FakeHardware::fileWriteCalls==0,"PTT memory release/save failed");
  reset();speech(2600);
  require(VoiceInput::recordPcmHandsFree("/test.pcm",400,25000,1200,stats,true) &&
          stats.samples==400000 && VoiceInput::recordingBytes()==800000 &&
          FakeHardware::fileWriteCalls==0,"25-second memory upload cap changed");
  VoiceInput::clearRecording();
}
}
int main(){try{ambientChecks();noSpeechChecks();audioChecks();storageTimingChecks();errorAndControlChecks();conversationChecks();manualRecordingChecks();memoryRecordingChecks();
std::cout<<"PASS: "<<checks<<" production capture/session checks (NS/VAD hardware faked)\n";
return 0;}catch(const std::exception &error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
