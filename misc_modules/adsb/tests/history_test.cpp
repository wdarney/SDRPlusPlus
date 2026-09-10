#include "history.h"
#include <cassert>
#include <string>
int main() {
    adsb::SessionHistory h;
    h.reset(6);
    for(unsigned i=0;i<1440;++i) h.append(i*15000ULL,std::to_string(i));
    assert(h.size()==1440 && h[1000]=="1000" && h[1439]=="1439");
    h.append(1439*15000ULL+1000,"too soon"); assert(h.size()==1440 && h[1439]=="1439");
    h.append(1440*15000ULL,"1440"); assert(h.size()==1440 && h[0]=="1");
    h.reset(1);
    for(unsigned i=0;i<800;++i)h.append(i*15000ULL,std::to_string(i));
    assert(h.size()==240 && h[0]=="560");
    h.append(1,"clock reset"); assert(h.size()==1 && h[0]=="clock reset");
    h.reset(24); assert(h.size()==0);
}
