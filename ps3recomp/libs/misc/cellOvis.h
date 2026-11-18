/*
 * ps3recomp - cellOvis HLE
 *
 * System overlay notifications. Stub — init/term work,
 * overlay operations are no-ops.
 */

#ifndef PS3RECOMP_CELL_OVIS_H
#define PS3RECOMP_CELL_OVIS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_OVIS_ERROR_NOT_INITIALIZED     0x80410701
#define CELL_OVIS_ERROR_ALREADY_INITIALIZED 0x80410702
#define CELL_OVIS_ERROR_INVALID_ARGUMENT    0x80410703

/* Types */
typedef u32 CellOvisHandle;

/* Functions */
s32 cellOvisInit(void);
s32 cellOvisTerm(void);

/* Real libovis ABI (matches RPCS3): the argument is the guest address of an
 * SPU ELF image, and the overlay-table size is the RETURN value. 0 means the
 * image has no overlays, which is what every lifted image here is. */
s32 cellOvisGetOverlayTableSize(u32 elf);
s32 cellOvisInitializeOverlayTable(u32 ea_ovly_table, u32 elf);
s32 cellOvisCreateOverlay(const void* table, u32 tableSize, CellOvisHandle* handle);
s32 cellOvisDestroyOverlay(CellOvisHandle handle);

s32 cellOvisInvalidateOverlay(CellOvisHandle handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_OVIS_H */
