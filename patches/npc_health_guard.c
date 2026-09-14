// Regen-safe override of getNpcCurrentHealth (game addr 0x800F20EC).
//
// The original indexes a health table:
//     idx  = *(uint32_t*)0x80137CE4;          // current slot index
//     base = *(uint32_t*)(npc + 0x190);        // health-array base for this NPC
//     return *(int32_t*)(base + (idx << 2));
// During structure destruction the demo tears an NPC down and npc+0x190 goes
// NULL/garbage, so the final load lands wildly outside RDRAM. On hardware the
// RDRAM address wraps harmlessly; in the recomp MEM_W does no KSEG0 masking, so
// it AVs -> the SEH handler unwinds the game thread out of the in-flight
// submitGfxFrame -> its osRecvMesg on the gfx-completion barrier 0x8011A818
// never returns -> whole-game freeze (the jade-moon / Tatooine attract-demo
// freeze). Guard the load: return 0 (a dead NPC's health is inconsequential)
// when the computed address is outside the 8 MB RDRAM window.
//
// This lived as an inline edit in RecompiledFuncs/funcs_41.c but was silently
// stripped by a regen TWICE. It belongs here so it survives regeneration:
// getNpcCurrentHealth is in the func table (recomp_overlays.inl) as a symbol
// reference, and PatchesLib is linked first under /FORCE:MULTIPLE, so this
// definition wins for BOTH direct calls and LOOKUP_FUNC(0x800F20EC) indirect
// calls (func_map[0x800F20EC] = &getNpcCurrentHealth resolves to this patch).

// mips64-elf-gcc with -nostdinc — no system headers.
typedef unsigned char uint8_t;
typedef unsigned int  uint32_t;

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

// Game global at 0x80137CE4 (health-table current-slot index), bound in syms.ld.
extern volatile uint32_t npcHealthTableIndex;

RECOMP_PATCH int getNpcCurrentHealth(uint8_t* npc) {
    // 0x1A7 flag set -> the field at npc+0x190 IS the health value (return it directly).
    if (npc[0x1A7] != 0) {
        return *(int*)(npc + 0x190);
    }
    uint32_t base = *(uint32_t*)(npc + 0x190);
    uint32_t addr = base + (npcHealthTableIndex << 2);
    // Skip the load when the address is not a valid KSEG0 RDRAM address.
    if ((addr & 0xE0000000u) != 0x80000000u || (addr & 0x1FFFFFFFu) >= 0x800000u) {
        return 0;
    }
    return *(int*)addr;
}
