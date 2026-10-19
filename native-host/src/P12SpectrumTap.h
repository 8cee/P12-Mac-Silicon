#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <algorithm>
#include <cstdint>
namespace p12 {
// Callback writes a fixed atomic ring only. FFT runs on the command worker.
class SpectrumTap {
 static constexpr unsigned size=1024;
 std::array<std::atomic<float>,size> ring{};std::atomic<uint32_t> written{0};uint32_t cursor=0;
public:
 void capture(const float* l,const float* r,int n) noexcept {for(int i=0;i<n;i++)ring[cursor++&(size-1)].store(l? .5f*(l[i]+r[i]):0,std::memory_order_relaxed);written.store(cursor,std::memory_order_release);}
 std::array<int,12> read(double rate) const noexcept {
  rate=std::clamp(std::isfinite(rate)?rate:48000.0,8000.0,192000.0);
  std::array<std::complex<double>,size> fft{};const auto end=written.load(std::memory_order_acquire);
  for(unsigned i=0;i<size;i++)fft[i]={ring[(end-size+i)&(size-1)].load(std::memory_order_relaxed)*(.5-.5*std::cos(6.283185307179586*i/(size-1))),0};
  for(unsigned i=1,j=0;i<size;i++){unsigned bit=size>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(fft[i],fft[j]);}
  for(unsigned length=2;length<=size;length<<=1){const auto step=std::polar(1.0,-6.283185307179586/length);for(unsigned i=0;i<size;i+=length){std::complex<double> w{1,0};for(unsigned j=0;j<length/2;j++){const auto a=fft[i+j],b=w*fft[i+j+length/2];fft[i+j]=a+b;fft[i+j+length/2]=a-b;w*=step;}}}
  std::array<int,12> result{};const double high=std::min(20000.0,rate*.49);
  for(int band=0;band<12;band++){const double lo=40*std::pow(high/40,band/12.0),hi=40*std::pow(high/40,(band+1)/12.0);const unsigned begin=std::clamp(unsigned(lo*size/rate),1u,size/2-1),finish=std::clamp(unsigned(std::ceil(hi*size/rate)),begin+1,size/2);double magnitude=0;for(unsigned k=begin;k<finish;k++)magnitude=std::max(magnitude,std::abs(fft[k])*4/size);const double db=20*std::log10(std::max(1.e-12,magnitude));result[band]=int(std::clamp((db+80)/80,0.0,1.0)*255);}
  return result;
 }
};
}
