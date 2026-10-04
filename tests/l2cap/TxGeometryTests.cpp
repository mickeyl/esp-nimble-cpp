#include "NimBLEL2CAPTxGeometry.h"
#include <initializer_list>
#include <cassert>
#include <cstdint>
int main() {
    static_assert(nimbleL2capTxSduWidth(1251,1251)==1249);
    static_assert(nimbleL2capTxSduWidth(672,23)==665);
    static_assert(nimbleL2capTxSduWidth(100,1251)==100);
    static_assert(nimbleL2capTxSduWidth(1251,0)==1251);
    static_assert(nimbleL2capTxSduWidth(1251,2)==1251);
    for (uint32_t mtu=1;mtu<=65535;++mtu) {
        for (uint16_t mps : {uint16_t(23),uint16_t(247),uint16_t(248),uint16_t(251),uint16_t(1251),uint16_t(65535)}) {
            const auto width=nimbleL2capTxSduWidth(mtu,mps);
            assert(width>0 && width<=mtu);
            if (mtu+2>=mps) {
                assert((uint32_t(width)+2)%mps==0);
                assert(uint32_t(width)+mps>mtu);
            } else { assert(width==mtu); }
        }
    }
    // Chunking a framed 4096-byte response must preserve all bytes and avoid
    // a tiny PDU on each full SDU: 1249,1249,1249,353 instead of 1251*3+347.
    unsigned remaining=4100, sdus=0, pdus=0;
    const auto width=nimbleL2capTxSduWidth(1251,1251);
    while (remaining) {
        unsigned n=remaining<width?remaining:width;
        pdus+=(n+2+1250)/1251;
        remaining-=n;
        ++sdus;
    }
    assert(sdus==4 && pdus==4);
}
