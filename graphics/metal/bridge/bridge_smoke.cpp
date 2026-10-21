// SPDX-License-Identifier: MIT
// Host run of the bridge self-test over the host Turnip and the host kernel stack (dyld-interposed IOKit shim). The software CP executes
// nothing, so result counts are not expected to be zero here; the run proves every step builds, submits and retires.
#include "NativeMetalBridge.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static void mark(uint32_t value) {std::printf("mark %x\n",value);std::fflush(stdout);}
int main(int argc,char **argv) {
    if(argc<3)return 2;
    std::vector<uint8_t> qc;
    if(argc>3) {
        FILE *f=std::fopen(argv[3],"rb");if(!f)return 3;
        std::fseek(f,0,SEEK_END);qc.resize(size_t(std::ftell(f)));std::fseek(f,0,SEEK_SET);
        if(std::fread(qc.data(),1,qc.size(),f)!=qc.size())return 4;std::fclose(f);
    }
    NmbSelftest r{};
    const uint32_t code=nmb_selftest(argv[1],argv[2],qc.data(),qc.size(),mark,&r);
    std::printf("code %x stage %x compute_bad %u render_bad %u import_bad %u qc %u kernels %u/%u pairs %u/%u msg '%s'\n",code,r.stage,
                r.compute_bad,r.render_bad,r.import_bad,r.qc_functions,r.qc_kernels_ok,r.qc_kernels_failed,r.qc_pairs_ok,r.qc_pairs_failed,r.message);
    return code?1:0;
}
