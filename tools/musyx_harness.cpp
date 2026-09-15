// MusyX offline decode HARNESS (Approach B): run the recompiled musyx_audio
// synth standalone on one sample and capture its PCM output — uses the EXACT
// N64 decoder, no ADPCM-format guessing.
//
// STATUS: scaffold. Deps wired; the remaining work is (1) build it, (2) set up
// a minimal command list + voice so the synth decodes a chosen sample (the
// voice/command format is the last unknown — see TODO below).
//
// BUILD (deps, from rsp.hpp + the ucode):
//   clang++ -std=c++20 -msse4.1 -O2 \
//     -I lib/N64ModernRuntime/librecomp/include \
//     -I lib/N64ModernRuntime/ultramodern/include \
//     -I <RecompiledFuncs include dir for recomp.h> \
//     -include include/rsp_dpc_macros.h \
//     tools/musyx_harness.cpp build/factor5_ucode/musyx_audio_recompiled.c \
//     <librecomp rsp tables .cpp (rspReciprocals/rspInverseSquareRoots)> \
//     -o musyx_harness
//
// The synth needs these host-provided globals (declared extern in rsp.hpp):
//   uint8_t  dmem[0x1000];
//   uint16_t rspReciprocals[512], rspInverseSquareRoots[512];   // grab from librecomp
// and the DPC bridge symbols from rsp_dpc_macros.h:
//   uint32_t g_rsp_dpc_start, g_rsp_dpc_end; void rsp_dpc_submit(...){}   // stub (no DPC in audio)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "librecomp/rsp.hpp"

// dmem + rspReciprocals/rspInverseSquareRoots are DEFINED in librecomp/src/rsp.cpp
// (which we link). Call recomp::rsp::constants_init() to fill the reciprocal tables.

// --- DPC bridge stubs (audio path never touches the RDP) ---
uint32_t g_rsp_dpc_start = 0, g_rsp_dpc_end = 0;
void rsp_dpc_submit(uint8_t*, uint32_t, uint32_t) {}

extern RspExitReason musyx_audio(uint8_t* rdram, uint32_t ucode_addr);
extern RspExitReason factor5_boot(uint8_t* rdram, uint32_t ucode_addr);

// poke a big-endian u32 into DMEM (i^3 swizzle, matching the RSP MEM layout)
static void dmem_poke(uint32_t off, uint32_t val){ for(int j=0;j<4;j++) dmem[(off+j)^3]=(uint8_t)(val>>(24-j*8)); }

static uint32_t be32(const uint8_t* b){ return (b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]; }

// load a file
static std::vector<uint8_t> slurp(const char* p){
    FILE* f=fopen(p,"rb"); if(!f){ printf("can't open %s\n",p); return {}; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    std::vector<uint8_t> v(n); fread(v.data(),1,n,f); fclose(f); return v;
}

int main(int argc, char** argv){
    recomp::rsp::constants_init();  // fill rspReciprocals / rspInverseSquareRoots
    // 8MB fake RDRAM
    static std::vector<uint8_t> rdram(0x800000, 0);
    auto data = slurp("dumps/musyx_data.bin");   // ucode DMEM data section (0x800)
    auto samp = slurp("dumps/snd/samp_SND.bin");  // ADPCM sample bank
    auto sdir = slurp("dumps/snd/sdir_SND.bin");  // sample directory
    if (data.empty() || samp.empty()) { printf("missing dumps (run extract_sounds.py + ROGUESQ_DUMP_AUDIO_UCODE)\n"); return 1; }

    // Place samp_SND in RDRAM (byte-swapped ^3 so RSP DMA reads it correctly).
    const uint32_t SAMP_ADDR = 0x100000;
    for (size_t i=0;i<samp.size() && SAMP_ADDR+i<rdram.size(); ++i)
        rdram[(SAMP_ADDR+i)^3] = samp[i];

    // ucode_data must be in RDRAM @0x0A4E00 (where the boot DMAs it from), matching the game.
    const uint32_t UCODE_DATA = 0x0A4E00;
    for (size_t i=0;i<data.size(); ++i) rdram[(UCODE_DATA+i)^3]=data[i];

    // DMEM: load the ucode data section to DMEM[0] (mimics SP_BOOT, as the in-game runner does).
    std::memset(dmem,0,0x1000);
    for (size_t i=0;i<data.size() && i<0xFC0; ++i) dmem[i^3]=data[i];

    // OSTask @ DMEM[0xFC0], with the game's real values (from [audio-ucode] log) so the synth
    // reads a valid task. data_ptr → a minimal (empty) command list; data_size=0 = no voices yet.
    const uint32_t CMDLIST = 0x010000, OUTBUF = 0x020000;
    dmem_poke(0xFC0+0x10, 0x80099FE0);              // ucode
    dmem_poke(0xFC0+0x14, 0x1000);                  // ucode_size
    dmem_poke(0xFC0+0x18, 0x800A4E00);              // ucode_data
    dmem_poke(0xFC0+0x1C, 0x800);                   // ucode_data_size
    dmem_poke(0xFC0+0x28, 0x80000000|OUTBUF);       // output_buff
    dmem_poke(0xFC0+0x2C, 0x2000);                  // output_buff_size
    dmem_poke(0xFC0+0x30, 0x80000000|CMDLIST);      // data_ptr (command list)
    dmem_poke(0xFC0+0x34, 0);                       // data_size (# voices) — 0 for first run

    // Run boot then synth, exactly like the in-game musyx_audio_runner.
    RspExitReason br = factor5_boot(rdram.data(), 0x80099FE0);
    printf("factor5_boot -> %d\n", (int)br);
    RspExitReason mr = musyx_audio(rdram.data(), 0x80099FE0);
    printf("musyx_audio -> %d\n", (int)mr);
    // scan output buffer for non-silence
    int peak=0; for(uint32_t i=0;i<0x2000;i+=2){ int16_t v=(int16_t)((rdram[(OUTBUF+i)^3]<<8)|rdram[(OUTBUF+i+1)^3]); if(v<0)v=-v; if(v>peak)peak=v; }
    printf("output_buff peak=%d (0=silence)\n", peak);

    // ---- TODO (the remaining RE): build a minimal command list + ONE voice ----
    // The synth reads the OSTask at DMEM[0xFC0]; data_ptr@+0x30 points to the
    // command list (a voice array, stride 0x4C, voice ptr at entry+0x08). To
    // decode sample N: construct a command list with one ACTIVE voice whose
    // sample pointer = SAMP_ADDR + sdir[N].offset, with the voice's rate/length/
    // active fields set so it plays 1:1 (no resample) into the output buffer.
    // The voice field layout (rate, length, vol, active flag, loop) must be
    // reverse-engineered from the synth's voice reads (or captured from a real
    // active voice in-game). Then set OSTask.data_ptr to the command list,
    // OSTask.output_buff to a known RDRAM addr, poke the OSTask to DMEM[0xFC0].
    //
    // After that: RspExitReason r = musyx_audio(rdram.data(), 0x80099FE0);
    // then read the mixed PCM from OSTask.output_buff (RDRAM) and write a WAV.
    printf("scaffold ready: rdram=%zuMB samp=%zuB sdir=%zuB data=%zuB\n",
           rdram.size()>>20, samp.size(), sdir.size(), data.size());
    printf("TODO: construct voice/command (see comment) then call musyx_audio + dump output_buff to WAV\n");
    (void)argc; (void)argv; (void)be32;
    return 0;
}
