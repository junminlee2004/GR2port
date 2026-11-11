// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/share_utility/shareutility.h"

// The console's share menu has no counterpart here: every call succeeds and opens nothing.
namespace Libraries::ShareUtility {

s32 PS4_SYSV_ABI sceShareUtilityInitialize() {
    LOG_DEBUG(Lib_ShareUtility, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceShareUtilityTerminate() {
    LOG_DEBUG(Lib_ShareUtility, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceShareUtilityOpenShareMenu() {
    LOG_INFO(Lib_ShareUtility, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceShareUtilityOpenShareMenuDefault() {
    LOG_INFO(Lib_ShareUtility, "called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("Jlv-lYxEnvM", "libSceShareUtility", 1, "libSceShareUtility",
                 sceShareUtilityInitialize);
    LIB_FUNCTION("DUWhxkyVPj4", "libSceShareUtility", 1, "libSceShareUtility",
                 sceShareUtilityTerminate);
    LIB_FUNCTION("Lqp4EDIRXSo", "libSceShareUtility", 1, "libSceShareUtility",
                 sceShareUtilityOpenShareMenu);
    LIB_FUNCTION("8hZ2EEl2Tto", "libSceShareUtility", 1, "libSceShareUtility",
                 sceShareUtilityOpenShareMenuDefault);
}

} // namespace Libraries::ShareUtility
