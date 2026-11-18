/*
 * ps3recomp - cellOvis HLE implementation
 *
 * Stub. Init/term work, overlay creation succeeds as no-op.
 */

#include "cellOvis.h"
#include <stdio.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */

/* Internal state */

static int s_initialized = 0;
static u32 s_next_handle = 1;

/* API */

s32 cellOvisInit(void)
{
    printf("[cellOvis] Init()\n");
    if (s_initialized)
        return (s32)CELL_OVIS_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    s_next_handle = 1;
    return CELL_OK;
}

s32 cellOvisTerm(void)
{
    printf("[cellOvis] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellOvisGetOverlayTableSize(u32 elf)
{
    /* No Init() is required on hardware: titles (Fat Princess's PhysX) call
     * this without cellOvisInit and treat any nonzero result as a table size
     * to allocate. The old (filePath, u32*) form returned NOT_INITIALIZED here,
     * PhysX tried to allocate 0x8041xxxx bytes, failed, and quit the game. */
    static int _n = 0;
    if (_n++ < 4) printf("[cellOvis] GetOverlayTableSize(elf=0x%08X) -> 0 (no overlays)\n", elf);
    return 0;
}

s32 cellOvisInitializeOverlayTable(u32 ea_ovly_table, u32 elf)
{
    static int _n = 0;
    if (_n++ < 4) printf("[cellOvis] InitializeOverlayTable(table=0x%08X, elf=0x%08X)\n", ea_ovly_table, elf);
    return CELL_OK;
}

s32 cellOvisCreateOverlay(const void* table, u32 tableSize, CellOvisHandle* handle)
{
    (void)table; (void)tableSize;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    if (!handle) return (s32)CELL_OVIS_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)handle, (u32)s_next_handle++);
    return CELL_OK;
}

s32 cellOvisDestroyOverlay(CellOvisHandle handle)
{
    (void)handle;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellOvisInvalidateOverlay(CellOvisHandle handle)
{
    (void)handle;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}
