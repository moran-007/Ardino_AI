#include <iostream>
#include <cassert>
#include "music_echo_capture.cpp"

static int checks = 0;
#define CHECK(v) do { ++checks; if (!(v)) { std::cerr << __LINE__ << ": " #v "\n"; std::abort(); } } while(0)
static void *tx=reinterpret_cast<void *>(1), *rx=reinterpret_cast<void *>(2), *third=reinterpret_cast<void *>(3);
static int32_t speaker[480], microphone[480], thirdMic[480];
static void fill(int base) {
  for (int i=0;i<240;++i) {
    speaker[i*2]=int32_t(base+i)*65536; speaker[i*2+1]=int32_t(-base)*65536;
    microphone[i*2]=int32_t(100+i)*65536; microphone[i*2+1]=int32_t(200+i)*65536;
    thirdMic[i*2]=int32_t(300+i)*65536;
  }
}
static void emit(int channel, int32_t *data, size_t bytes=1920) {
  // Deprecated secondary data is deliberately different from dma_buf.
  i2s_event_data_t event{nullptr, data, bytes};
  auto cb=channel==1 ? callbacks[channel].on_sent : callbacks[channel].on_recv;
  CHECK(cb != nullptr); cb(reinterpret_cast<void *>(uintptr_t(channel)), &event, nullptr);
}
int main() {
  using namespace MusicEcho;
  CHECK(begin(tx,rx,third,7)); CHECK(active()); CHECK(frameSize()==512);
  CHECK(disables==3 && enables==3);
  CHECK(lastConfig.caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  CHECK(lastConfig.sample_rate==16000 && lastConfig.filter_length==4 && lastConfig.nlp_level==0 && lastConfig.mode==1);
  NativeFrame out[240]; fill(1000); setMuted(false);
  emit(1,speaker); emit(3,thirdMic); emit(2,microphone);
  CHECK(readNative(out,240,1));
  for (int i=0;i<240;++i) {
    CHECK(out[i].mic[0]==100+i); CHECK(out[i].mic[1]==200+i);
    CHECK(out[i].mic[2]==300+i); CHECK(out[i].reference==1000+i);
  }
  CHECK(!readNative(out,1,2)); CHECK(!readNative(out,241,1)); CHECK(!readNative(nullptr,1,1));
  setMuted(true); emit(1,speaker); emit(2,microphone); CHECK(readNative(out,240,1));
  for (auto &f:out) CHECK(f.reference==0);
  setMuted(false); emit(1,speaker); delay(11); emit(2,microphone); CHECK(readNative(out,240,1));
  for (auto &f:out) { CHECK(f.reference==0); CHECK(f.mic[2]==0); }
  CHECK(diagnostics().missingReferenceFrames==240);
  // Capacity overrun must discard mic and reference together, preserving order.
  for (int b=0;b<13;++b) { fill(2000+b*240); emit(1,speaker); emit(2,microphone); delay(5); }
  CHECK(diagnostics().droppedFrames==240);
  CHECK(readNative(out,126,1)); CHECK(out[0].reference==2240);
  CHECK(readNative(out,114,1)); CHECK(out[0].reference==2366);
  for (int b=2;b<13;++b) {
    CHECK(readNative(out,240,1)); CHECK(out[0].reference==2000+b*240);
  }
  emit(2,microphone,1912); CHECK(!readNative(out,1,1));
  alignas(16) int16_t near[512],ref[512],clean[512];
  for (int i=0;i<512;++i) { near[i]=100; ref[i]=200; }
  process(near,ref,clean); CHECK(processedAec==1);
  for (int i=0;i<512;++i) CHECK(clean[i]==50);
  const auto d=diagnostics(); CHECK(d.frames==1 && d.inputRms==100 && d.referenceRms==200 && d.outputRms==50);
  end(); CHECK(!active()); CHECK(!readNative(out,1,1)); CHECK(destroyedAec==1);
  failAec=true; CHECK(!begin(tx,rx,nullptr)); CHECK(!active()); CHECK(*error()); end(); failAec=false;
  fakeChunk=256; CHECK(!begin(tx,rx,nullptr)); CHECK(!active()); end(); fakeChunk=512;
  fakeHeapFailureCaps=MALLOC_CAP_INTERNAL; CHECK(!begin(tx,rx,nullptr)); CHECK(*error()); end(); fakeHeapFailureCaps=0;
  failInstall=2; CHECK(!begin(tx,rx,nullptr)); CHECK(!active()); CHECK(*error());
  // Partial callback install retains owned storage until channels are deleted.
  emit(1,speaker); end(); failInstall=0;
  CHECK(begin(tx,rx,nullptr)); CHECK(diagnostics().droppedFrames==0);
  fill(1000); setMuted(false); emit(1,speaker); emit(2,microphone); CHECK(readNative(out,240,1));
  for(int i=0;i<240;++i) { CHECK(out[i].mic[0]==100+i); CHECK(out[i].mic[1]==0 && out[i].mic[2]==0); CHECK(out[i].reference==1000+i); }
  end();
  CHECK(begin(tx,rx,third,5)); fill(1500); emit(1,speaker); emit(3,thirdMic); emit(2,microphone); CHECK(readNative(out,240,1));
  for(int i=0;i<240;++i) { CHECK(out[i].mic[0]==100+i && out[i].mic[1]==0 && out[i].mic[2]==300+i); CHECK(out[i].reference==1500+i); }
  for(int i=0;i<13;++i) { emit(1,speaker); emit(2,microphone); }
  CHECK(diagnostics().droppedFrames==240);
  discardPending(); CHECK(diagnostics().droppedFrames==0 && diagnostics().frames==0 && active());
  CHECK(!readNative(out,1,1));
  emit(1,speaker); emit(2,microphone); CHECK(readNative(out,240,1)); CHECK(out[0].reference==1500);
  end(); CHECK(!begin(tx,rx,nullptr,0)); CHECK(!begin(tx,rx,nullptr,5));
  std::cout << "PASS " << checks << " DMA paired echo capture checks (fake vendor DSP)\n";
}
