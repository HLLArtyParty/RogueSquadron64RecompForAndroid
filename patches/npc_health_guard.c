// Regen-safe overrides of the NPC health/damage accessors (game 0x800F2070..0x800F219C).
//
// Enemy health (npc[0x1A7]==0) is a per-difficulty array: npc+0x190 points at the
// array, indexed by the global difficulty slot npcHealthTableIndex (0x80137CE4):
//     value = *( *(int32_t**)(npc + 0x190) + npcHealthTableIndex )
// (npc[0x1A7]!=0 stores the health directly at npc+0x190 instead.)
//
// During structure destruction an NPC gets torn down and npc+0x190 goes
// NULL/garbage, so the computed slot address lands wildly outside RDRAM. On
// hardware the RDRAM address wraps harmlessly; in the recomp MEM_W does no KSEG0
// masking, so it AVs. For a *read* the SEH handler swallows the AV and the game
// limps on; for the *write* in dealDamageToNpc / setNpcHealth the swallowed AV
// unwinds the caller mid-update, so the damage never lands and the actor's tick
// is abandoned -> AT-PTs that won't take damage and freeze (Jade Moon), and the
// jade-moon / Tatooine attract-demo gfx-barrier freeze. Guarding only the read
// (as this file first did) leaves the writers exposed, so all five accessors
// that dereference the per-difficulty slot get the same guard here.
//
// These live in patches/ because inline edits to RecompiledFuncs/funcs_41.c are
// silently stripped by every regen. Each is in the func table
// (recomp_overlays.inl) as a symbol reference, and PatchesLib links first under
// /FORCE:MULTIPLE, so these win for both direct and LOOKUP_FUNC indirect calls.

// mips64-elf-gcc with -nostdinc — no system headers.
typedef unsigned char uint8_t;
typedef unsigned int  uint32_t;

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

// Game global at 0x80137CE4 (health-table current-slot index), bound in syms.ld.
extern volatile uint32_t npcHealthTableIndex;
// getNpcHealthPercentage scale constants (game rodata), bound in syms.ld.
extern const float healthPctScaleArray;
extern const float healthPctScaleDirect;

// True when a computed slot address is inside the 8 MB KSEG0 RDRAM window.
#define SLOT_OK(addr) (((addr) & 0xE0000000u) == 0x80000000u && ((addr) & 0x1FFFFFFFu) < 0x800000u)

RECOMP_PATCH int getNpcCurrentHealth(uint8_t* npc) {
    if (npc[0x1A7] != 0) {
        return *(int*)(npc + 0x190);
    }
    uint32_t addr = *(uint32_t*)(npc + 0x190) + (npcHealthTableIndex << 2);
    if (!SLOT_OK(addr)) {
        return 0;
    }
    return *(int*)addr;
}

RECOMP_PATCH int dealDamageToNpc(uint8_t* npc, int damage) {
    if (npc[0x1A7] != 0) {
        int v = *(int*)(npc + 0x190) - damage;
        *(int*)(npc + 0x190) = v;
        return v;
    }
    uint32_t addr = *(uint32_t*)(npc + 0x190) + (npcHealthTableIndex << 2);
    if (!SLOT_OK(addr)) {
        return 0;  // torn-down NPC: drop the damage rather than AV on the write
    }
    int v = *(int*)addr - damage;
    *(int*)addr = v;
    return v;
}

RECOMP_PATCH int setNpcHealth(uint8_t* npc, uint8_t* healthArray, uint8_t* infoStruct) {
    uint32_t addr = (uint32_t)healthArray + (npcHealthTableIndex << 2);
    int slot = SLOT_OK(addr) ? *(int*)addr : 0;
    if (npc[0x1A7] == 0) {
        *(uint32_t*)(npc + 0x190) = (uint32_t)healthArray;
        if (*(uint32_t*)(infoStruct + 0x54) == 0x80000000u) {
            *(int*)(infoStruct + 0x54) = slot;
        }
        *(int*)(npc + 0x194) = *(int*)(infoStruct + 0x54);
    } else {
        *(int*)(npc + 0x190) = slot;
        *(int*)(npc + 0x194) = slot;
    }
    return slot;
}

RECOMP_PATCH int getNpcMissingHealth(uint8_t* npc) {
    if (npc[0x1A7] != 0) {
        return *(int*)(npc + 0x194) - *(int*)(npc + 0x190);
    }
    uint32_t addr = *(uint32_t*)(npc + 0x190) + (npcHealthTableIndex << 2);
    int cur = SLOT_OK(addr) ? *(int*)addr : 0;
    return *(int*)(npc + 0x194) - cur;
}

RECOMP_PATCH float getNpcHealthPercentage(uint8_t* npc) {
    if (npc[0x1A7] != 0) {
        float cur = (float)*(int*)(npc + 0x190);
        float max = (float)*(int*)(npc + 0x194);
        return cur / max * healthPctScaleDirect;
    }
    uint32_t addr = *(uint32_t*)(npc + 0x190) + (npcHealthTableIndex << 2);
    if (!SLOT_OK(addr)) {
        return 0.0f;
    }
    float cur = (float)*(int*)addr;
    float max = (float)*(int*)(npc + 0x194);
    return cur / max * healthPctScaleArray;
}
