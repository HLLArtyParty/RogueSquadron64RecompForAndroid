/* Symbol bindings for patches. Two kinds of entries:
 *
 *  1. Game-side functions/data the patches need to reference. These resolve
 *     to addresses in the original ROM. Add as we need them.
 *
 *  2. Runtime stubs at fake absolute addresses (0x8FXXXXXX). N64Recomp
 *     recognises these and emits direct calls into librecomp/host code.
 */

__start = 0x80000000;

/* --- Runtime stubs (host code callable from patches) -------------------- */
/* (none yet — populate when we need them) */

/* --- Game functions called from patches --------------------------------- */
/* heap_guards.c (currently DISABLED -> patches/disabled/) calls
 * findAndUnlinkSmallestEntry. NOTE: an absolute address here is required for ld
 * to emit the jal, but then N64Recomp fails to resolve it ("No function found
 * for jal target 0x8002221C") -- a patches-recompile limitation for patch->game
 * function calls that is unresolved. Kept for when that's fixed + heap_guards
 * is re-enabled. See plans/wire-register-patches note. */
findAndUnlinkSmallestEntry = 0x8002221C;

/* --- Game data symbols touched by patches ------------------------------- */
/* heap_guards.c reads/writes these globals. Addresses are static RAM slots
 * in the original game's BSS.
 *
 *  heap_free_head      (Block*)   the head of the heap's free list
 *  heap_other_listhead (Block**)  pointer to the active-queue head slot
 *  heap_tail_loc       (Block**)  pointer to current tail-node slot
 *  heap_bump           (void*)    bump-allocator pointer (popped + 0x100)
 */
heap_free_head      = 0x801163B0;
heap_other_listhead = 0x801163FC;
heap_tail_loc       = 0x8011A7DC;
heap_bump           = 0x801163D4;

/* npc_health_guard.c reads the health-table current-slot index. */
npcHealthTableIndex = 0x80137CE4;

/* npc_health_guard.c getNpcHealthPercentage scale constants (game rodata):
 *   ScaleArray  -> the array-health (npc[0x1A7]==0) path
 *   ScaleDirect -> the direct-value (npc[0x1A7]!=0) path */
healthPctScaleArray  = 0x800A89D8;
healthPctScaleDirect = 0x800A89DC;
