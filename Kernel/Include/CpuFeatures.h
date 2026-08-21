#pragma once

#include <Types.h>

INT CpuEnableSmepSmap(VOID);
INT CpuEnableUmip(VOID);
BOOL CpuHasAvx(VOID);
VOID CpuEnableXsave(VOID);