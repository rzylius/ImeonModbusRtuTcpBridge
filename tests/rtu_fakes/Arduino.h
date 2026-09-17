#pragma once
#include <cstdint>
#include <cstddef>
#define lowByte(x) ((uint8_t)((x)&255))
#define highByte(x) ((uint8_t)((x)>>8))
#define bitRead(x,n) (((x)>>(n))&1)
#define bitWrite(x,n,b) ((b)?((x)|=(1UL<<(n))):((x)&=~(1UL<<(n))))
inline uint16_t word(uint8_t a,uint8_t b){return (uint16_t(a)<<8)|b;}
inline uint16_t word(uint16_t a){return a;}
inline uint32_t millis(){static uint32_t t=0;return ++t;}
class Stream {public: virtual int read()=0;virtual int available()=0;virtual size_t write(uint8_t)=0;virtual void flush()=0;};
