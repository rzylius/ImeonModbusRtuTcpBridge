// Run the actual sketch with deterministic RTU, time and FreeRTOS queue fakes.
#include <cassert>
#include "imeon_bridge_104_rw.ino"

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

uint32_t batteryAge() {
  const uint16_t a=FRESHNESS_REGISTER_START+2*4;
  return (uint32_t(mbTcp.Hreg(a+2))<<16)|mbTcp.Hreg(a+3);
}
uint16_t batteryStatus() { return mbTcp.Hreg(FRESHNESS_REGISTER_START+2*4+1); }
void freshnessAge() {
  applyRegisterUpdates();
  assert(batteryStatus()==0 && batteryAge()==UINT32_MAX);
  uint16_t values[]={530,30,0,80};
  fake::now=1000;
  assert(queueRegisterUpdate(0x0300,4,values));
  fake::now=2000; applyRegisterUpdates();
  assert(batteryStatus()==3 && batteryAge()==1000);
  fake::now=1000+TELEMETRY_STALE_AFTER_MS; applyRegisterUpdates();
  assert(batteryStatus()==3);
  fake::now++; applyRegisterUpdates();
  assert(batteryStatus()==1 && mbTcp.Hreg(0x0303)==80);
  assert(queueRegisterUpdate(0x0300,4,values)); applyRegisterUpdates();
  assert(batteryStatus()==3 && batteryAge()==0);
}
void freshnessWrap() {
  uint16_t values[]={530,30,0,80};
  fake::now=UINT32_MAX-100;
  assert(queueRegisterUpdate(0x0300,4,values)); applyRegisterUpdates();
  fake::now=50; applyRegisterUpdates();
  assert(batteryAge()==151 && batteryStatus()==3);
  fake::now=UINT32_MAX-10; applyRegisterUpdates();
  assert(batteryAge()==UINT32_MAX && batteryStatus()==1);
  fake::now=100; applyRegisterUpdates();
  assert(batteryAge()==UINT32_MAX && batteryStatus()==1);
}
void freshnessRejected() {
  int batteryReads=0;
  fake::onRead=[&](uint16_t a,std::vector<uint16_t>& v) {
    if(a==0x0300) v={uint16_t(++batteryReads==1 ? 530 : 900),30,0,80};
    return uint8_t(0);
  };
  runFor(160000);
  assert(batteryReads>2 && invalidReadCount==batteryReads-1);
  assert(batteryStatus()==1 && mbTcp.Hreg(0x0300)==530);
  size_t reads=0;
  for(auto& op:fake::operations) if(!op.write) reads++;
  assert(readCount==reads-invalidReadCount);
}
void freshnessFailedAndDropped() {
  uint16_t values[]={530,30,0,80};
  assert(queueRegisterUpdate(0x0300,4,values)); applyRegisterUpdates();
  fake::onRead=[](uint16_t,std::vector<uint16_t>&) { return uint8_t(0xE2); };
  runFor(130000);
  assert(batteryStatus()==1 && readCount==0 && readError>0);
  const auto age=batteryAge();
  for(int i=0;i<REGISTER_UPDATE_QUEUE_LENGTH;i++)
    assert(queueRegisterUpdate(40000,1,values));
  assert(!queueRegisterUpdate(0x0300,4,values));
  applyRegisterUpdates();
  assert(batteryStatus()==1 && batteryAge()==age);
}
void sustainedWrites() {
  enqueue(0xBFFF);
  fake::onWrite=[](uint16_t a,const std::vector<uint16_t>&) {
    if(a==0x1306) enqueue(0xBFFF); // The producer never lets the queue empty.
    return uint8_t(0);
  };
  runFor(50000);
  size_t consecutive=0, maximum=0;
  std::vector<uint16_t> ranges;
  for(auto& op:fake::operations) {
    if(op.write && op.address==0x1306) {
      maximum=std::max(maximum,++consecutive);
    } else if(!op.write) {
      consecutive=0; ranges.push_back(op.address);
    }
  }
  assert(writeCount>10 && uxQueueMessagesWaiting(commandQueue)==1);
  assert(maximum==MAX_CONSECUTIVE_WRITES);
  assert(ranges.size()>=size_t(rangeCount));
  for(size_t i=0;i<ranges.size();i++) assert(ranges[i]==predefinedRanges[i%rangeCount].start);
}
void tcpTransactionIds() {
  for(uint16_t id : {0,1,32768,65535}) {
    Modbus::frame_arg_t source={7,0x7f000001,id,true};
    uint8_t single[]={6,0x13,6,0xBF,0xFF};
    auto before=mbTcp.socket.frames.size();
    assert(onModbusRequest(single,sizeof(single),&source)==Modbus::EX_SUCCESS);
    assert(mbTcp.socket.frames.size()==before); // Deferred, never reentrant.
    serviceTcpResponses();
    assert((mbTcp.socket.frames.back()==std::vector<uint8_t>{
      uint8_t(id>>8),uint8_t(id),0,0,0,6,7,6,0x13,6,0xBF,0xFF}));
    uint8_t multiple[]={0x10,0x13,6,0,1,2,0xBF,0xFF};
    assert(onModbusRequest(multiple,sizeof(multiple),&source)==Modbus::EX_SUCCESS);
    serviceTcpResponses();
    assert((mbTcp.socket.frames.back()==std::vector<uint8_t>{
      uint8_t(id>>8),uint8_t(id),0,0,0,6,7,0x10,0x13,6,0,1}));
    uint8_t illegal[]={0x7f};
    assert(onModbusRequest(illegal,sizeof(illegal),&source)==Modbus::EX_SUCCESS);
    serviceTcpResponses();
    assert((mbTcp.socket.frames.back()==std::vector<uint8_t>{
      uint8_t(id>>8),uint8_t(id),0,0,0,3,7,0xff,1}));
  }
}
void tcpSendFailures() {
  uint8_t pdu[]={0x86,3};
  mbTcp.socket.live=false;
  assert(!mbTcp.sendServerResponse(IPAddress(1),0,7,pdu,2));
  assert(mbTcp.socket.frames.empty());
  mbTcp.socket.live=true; mbTcp.socket.writeLimit=4;
  assert(!mbTcp.sendServerResponse(IPAddress(1),0,7,pdu,2));
  mbTcp.socket.writeLimit=SIZE_MAX;
  assert(!mbTcp.sendServerResponse(IPAddress(1),0,7,nullptr,2));
  assert(!mbTcp.sendServerResponse(IPAddress(1),0,7,pdu,0));
  assert(!mbTcp.sendServerResponse(IPAddress(1),0,7,pdu,6));
  assert(mbTcp.sendServerResponse(IPAddress(1),0,7,pdu,2));
}
int main(int argc,char** argv) {
  assert(argc==2); setup();
  std::string test=argv[1];
  if(test=="ordered_retry") orderedRetry();
  else if(test=="full_queue_retry") fullQueueRetry();
  else if(test=="exhausted_retry") exhaustedRetry();
  else if(test=="freshness_age") freshnessAge();
  else if(test=="freshness_wrap") freshnessWrap();
  else if(test=="freshness_rejected") freshnessRejected();
  else if(test=="freshness_failed_dropped") freshnessFailedAndDropped();
  else if(test=="sustained_writes") sustainedWrites();
  else if(test=="tcp_transaction_ids") tcpTransactionIds();
  else if(test=="tcp_send_failures") tcpSendFailures();
  else return 2;
  printf("PASS %s\n",argv[1]);
}
