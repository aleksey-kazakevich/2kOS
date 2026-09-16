#pragma once

#include <Types.h>

INT CpuEnableSmep(VOID);
INT CpuEnableUmip(VOID);
BOOL CpuHasAvx(VOID);
VOID CpuEnableXsave(VOID);