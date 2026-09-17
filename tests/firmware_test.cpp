// Run the actual sketch with deterministic RTU, time and FreeRTOS queue fakes.
#include <cassert>
#include "../imeon_bridge_104_rw.ino"

void runFor(uint32_t ms) {
  fake::stopAt = fake::now + ms;
  fake::tick = [] { applyRegisterUpdates(); serviceLogs(); serviceLeds(); };
  try { modbusRTU(nullptr); } catch (const fake::Stop&) {}
}
void enqueue(uint16_t value) { assert(enqueueWriteCommand(0x1306,1,&value)); }
std::vector<uint16_t> commands() {
  std::vector<uint16_t> values;
  for (auto& op:fake::operations) if(op.write && op.address==0x1306) values.push_back(op.values[0]);
  return values;
}
void orderedRetry() {
  enqueue(0x4000); enqueue(0xBFFF);
  int attempts=0;
  fake::onWrite=[&](uint16_t a,const std::vector<uint16_t>&) {
    return uint8_t(a==0x1306 && attempts++==0 ? 0xE2 : 0);
  };
  runFor(15000);
  assert((commands()==std::vector<uint16_t>{0x4000,0x4000,0xBFFF}));
  assert(writeCount==2 && writeError==1 && droppedWriteCount==0);
  bool failed=false, readBetween=false;
  for(auto& op:fake::operations) {
    if(!op.write && failed) readBetween=true;
    if(op.write && op.address==0x1306) {
      if(failed) { assert(readBetween); break; }
      failed=true;
    }
  }
}
void fullQueueRetry() {
  enqueue(0x4000);
  int attempts=0;
  fake::onWrite=[&](uint16_t a,const std::vector<uint16_t>&) {
    if(a==0x1306 && attempts++==0) {
      for(int i=0;i<WRITE_QUEUE_LENGTH;i++) enqueue(0xBFFF);
      return uint8_t(0xE2);
    }
    return uint8_t(0);
  };
  runFor(25000);
  auto values=commands();
  assert(values.size()==WRITE_QUEUE_LENGTH+2);
  assert(values[0]==0x4000 && values[1]==0x4000);
  assert(droppedWriteCount==0 && !writeRetryPending);
}
void exhaustedRetry() {
  enqueue(0x4000); enqueue(0xBFFF);
  fake::onWrite=[](uint16_t a,const std::vector<uint16_t>& v) {
    return uint8_t(a==0x1306 && v[0]==0x4000 ? 0xE2 : 0);
  };
  runFor(25000);
  assert((commands()==std::vector<uint16_t>{0x4000,0x4000,0x4000,0x4000,0xBFFF}));
  assert(droppedWriteCount==1 && writeError==4 && writeCount==1);
}
int main(int argc,char** argv) {
  assert(argc==2); setup();
  std::string test=argv[1];
  if(test=="ordered_retry") orderedRetry();
  else if(test=="full_queue_retry") fullQueueRetry();
  else if(test=="exhausted_retry") exhaustedRetry();
  else return 2;
  printf("PASS %s\n",argv[1]);
}
