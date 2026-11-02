#pragma once

#include <PerCpu.h>
#include <Types.h>

VOID TssInitPerCpu(PerCpu *P);
VOID TssSetRsp0(PerCpu *P, UINT64 Rsp0);